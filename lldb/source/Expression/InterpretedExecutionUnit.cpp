//===-- InterpretedExecutionUnit.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/InterpretedExecutionUnit.h"

#include "llvm/IR/Module.h"

#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Expression/IRInterpreter.h"
#include "lldb/Target/ABI.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ErrorMessages.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/lldb-defines.h"

using namespace lldb_private;

bool InterpretedExecutionUnit::PrepareToRun(
    DiagnosticManager &diagnostic_manager, Target &target, Process *process) {
  if (m_stack_bottom != LLDB_INVALID_ADDRESS)
    return true;

  size_t stack_frame_size = target.GetExprAllocSize();
  if (stack_frame_size == 0) {
    lldb::ABISP abi_sp;
    if (process && (abi_sp = process->GetABI()))
      stack_frame_size = abi_sp->GetStackFrameSize();
    else
      stack_frame_size = 512 * 1024;
  }

  const bool zero_memory = false;
  if (auto address_or_error =
          Malloc(stack_frame_size, 8,
                 lldb::ePermissionsReadable | lldb::ePermissionsWritable,
                 IRMemoryMap::eAllocationPolicyHostOnly, zero_memory)) {
    m_stack_bottom = *address_or_error;
    m_stack_top = m_stack_bottom + stack_frame_size;
    return true;
  } else {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't allocate space for the stack frame: %s",
                              toString(address_or_error.takeError()).c_str());
    return false;
  }
}

lldb::ExpressionResults InterpretedExecutionUnit::Run(
    llvm::ArrayRef<lldb::addr_t> args, ExecutionContext &exe_ctx,
    const EvaluateExpressionOptions &options,
    DiagnosticManager &diagnostic_manager,
    lldb::UserExpressionSP &expression_sp, lldb::addr_t &function_stack_bottom,
    lldb::addr_t &function_stack_top) {
  llvm::Module *module = GetModule();
  llvm::Function *function = GetFunction();

  if (!module || !function) {
    diagnostic_manager.PutString(lldb::eSeverityError,
                                 "supposed to interpret, but nothing is there");
    return lldb::eExpressionSetupError;
  }

  Status interpreter_error;

  function_stack_bottom = m_stack_bottom;
  function_stack_top = m_stack_top;

  IRInterpreter::Interpret(*module, *function, args, *this, interpreter_error,
                           function_stack_bottom, function_stack_top, exe_ctx,
                           options.GetTimeout());

  if (!interpreter_error.Success()) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "supposed to interpret, but failed: %s",
                              interpreter_error.AsCString());
    return lldb::eExpressionDiscarded;
  }

  return lldb::eExpressionCompleted;
}
