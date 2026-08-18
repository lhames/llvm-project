//===-- LLVMUserExpression.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/LLVMUserExpression.h"
#include "lldb/Core/Module.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/ExpressionVariable.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Expression/Materializer.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/ErrorMessages.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"

using namespace lldb;
using namespace lldb_private;

char LLVMUserExpression::ID;

LLVMUserExpression::LLVMUserExpression(ExecutionContextScope &exe_scope,
                                       llvm::StringRef expr,
                                       llvm::StringRef prefix,
                                       SourceLanguage language,
                                       ResultType desired_type,
                                       const EvaluateExpressionOptions &options)
    : UserExpression(exe_scope, expr, prefix, language, desired_type, options),
      m_allow_cxx(false), m_allow_objc(false), m_transformed_text(),
      m_execution_unit_sp(), m_materializer_up(), m_jit_module_wp(),
      m_target(nullptr), m_materialized_address(LLDB_INVALID_ADDRESS) {}

bool LLVMUserExpression::CanInterpret() {
  return m_execution_unit_sp && m_execution_unit_sp->WillInterpret();
}

LLVMUserExpression::~LLVMUserExpression() {
  if (m_target) {
    lldb::ModuleSP jit_module_sp(m_jit_module_wp.lock());
    if (jit_module_sp)
      m_target->GetImages().Remove(jit_module_sp);
  }
}

lldb::ExpressionResults
LLVMUserExpression::DoExecute(DiagnosticManager &diagnostic_manager,
                              ExecutionContext &exe_ctx,
                              const EvaluateExpressionOptions &options,
                              lldb::UserExpressionSP &shared_ptr_to_me,
                              lldb::ExpressionVariableSP &result_sp) {
  if (m_jit_start_addr == LLDB_INVALID_ADDRESS && !CanInterpret()) {
    diagnostic_manager.PutString(
        lldb::eSeverityError,
        "Expression can't be run, because there is no JIT compiled function");
    return lldb::eExpressionSetupError;
  }

  lldb::addr_t struct_address = LLDB_INVALID_ADDRESS;

  if (!PrepareToExecuteJITExpression(diagnostic_manager, exe_ctx,
                                     struct_address)) {
    diagnostic_manager.Printf(
        lldb::eSeverityError,
        "errored out in %s, couldn't PrepareToExecuteJITExpression",
        __FUNCTION__);
    return lldb::eExpressionSetupError;
  }

  std::vector<lldb::addr_t> args;

  if (!AddArguments(exe_ctx, args, struct_address, diagnostic_manager)) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "errored out in %s, couldn't AddArguments",
                              __FUNCTION__);
    return lldb::eExpressionSetupError;
  }

  lldb::addr_t function_stack_bottom = LLDB_INVALID_ADDRESS;
  lldb::addr_t function_stack_top = LLDB_INVALID_ADDRESS;

  lldb::ExpressionResults execution_result = m_execution_unit_sp->Run(
      args, exe_ctx, options, diagnostic_manager, shared_ptr_to_me,
      function_stack_bottom, function_stack_top);

  if (execution_result != lldb::eExpressionCompleted)
    return execution_result;

  if (FinalizeJITExecution(diagnostic_manager, exe_ctx, result_sp,
                           function_stack_bottom, function_stack_top))
    return lldb::eExpressionCompleted;

  return lldb::eExpressionResultUnavailable;
}

bool LLVMUserExpression::FinalizeJITExecution(
    DiagnosticManager &diagnostic_manager, ExecutionContext &exe_ctx,
    lldb::ExpressionVariableSP &result, lldb::addr_t function_stack_bottom,
    lldb::addr_t function_stack_top) {
  Log *log = GetLog(LLDBLog::Expressions);

  LLDB_LOGF(log, "-- [UserExpression::FinalizeJITExecution] Dematerializing "
                 "after execution --");

  if (!m_dematerializer_sp) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't apply expression side effects : no "
                              "dematerializer is present");
    return false;
  }

  Status dematerialize_error;

  m_dematerializer_sp->Dematerialize(dematerialize_error, function_stack_bottom,
                                     function_stack_top);

  if (!dematerialize_error.Success()) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't apply expression side effects : %s",
                              dematerialize_error.AsCString("unknown error"));
    return false;
  }

  result =
      GetResultAfterDematerialization(exe_ctx.GetBestExecutionContextScope());

  if (result) {
    // TransferAddress also does the offset_to_top calculation, so record the
    // dynamic option before we do that.
    if (EvaluateExpressionOptions *options = GetOptions())
      result->PreserveDynamicOption(options->GetUseDynamic());
    result->TransferAddress();
  }

  m_dematerializer_sp.reset();

  return true;
}

bool LLVMUserExpression::PrepareToExecuteJITExpression(
    DiagnosticManager &diagnostic_manager, ExecutionContext &exe_ctx,
    lldb::addr_t &struct_address) {
  lldb::TargetSP target;
  lldb::ProcessSP process;
  lldb::StackFrameSP frame;

  if (!LockAndCheckContext(exe_ctx, target, process, frame)) {
    diagnostic_manager.PutString(
        lldb::eSeverityError,
        "The context has changed before we could JIT the expression!");
    return false;
  }

  if (m_jit_start_addr != LLDB_INVALID_ADDRESS || CanInterpret()) {
    if (!AllocateArgumentStruct(diagnostic_manager, struct_address))
      return false;

    if (!m_execution_unit_sp->PrepareToRun(diagnostic_manager, *target,
                                           process.get()))
      return false;

    Status materialize_error;

    m_dematerializer_sp = m_materializer_up->Materialize(
        frame, *m_execution_unit_sp, struct_address, materialize_error);

    if (!materialize_error.Success()) {
      diagnostic_manager.Printf(lldb::eSeverityError,
                                "Couldn't materialize: %s",
                                materialize_error.AsCString());
      return false;
    }
  }
  return true;
}

bool LLVMUserExpression::AllocateArgumentStruct(
    DiagnosticManager &diagnostic_manager, lldb::addr_t &struct_address) {
  if (m_materialized_address == LLDB_INVALID_ADDRESS) {
    IRMemoryMap::AllocationPolicy policy =
        CanInterpret() ? IRMemoryMap::eAllocationPolicyHostOnly
                        : IRMemoryMap::eAllocationPolicyMirror;

    const bool zero_memory = false;
    if (auto address_or_error = m_execution_unit_sp->Malloc(
            m_materializer_up->GetStructByteSize(),
            m_materializer_up->GetStructAlignment(),
            lldb::ePermissionsReadable | lldb::ePermissionsWritable, policy,
            zero_memory)) {
      m_materialized_address = *address_or_error;
    } else {
      diagnostic_manager.Printf(
          lldb::eSeverityError,
          "Couldn't allocate space for materialized struct: %s",
          toString(address_or_error.takeError()).c_str());
      return false;
    }
  }

  struct_address = m_materialized_address;
  return true;
}
