//===-- JITExecutionUnit.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_EXPRESSION_JITEXECUTIONUNIT_H
#define LLDB_EXPRESSION_JITEXECUTIONUNIT_H

#include "lldb/Expression/IRExecutionUnit.h"

namespace lldb_private {

/// \class JITExecutionUnit JITExecutionUnit.h
/// "lldb/Expression/JITExecutionUnit.h" An execution unit that evaluates its
/// expression by running JIT-compiled code in the target.
///
/// The IR is compiled to machine code, the resulting sections are copied into
/// the target process, and the expression's wrapper function is called there
/// via a thread plan. Unlike interpretation this requires a running process,
/// and it leaves code behind in the target: see GetExportedSymbols and
/// NeedsToOutliveExpression.
///
// TODO: The JIT machinery still lives on IRExecutionUnit. Moving it down here
// is the next step; this class currently only supplies the mechanism.
class JITExecutionUnit : public IRExecutionUnit {
public:
  using IRExecutionUnit::IRExecutionUnit;

  bool WillInterpret() const override { return false; }

  lldb::ExpressionResults Run(llvm::ArrayRef<lldb::addr_t> args,
                              ExecutionContext &exe_ctx,
                              const EvaluateExpressionOptions &options,
                              DiagnosticManager &diagnostic_manager,
                              lldb::UserExpressionSP &expression_sp,
                              lldb::addr_t &function_stack_bottom,
                              lldb::addr_t &function_stack_top) override;
};

} // namespace lldb_private

#endif // LLDB_EXPRESSION_JITEXECUTIONUNIT_H
