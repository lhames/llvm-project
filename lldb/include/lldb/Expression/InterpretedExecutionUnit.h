//===-- InterpretedExecutionUnit.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_EXPRESSION_INTERPRETEDEXECUTIONUNIT_H
#define LLDB_EXPRESSION_INTERPRETEDEXECUTIONUNIT_H

#include "lldb/Expression/IRExecutionUnit.h"

namespace lldb_private {

/// \class InterpretedExecutionUnit InterpretedExecutionUnit.h
/// "lldb/Expression/InterpretedExecutionUnit.h" An execution unit that
/// evaluates its expression by interpreting the IR directly.
///
/// Nothing is written into the target: the IR is walked by IRInterpreter, and
/// the memory the expression needs comes from this unit's own IRMemoryMap,
/// allocated host-only. That is why an interpreted expression can be evaluated
/// with no running process.
class InterpretedExecutionUnit : public IRExecutionUnit {
public:
  using IRExecutionUnit::IRExecutionUnit;

  bool WillInterpret() const override { return true; }

  /// Allocate the interpreter's private, host-only scratch stack, if one
  /// hasn't already been allocated. Idempotent.
  bool PrepareToRun(DiagnosticManager &diagnostic_manager, Target &target,
                    Process *process) override;

  lldb::ExpressionResults Run(llvm::ArrayRef<lldb::addr_t> args,
                              ExecutionContext &exe_ctx,
                              const EvaluateExpressionOptions &options,
                              DiagnosticManager &diagnostic_manager,
                              lldb::UserExpressionSP &expression_sp,
                              lldb::addr_t &function_stack_bottom,
                              lldb::addr_t &function_stack_top) override;

private:
  /// The interpreter's private, host-only scratch stack.
  lldb::addr_t m_stack_bottom = LLDB_INVALID_ADDRESS;
  lldb::addr_t m_stack_top = LLDB_INVALID_ADDRESS;
};

} // namespace lldb_private

#endif // LLDB_EXPRESSION_INTERPRETEDEXECUTIONUNIT_H
