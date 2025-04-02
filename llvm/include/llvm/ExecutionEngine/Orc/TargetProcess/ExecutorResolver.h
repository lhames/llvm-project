//===----- ExecutorResolver.h - Resolve symbols in executor -----*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_EXECUTORRESOLVER_H
#define LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_EXECUTORRESOLVER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"

namespace llvm::orc {

class ExecutorResolver {
public:
  using ResolveResult = Expected<std::vector<std::optional<ExecutorSymbolDef>>>;

  using YieldResolveResultFn = unique_function<void(ResolveResult)>;

  virtual ~ExecutorResolver();
  virtual void resolveAsync(ArrayRef<std::string> Names,
                            YieldResolveResultFn &&YieldResolveResult) = 0;
};

} // namespace llvm::orc

extern "C" LLVM_ABI llvm::orc::shared::CWrapperFunctionResult
orc_rt_lite_ExecutorResolver_resolveSPSWrapper(
    const char *ArgData, size_t ArgSize);

#endif // LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_EXECUTORRESOLVER_H
