//===---------------- SimpleExecutorMemoryManager.h -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A simple allocator class suitable for basic remote-JIT use.
//
// FIXME: The functionality in this file should be moved to the ORC runtime.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_SIMPLEEXECUTORMEMORYMANAGER_H
#define LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_SIMPLEEXECUTORMEMORYMANAGER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/TargetProcessControlTypes.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/ExecutorBootstrapService.h"
#include "llvm/Support/Error.h"

#include <mutex>

namespace llvm {
namespace orc {
namespace rt_bootstrap {

/// Simple page-based allocator.
class SimpleExecutorMemoryManager : public ExecutorBootstrapService {
public:
  virtual ~SimpleExecutorMemoryManager();

  Expected<ExecutorAddr> allocate(uint64_t Size);
  void finalize(unique_function<void(Error)> OnComplete,
                tpctypes::FinalizeRequest FR);
  void deallocate(unique_function<void(Error)> OnComplete,
                  const std::vector<ExecutorAddr> Bases);

  Error shutdown() override;
  void addBootstrapSymbols(StringMap<ExecutorAddr> &M) override;

private:
  struct Allocation {
    size_t Size = 0;
    std::vector<shared::WrapperFunctionCall> DeallocActions;
  };

  using AllocationsMap = DenseMap<void *, Allocation>;

  Error
  recordFinalizedAlloc(ExecutorAddrRange R,
                       std::vector<shared::WrapperFunctionCall> DeallocActions);

  void deallocateSeq(unique_function<void(Error)> OnComplete,
                     std::vector<std::pair<void *, Allocation>> Allocs,
                     Error Err);

  static void reserveWrapper(const char *ArgData, size_t ArgSize,
                             void *SessionCtx, uintptr_t MsgCtx,
                             llvm::orc::shared::CYieldFn Yield);

  static void finalizeWrapper(const char *ArgData, size_t ArgSize,
                              void *SessionCtx, uintptr_t MsgCtx,
                              llvm::orc::shared::CYieldFn Yield);

  static void deallocateWrapper(const char *ArgData, size_t ArgSize,
                                void *SessionCtx, uintptr_t MsgCtx,
                                llvm::orc::shared::CYieldFn Yield);

  std::mutex M;
  AllocationsMap Allocations;
};

} // end namespace rt_bootstrap
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_TARGETPROCESS_SIMPLEEXECUTORMEMORYMANAGER_H
