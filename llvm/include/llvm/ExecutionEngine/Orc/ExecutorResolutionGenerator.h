//===- ExecutorResolutionGenerator.h - Resolve syms in executor -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Resolves symbols in the executor.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_EXECUTORRESOLUTIONGENERATOR_H
#define LLVM_EXECUTIONENGINE_ORC_EXECUTORRESOLUTIONGENERATOR_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"

namespace llvm::orc {

class ExecutorResolutionGenerator : public DefinitionGenerator {
public:
  using CreateAbsoluteSymsFn =
    unique_function<std::unique_ptr<MaterializationUnit>(SymbolMap)>;

  ExecutorResolutionGenerator(
      ExecutionSession &ES, ExecutorSymbolDef ResolverObj,
      ExecutorSymbolDef ResolverFunc,
      CreateAbsoluteSymsFn CreateAbsoluteSyms = absoluteSymbols)
    : ES(ES), ResolverObj(ResolverObj), ResolverFunc(ResolverFunc),
      CreateAbsoluteSyms(std::move(CreateAbsoluteSyms)) {}

  Error tryToGenerate(LookupState &LS, LookupKind K, JITDylib &JD,
                      JITDylibLookupFlags JDLookupFlags,
                      const SymbolLookupSet &LookupSet) override;
private:

  ExecutionSession &ES;
  ExecutorSymbolDef ResolverObj;
  ExecutorSymbolDef ResolverFunc;
  CreateAbsoluteSymsFn CreateAbsoluteSyms;
};

} // namespace llvm::orc

#endif // LLVM_EXECUTIONENGINE_ORC_EXECUTORRESOLUTIONGENERATOR_H
