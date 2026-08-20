//===-- ExpressionSymbolResolver.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_EXPRESSION_EXPRESSIONSYMBOLRESOLVER_H
#define LLDB_EXPRESSION_EXPRESSIONSYMBOLRESOLVER_H

#include <vector>

#include "llvm/ADT/ArrayRef.h"

#include "lldb/Core/ModuleList.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"

namespace lldb_private {

/// \class ExpressionSymbolResolver ExpressionSymbolResolver.h
/// "lldb/Expression/ExpressionSymbolResolver.h" Resolves the symbols an
/// expression refers to but does not define, to load addresses in the target.
///
/// Expression IR refers to functions and variables by name. Before that IR can
/// be interpreted or JITted, each of those names has to be turned into a load
/// address. That is not a simple lookup: the name may be an LLDB function call
/// label, it may need alternate manglings tried, and it may be satisfied by a
/// language runtime rather than by a module's symbol table.
///
/// Names can also be satisfied by symbols that *earlier* expressions defined
/// and published, which is how one expression calls a function another one
/// declared. Those are reached through the target's persistent symbols.
///
/// This class encapsulates that policy. It is independent of how the
/// expression will be evaluated -- IRForTarget resolves symbols while
/// rewriting the IR, before either mechanism has been chosen, and interpreted
/// IR needs addresses just as JITted code does.

class ExpressionSymbolResolver {
public:
  /// Constructor
  ///
  /// \param[in] sym_ctx
  ///     The symbol context to look up in. Its module, when set, is searched
  ///     ahead of all others.
  ///
  /// \param[in] strip_underscore
  ///     True on platforms where global symbols carry a leading underscore, in
  ///     which case a name with that underscore removed is also tried.
  ExpressionSymbolResolver(const SymbolContext &sym_ctx, bool strip_underscore);

  /// Add modules to be searched before any others, except for the module
  /// belonging to the symbol context this was constructed with.
  void AppendPreferredModules(SymbolContextList const &contexts);

  /// Find the load address of \p name in the target.
  ///
  /// \param[out] missing_weak
  ///     Set when the name resolved to a weak symbol that is legitimately
  ///     absent. Callers must treat the result as a null pointer rather than
  ///     as a failure: testing a weak symbol's address against NULL is how its
  ///     presence is detected.
  ///
  /// \return
  ///     The load address, or LLDB_INVALID_ADDRESS if it could not be found.
  lldb::addr_t FindSymbol(ConstString name, bool &missing_weak);

private:
  /// Search, in order: functions then symbols, each time trying the symbol
  /// context's own module, then the preferred modules, then everything else
  /// the platform allows to be searched without constraint.
  lldb::addr_t FindInSymbols(llvm::ArrayRef<ConstString> names,
                             bool &symbol_was_missing_weak);

  /// Ask each of the process's language runtimes for the symbol.
  lldb::addr_t FindInRuntimes(llvm::ArrayRef<ConstString> names);

  /// Look the symbol up among those defined by previous expressions.
  lldb::addr_t FindInUserDefinedSymbols(llvm::ArrayRef<ConstString> names);

  /// Build the C candidate names for \p name: the name itself, preceded by the
  /// name with its leading underscore removed where that applies.
  void CollectCandidateCNames(std::vector<ConstString> &C_names,
                              ConstString name);

  /// Build alternate C++ manglings to try once the C candidates have failed.
  void CollectCandidateCPlusPlusNames(std::vector<ConstString> &CPP_names,
                                      llvm::ArrayRef<ConstString> C_names);

  /// Used for symbol lookups.
  SymbolContext m_sym_ctx;

  /// Any module in this list is used for symbol and function lookup before any
  /// other module, except for the one corresponding to the current frame.
  ModuleList m_preferred_modules;

  /// True for platforms where global symbols have a _ prefix.
  bool m_strip_underscore;
};

} // namespace lldb_private

#endif // LLDB_EXPRESSION_EXPRESSIONSYMBOLRESOLVER_H
