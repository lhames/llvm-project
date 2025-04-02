//===--------- ExecutorResolver.cpp - Resolve symbols in executor ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcess/ExecutorResolver.h"

#include "llvm/Support/MSVCErrorWorkarounds.h"

#include <future>
#include <optional>
#include <string>
#include <vector>

#define DEBUG_TYPE "orc"

using namespace llvm;
using namespace llvm::orc;
using namespace llvm::orc::shared;

namespace llvm::orc {

ExecutorResolver::~ExecutorResolver() = default;

} // namespace llvm::orc

llvm::orc::shared::CWrapperFunctionResult
orc_rt_lite_ExecutorResolver_resolveSPSWrapper(
    const char *ArgData, size_t ArgSize) {

  using ResolveResult = ExecutorResolver::ResolveResult;

  using ResolveSPSSig =
    SPSExpected<SPSSequence<SPSOptional<SPSExecutorSymbolDef>>>(
        SPSExecutorAddr, SPSSequence<SPSString>);

  return WrapperFunction<ResolveSPSSig>::handle(
      ArgData, ArgSize,
      [](ExecutorAddr Obj, std::vector<std::string> Names) -> ResolveResult {
        using TmpResultT =
            MSVCPExpected<std::vector<std::optional<ExecutorSymbolDef>>>;
        std::promise<TmpResultT> P;
        auto F = P.get_future();
        Obj.toPtr<ExecutorResolver*>()->resolveAsync(
            Names, [&](ResolveResult R) { P.set_value(std::move(R)); });
        return F.get();
      }).release();
}
