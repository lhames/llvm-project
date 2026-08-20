//===-- IRExecutionUnit.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_EXPRESSION_IREXECUTIONUNIT_H
#define LLDB_EXPRESSION_IREXECUTIONUNIT_H

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/Module.h"

#include "lldb/Core/ModuleList.h"
#include "lldb/Expression/ExpressionSymbolResolver.h"
#include "lldb/Expression/IRMemoryMap.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"

namespace llvm {

class Module;

} // namespace llvm

namespace lldb_private {

class Status;

/// \class IRExecutionUnit IRExecutionUnit.h
/// "lldb/Expression/IRExecutionUnit.h" Holds an expression's IR, and knows how
/// to evaluate it.
///
/// This class wraps the IR module that comes from the expression parser,
/// together with the memory the expression needs and the resolver that turns
/// the names it refers to into addresses in the target.
///
/// How the expression is actually evaluated is left to a subclass:
/// InterpretedExecutionUnit walks the IR, and JITExecutionUnit compiles it and
/// runs the result in the target. The parser picks between them once it knows
/// whether the IR can be interpreted.
class IRExecutionUnit : public std::enable_shared_from_this<IRExecutionUnit>,
                        public IRMemoryMap {
public:
  /// Constructor
  IRExecutionUnit(std::unique_ptr<llvm::LLVMContext> &context_up,
                  std::unique_ptr<llvm::Module> &module_up, ConstString &name,
                  const lldb::TargetSP &target_sp,
                  ExpressionSymbolResolver symbol_resolver,
                  std::vector<std::string> &cpu_features);

  /// Destructor
  virtual ~IRExecutionUnit();

  ConstString GetFunctionName() { return m_name; }

  llvm::Module *GetModule() { return m_module; }

  llvm::Function *GetFunction() {
    return ((m_module != nullptr) ? m_module->getFunction(m_name.GetStringRef())
                                  : nullptr);
  }

  /// True if this expression will be evaluated by interpreting its IR, false
  /// if it will be evaluated by running JITted code in the target.
  virtual bool WillInterpret() const = 0;

  /// Do whatever this unit needs before it can Run. Called once the
  /// expression's arguments have been allocated but before they are
  /// materialized.
  virtual bool PrepareToRun(DiagnosticManager &diagnostic_manager,
                            Target &target, Process *process) {
    return true;
  }

  /// Run the expression, by interpreting its IR or by running its JITted code
  /// in the target, whichever this unit was prepared for.
  ///
  /// \param[in] args
  ///     The already-built argument list for the expression's wrapper
  ///     function, including the address of the materialized struct.
  ///
  /// \param[in] expression_sp
  ///     Keeps the expression alive for as long as the thread plan that runs
  ///     it, and receives the plan's callbacks. Unused when interpreting.
  ///
  /// \param[out] function_stack_bottom
  /// \param[out] function_stack_top
  ///     The bounds of the stack that the expression's frame occupied, which
  ///     the caller needs in order to dematerialize its results.
  ///
  /// \return
  ///     eExpressionCompleted on success.
  //
  // TODO: Revisit the interaction between materialization, running and
  // dematerialization, and discuss with the community before going further.
  //
  // Two things are unresolved here. First, the stack bounds reported above are
  // internal state for the interpreted case but are derived at run time from
  // the thread plan's stack pointer for the JITted one. Second, and the reason
  // expression_sp has to be passed at all, there are two paths that finalize
  // an expression: normally LLVMUserExpression::DoExecute dematerializes once
  // Run returns, but when the expression hits a user breakpoint and is left
  // running in the target, DoExecute returns early and the thread plan takes
  // over -- see ThreadPlanCallUserExpression::MischiefManaged, which calls
  // FinalizeJITExecution on the expression and stashes the result variable so
  // it can still be reported later.
  //
  // For the thread plan to hold only an execution unit, both dematerialization
  // and the production of that result variable would have to move here. The
  // latter is language-specific -- UserExpression::GetResultAfterDematerializa-
  // tion is virtual -- so it likely needs a narrower callback interface rather
  // than a straight move.
  virtual lldb::ExpressionResults Run(llvm::ArrayRef<lldb::addr_t> args,
                                      ExecutionContext &exe_ctx,
                                      const EvaluateExpressionOptions &options,
                                      DiagnosticManager &diagnostic_manager,
                                      lldb::UserExpressionSP &expression_sp,
                                      lldb::addr_t &function_stack_bottom,
                                      lldb::addr_t &function_stack_top) = 0;

  /// The resolver for symbols this expression refers to but does not define.
  ///
  /// Both mechanisms need this. Interpreted IR can name a function just as
  /// JITted code can, and either way the name has to become a real address in
  /// the target before the expression can use it. Contrast GetExportedSymbols,
  /// which is about the symbols an expression leaves behind, and which only a
  /// unit that wrote code into the target can have.
  ExpressionSymbolResolver &GetExternalSymbolResolver() {
    return m_symbol_resolver;
  }

  /// True if this unit has to outlive the expression that created it, because
  /// an expression result may hold an address that points into code it emitted.
  /// Only a unit that emitted code into the target can need this.
  virtual bool NeedsToOutliveExpression() const { return false; }

  /// Report the symbols this unit exported into the target, and the addresses
  /// they ended up at, to \p callback. A unit that wrote nothing into the
  /// target exports nothing.
  virtual void GetExportedSymbols(
      llvm::function_ref<void(ConstString, lldb::addr_t)> callback) {}

  /// An lldb Module describing the code this unit emitted into the target, for
  /// debug info purposes, or null if it emitted none.
  virtual lldb::ModuleSP GetJITModule() { return nullptr; }

  /// The addresses of this unit's static initializers in the target. A unit
  /// that emitted no code into the target has none that can be called.
  //
  // TODO: Running these is arguably this class's business rather than
  // ExpressionParser::RunStaticInitializers'. Raised for discussion.
  virtual void
  GetStaticInitializers(std::vector<lldb::addr_t> &static_initializers) {}

protected:
  std::unique_ptr<llvm::LLVMContext> m_context_up;
  std::unique_ptr<llvm::Module>
      m_module_up;        ///< Holder for the module until it's been handed off
  llvm::Module *m_module; ///< Owned by the execution engine once JITted
  std::vector<std::string> m_cpu_features;
  const ConstString m_name;
  ExpressionSymbolResolver m_symbol_resolver;
};

} // namespace lldb_private

#endif // LLDB_EXPRESSION_IREXECUTIONUNIT_H
