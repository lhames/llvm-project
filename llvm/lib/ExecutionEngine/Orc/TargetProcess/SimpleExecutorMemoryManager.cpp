//===- SimpleExecuorMemoryManagare.cpp - Simple executor-side memory mgmt -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcess/SimpleExecutorMemoryManager.h"

#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MSVCErrorWorkarounds.h"

#include <future>

#define DEBUG_TYPE "orc"

using namespace llvm::orc::shared;

namespace llvm {
namespace orc {
namespace rt_bootstrap {

SimpleExecutorMemoryManager::~SimpleExecutorMemoryManager() {
  assert(Allocations.empty() && "shutdown not called?");
}

Expected<ExecutorAddr> SimpleExecutorMemoryManager::allocate(uint64_t Size) {
  std::error_code EC;
  auto MB = sys::Memory::allocateMappedMemory(
      Size, nullptr, sys::Memory::MF_READ | sys::Memory::MF_WRITE, EC);
  if (EC)
    return errorCodeToError(EC);
  std::lock_guard<std::mutex> Lock(M);
  assert(!Allocations.count(MB.base()) && "Duplicate allocation addr");
  Allocations[MB.base()].Size = Size;
  return ExecutorAddr::fromPtr(MB.base());
}

void SimpleExecutorMemoryManager::finalize(
    unique_function<void(Error)> OnComplete, tpctypes::FinalizeRequest FR) {
  // TODO: Check that segments don't overlap prior to taking any actions?
  //       This would require registering the range up-front and then removing
  //       it again if any actions errored out.
  // TODO: Check for duplicate finalization?

  if (FR.Segments.empty()) {
    if (FR.Actions.empty())
      return OnComplete(Error::success());
    else
      return OnComplete(
          make_error<StringError>("Finalization actions attached to empty "
                                  "finalization request",
                                  inconvertibleErrorCode()));
  }

  // Find the address range for this allocation to use as a key.
  ExecutorAddrRange AllocRange(FR.Segments.front().Addr,
                               FR.Segments.front().Addr);
  for (auto &Seg : FR.Segments) {
    AllocRange.Start = std::min(AllocRange.Start, Seg.Addr);
    AllocRange.End = std::max(AllocRange.End, Seg.Addr + Seg.Size);
  }

  // Deallocate memory.
  auto ReleaseMemory = [AllocRange](Error Err) -> Error {
    sys::MemoryBlock MB(AllocRange.Start.toPtr<void *>(), AllocRange.size());
    auto EC = sys::Memory::releaseMappedMemory(MB);
    return joinErrors(std::move(Err), errorCodeToError(EC));
  };
  auto Abandon = [&](Error Err) { OnComplete(ReleaseMemory(std::move(Err))); };

  // Copy content and apply permissions.
  for (auto &Seg : FR.Segments) {

    // Check segment ranges.
    if (LLVM_UNLIKELY(Seg.Size < Seg.Content.size()))
      return Abandon(make_error<StringError>(
          formatv("Segment {0:x} content size ({1:x} bytes) "
                  "exceeds segment size ({2:x} bytes)",
                  Seg.Addr.getValue(), Seg.Content.size(), Seg.Size),
          inconvertibleErrorCode()));
    ExecutorAddr SegEnd = Seg.Addr + ExecutorAddrDiff(Seg.Size);
    if (LLVM_UNLIKELY(Seg.Addr < AllocRange.Start || SegEnd > AllocRange.End))
      return Abandon(make_error<StringError>(
          formatv("Segment {0:x} -- {1:x} crosses boundary of "
                  "allocation {2:x} -- {3:x}",
                  Seg.Addr.getValue(), SegEnd.getValue(),
                  AllocRange.Start.getValue(), AllocRange.End.getValue()),
          inconvertibleErrorCode()));

    char *Mem = Seg.Addr.toPtr<char *>();
    if (!Seg.Content.empty())
      memcpy(Mem, Seg.Content.data(), Seg.Content.size());
    memset(Mem + Seg.Content.size(), 0, Seg.Size - Seg.Content.size());
    assert(Seg.Size <= std::numeric_limits<size_t>::max());
    if (auto EC = sys::Memory::protectMappedMemory(
            {Mem, static_cast<size_t>(Seg.Size)},
            toSysMemoryProtectionFlags(Seg.RAG.Prot)))
      return Abandon(errorCodeToError(EC));
    if ((Seg.RAG.Prot & MemProt::Exec) == MemProt::Exec)
      sys::Memory::InvalidateInstructionCache(Mem, Seg.Size);
  }

  runFinalizeActions(
      std::move(FR.Actions),
      [this, R = std::move(AllocRange), OnComplete = std::move(OnComplete),
       ReleaseMemory = std::move(ReleaseMemory)](
          Expected<std::vector<WrapperFunctionCall>> DeallocActions) mutable {
        if (!DeallocActions)
          return OnComplete(ReleaseMemory(DeallocActions.takeError()));
        if (auto Err = recordFinalizedAlloc(R, std::move(*DeallocActions)))
          return OnComplete(ReleaseMemory(std::move(Err)));
        OnComplete(Error::success());
      });
}

void SimpleExecutorMemoryManager::deallocate(
    unique_function<void(Error)> OnComplete, std::vector<ExecutorAddr> Bases) {
  std::vector<std::pair<void *, Allocation>> AllocPairs;
  AllocPairs.reserve(Bases.size());

  // Get allocation to destroy.
  Error Err = Error::success();
  {
    std::lock_guard<std::mutex> Lock(M);
    for (auto &Base : Bases) {
      auto I = Allocations.find(Base.toPtr<void *>());

      // Check for missing allocation (effective a double free).
      if (I != Allocations.end()) {
        AllocPairs.push_back(std::move(*I));
        Allocations.erase(I);
      } else
        Err = joinErrors(
            std::move(Err),
            make_error<StringError>("No allocation entry found for " +
                                        formatv("{0:x}", Base.getValue()),
                                    inconvertibleErrorCode()));
    }
  }

  deallocateSeq(std::move(OnComplete), std::move(AllocPairs), std::move(Err));
}

Error SimpleExecutorMemoryManager::shutdown() {

  AllocationsMap AM;
  {
    std::lock_guard<std::mutex> Lock(M);
    AM = std::move(Allocations);
  }

  std::vector<std::pair<void *, Allocation>> Allocs;
  for (auto &[Addr, Alloc] : AM)
    Allocs.push_back(std::make_pair(std::move(Addr), std::move(Alloc)));

  std::promise<MSVCPError> ErrP;
  auto ErrF = ErrP.get_future();

  deallocateSeq([&](Error Err) { ErrP.set_value(std::move(Err)); },
                std::move(Allocs), Error::success());

  return ErrF.get();
}

void SimpleExecutorMemoryManager::addBootstrapSymbols(
    StringMap<ExecutorAddr> &M) {
  M[rt::SimpleExecutorMemoryManagerInstanceName] = ExecutorAddr::fromPtr(this);
  M[rt::SimpleExecutorMemoryManagerReserveWrapperName] =
      ExecutorAddr::fromPtr(&reserveWrapper);
  M[rt::SimpleExecutorMemoryManagerFinalizeWrapperName] =
      ExecutorAddr::fromPtr(&finalizeWrapper);
  M[rt::SimpleExecutorMemoryManagerDeallocateWrapperName] =
      ExecutorAddr::fromPtr(&deallocateWrapper);
}

Error SimpleExecutorMemoryManager::recordFinalizedAlloc(
    ExecutorAddrRange R,
    std::vector<shared::WrapperFunctionCall> DeallocActions) {
  std::lock_guard<std::mutex> Lock(M);

  auto I = Allocations.find(R.Start.toPtr<void *>());
  if (I == Allocations.end())
    return make_error<StringError>("Allocation at " +
                                       formatv("{0:x}", R.Start) +
                                       " overlaps existing allocation",
                                   inconvertibleErrorCode());

  I->second.DeallocActions = std::move(DeallocActions);
  return Error::success();
}

void SimpleExecutorMemoryManager::deallocateSeq(
    unique_function<void(Error)> OnComplete,
    std::vector<std::pair<void *, Allocation>> Allocs, Error Err) {
  if (Allocs.empty())
    return OnComplete(std::move(Err));

  auto A = Allocs.back();
  Allocs.pop_back();

  runDeallocActions(
      std::move(A.second.DeallocActions),
      [this, Allocs = std::move(Allocs), PreviousErrs = std::move(Err),
       OnComplete = std::move(OnComplete)](Error Err) mutable {
        deallocateSeq(std::move(OnComplete), std::move(Allocs),
                      joinErrors(std::move(PreviousErrs), std::move(Err)));
      });
}

void SimpleExecutorMemoryManager::reserveWrapper(const char *ArgData,
                                                 size_t ArgSize,
                                                 void *SessionCtx,
                                                 uintptr_t MsgCtx,
                                                 CYieldFn Yield) {
  WrapperFunction<rt::SPSSimpleExecutorMemoryManagerReserveSignature>::
      handleAsyncWithSync(
          ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
          makeMethodWrapperHandler(&SimpleExecutorMemoryManager::allocate));
}

void SimpleExecutorMemoryManager::finalizeWrapper(const char *ArgData,
                                                  size_t ArgSize,
                                                  void *SessionCtx,
                                                  uintptr_t MsgCtx,
                                                  CYieldFn Yield) {
  WrapperFunction<rt::SPSSimpleExecutorMemoryManagerFinalizeSignature>::
      handleAsync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                  makeAsyncMethodWrapperHandler(
                      &SimpleExecutorMemoryManager::finalize));
}

void SimpleExecutorMemoryManager::deallocateWrapper(const char *ArgData,
                                                    size_t ArgSize,
                                                    void *SessionCtx,
                                                    uintptr_t MsgCtx,
                                                    CYieldFn Yield) {
  WrapperFunction<rt::SPSSimpleExecutorMemoryManagerDeallocateSignature>::
      handleAsync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                  makeAsyncMethodWrapperHandler(
                      &SimpleExecutorMemoryManager::deallocate));
}

} // namespace rt_bootstrap
} // end namespace orc
} // end namespace llvm
