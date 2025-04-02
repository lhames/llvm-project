//===----- ExecutorResolutionGenerator.cpp - Resolve syms in executor -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/ExecutorResolutionGenerator.h"

#include "llvm/ExecutionEngine/Orc/DebugUtils.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"

#define DEBUG_TYPE "orc"

using namespace llvm::orc::shared;

namespace llvm::orc {

Error ExecutorResolutionGenerator::tryToGenerate(
    LookupState &LS, LookupKind K, JITDylib &JD,
    JITDylibLookupFlags JDLookupFlags,
    const SymbolLookupSet &LookupSet) {

  assert(!LookupSet.empty() && "Request for no symbols?");

  LLVM_DEBUG({
    dbgs() << "Executor-resolution (resolver obj = " << ResolverObj.getAddress()
           << ", func = " << ResolverFunc.getAddress() << "): [ ";
    for (auto &[Name, Flags] : LookupSet)
      dbgs() << "(" << Name
             << (Flags == SymbolLookupFlags::WeaklyReferencedSymbol
                 ? " (weak)" : "") << ") ";
      dbgs() << "]\n";
  });

  using ExcetorResolveSPSSig =
    SPSSequence<SPSOptional<SPSExecutorSymbolDef>>(
        SPSExecutorSymbolDef, SPSSequence<SPSString>);

  // We need to hand LookupSet off to the handler lambda and also use it as an
  // argument to the call. Create a lightweight copy.
  std::vector<StringRef> LookupSeq;
  LookupSeq.reserve(LookupSet.size());
  for (auto &[Name, _] : LookupSet)
    LookupSeq.push_back(*Name);

  ES.callSPSWrapperAsync<ExcetorResolveSPSSig>(
      ResolverFunc.getAddress(),
      [this, LS = std::move(LS), JD = JITDylibSP(&JD), LookupSet]
      (Error SerializationError,
       std::vector<std::optional<ExecutorSymbolDef>> Syms) mutable {
        if (SerializationError)
          return LS.continueLookup(std::move(SerializationError));

        size_t SymIdx = 0;
        SymbolNameSet MissingSymbols;
        SymbolMap Result;
        for (auto &[Name, Flags] : LookupSet) {
          auto Sym = Syms[SymIdx++];

          if (LLVM_UNLIKELY(!Sym && Flags == SymbolLookupFlags::RequiredSymbol))
            MissingSymbols.insert(Name);
          else
            Result[Name] = Sym ? *Sym : ExecutorSymbolDef();
        }

        if (LLVM_UNLIKELY(!MissingSymbols.empty()))
          return LS.continueLookup(
              make_error<SymbolsNotFound>(ES.getSymbolStringPool(),
                                          std::move(MissingSymbols)));

        LS.continueLookup(JD->define(CreateAbsoluteSyms(std::move(Result))));
      }, ResolverObj, LookupSeq);

  return Error::success();
}

} // namespace llvm::orc
