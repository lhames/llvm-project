//===-- JITExecutionUnit.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/JITExecutionUnit.h"

#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Target/ThreadPlanCallUserExpression.h"
#include "lldb/Utility/ErrorMessages.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Policy.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/lldb-defines.h"

using namespace lldb_private;

lldb::ExpressionResults JITExecutionUnit::Run(
    llvm::ArrayRef<lldb::addr_t> args, ExecutionContext &exe_ctx,
    const EvaluateExpressionOptions &options,
    DiagnosticManager &diagnostic_manager,
    lldb::UserExpressionSP &expression_sp, lldb::addr_t &function_stack_bottom,
    lldb::addr_t &function_stack_top) {
  // The expression log is quite verbose, and if you're just tracking the
  // execution of the expression, it's quite convenient to have these logs come
  // out with the STEP log as well.
  Log *log(GetLog(LLDBLog::Expressions | LLDBLog::Step));

  if (!exe_ctx.HasThreadScope()) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "%s called with no thread selected",
                              __FUNCTION__);
    return lldb::eExpressionSetupError;
  }

  // Store away the thread ID for error reporting, in case it exits
  // during execution:
  lldb::tid_t expr_thread_id = exe_ctx.GetThreadRef().GetID();

  Address wrapper_address(m_function_load_addr);

  lldb::ThreadPlanSP call_plan_sp(new ThreadPlanCallUserExpression(
      exe_ctx.GetThreadRef(), wrapper_address, args, options, expression_sp));

  StreamString ss;
  if (!call_plan_sp || !call_plan_sp->ValidatePlan(&ss)) {
    diagnostic_manager.PutString(lldb::eSeverityError, ss.GetString());
    return lldb::eExpressionSetupError;
  }

  ThreadPlanCallUserExpression *user_expression_plan =
      static_cast<ThreadPlanCallUserExpression *>(call_plan_sp.get());

  lldb::addr_t function_stack_pointer =
      user_expression_plan->GetFunctionStackPointer();

  function_stack_bottom = function_stack_pointer - HostInfo::GetPageSize();
  function_stack_top = function_stack_pointer;

  LLDB_LOGF(log,
            "-- [UserExpression::Execute] Execution of expression begins --");

  if (exe_ctx.GetProcessPtr())
    exe_ctx.GetProcessPtr()->SetRunningUserExpression(true);

  PolicyStack::Guard expr_policy_guard =
      PolicyStack::Get().PushPublicStateRunningExpression();

  lldb::ExpressionResults execution_result =
      exe_ctx.GetProcessRef().RunThreadPlan(exe_ctx, call_plan_sp, options,
                                            diagnostic_manager);

  if (exe_ctx.GetProcessPtr())
    exe_ctx.GetProcessPtr()->SetRunningUserExpression(false);

  LLDB_LOGF(log, "-- [UserExpression::Execute] Execution of expression "
                 "completed --");

  if (execution_result == lldb::eExpressionInterrupted ||
      execution_result == lldb::eExpressionHitBreakpoint) {
    const char *error_desc = nullptr;
    const char *explanation = execution_result == lldb::eExpressionInterrupted
                                  ? "was interrupted"
                                  : "hit a breakpoint";

    if (user_expression_plan) {
      if (auto real_stop_info_sp = user_expression_plan->GetRealStopInfo())
        error_desc = real_stop_info_sp->GetDescription();
    }

    if (error_desc)
      diagnostic_manager.Printf(lldb::eSeverityError,
                                "Expression execution %s: %s.", explanation,
                                error_desc);
    else
      diagnostic_manager.Printf(lldb::eSeverityError,
                                "Expression execution %s.", explanation);

    if ((execution_result == lldb::eExpressionInterrupted &&
         options.DoesUnwindOnError()) ||
        (execution_result == lldb::eExpressionHitBreakpoint &&
         options.DoesIgnoreBreakpoints()))
      diagnostic_manager.AppendMessageToDiagnostic(
          "The process has been returned to the state before expression "
          "evaluation.");
    else {
      if (execution_result == lldb::eExpressionHitBreakpoint)
        user_expression_plan->TransferExpressionOwnership();
      diagnostic_manager.AppendMessageToDiagnostic(
          "The process has been left at the point where it was "
          "interrupted, use \"thread return -x\" to return to the state "
          "before expression evaluation.");
    }

    return execution_result;
  }

  if (execution_result == lldb::eExpressionStoppedForDebug) {
    diagnostic_manager.PutString(
        lldb::eSeverityInfo,
        "Expression execution was halted at the first instruction of the "
        "expression function because \"debug\" was requested.\n"
        "Use \"thread return -x\" to return to the state before expression "
        "evaluation.");
    return execution_result;
  }

  if (execution_result == lldb::eExpressionThreadVanished) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't execute expression: the thread on "
                              "which the expression was being run (0x%" PRIx64
                              ") exited during its execution.",
                              expr_thread_id);
    return execution_result;
  }

  if (execution_result != lldb::eExpressionCompleted) {
    diagnostic_manager.Printf(lldb::eSeverityError,
                              "Couldn't execute expression: result was %s",
                              toString(execution_result).c_str());
    return execution_result;
  }

  return lldb::eExpressionCompleted;
}
