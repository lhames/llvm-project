//===------------------------ OrcRTBootstrap.cpp --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OrcRTBootstrap.h"

#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"

#define DEBUG_TYPE "orc"

using namespace llvm::orc::shared;

namespace llvm {
namespace orc {
namespace rt_bootstrap {

template <typename WriteT, typename SPSWriteT>
static void writeUIntsWrapper(const char *ArgData, size_t ArgSize,
                              void *SessionCtx, uintptr_t MsgCtx,
                              shared::CYieldFn Yield) {
  WrapperFunction<void(SPSSequence<SPSWriteT>)>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](std::vector<WriteT> Ws) {
        for (auto &W : Ws)
          *W.Addr.template toPtr<decltype(W.Value) *>() = W.Value;
      });
}

static void writeBuffersWrapper(const char *ArgData, size_t ArgSize,
                                void *SessionCtx, uintptr_t MsgCtx,
                                shared::CYieldFn Yield) {
  WrapperFunction<void(SPSSequence<SPSMemoryAccessBufferWrite>)>::
      handleAsyncWithSync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                          [](std::vector<tpctypes::BufferWrite> Ws) {
                            for (auto &W : Ws)
                              memcpy(W.Addr.template toPtr<char *>(),
                                     W.Buffer.data(), W.Buffer.size());
                          });
}

static void writePointersWrapper(const char *ArgData, size_t ArgSize,
                                 void *SessionCtx, uintptr_t MsgCtx,
                                 shared::CYieldFn Yield) {
  WrapperFunction<void(SPSSequence<SPSMemoryAccessPointerWrite>)>::
      handleAsyncWithSync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                          [](std::vector<tpctypes::PointerWrite> Ws) {
                            for (auto &W : Ws)
                              *W.Addr.template toPtr<void **>() =
                                  W.Value.template toPtr<void *>();
                          });
}

static void runAsMainWrapper(const char *ArgData, size_t ArgSize,
                             void *SessionCtx, uintptr_t MsgCtx,
                             shared::CYieldFn Yield) {
  WrapperFunction<rt::SPSRunAsMainSignature>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](ExecutorAddr MainAddr, std::vector<std::string> Args) -> int64_t {
        return runAsMain(MainAddr.toPtr<int (*)(int, char *[])>(), Args);
      });
}

static void runAsVoidFunctionWrapper(const char *ArgData, size_t ArgSize,
                                     void *SessionCtx, uintptr_t MsgCtx,
                                     shared::CYieldFn Yield) {
  WrapperFunction<rt::SPSRunAsVoidFunctionSignature>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](ExecutorAddr MainAddr) -> int32_t {
        return runAsVoidFunction(MainAddr.toPtr<int32_t (*)(void)>());
      });
}

static void runAsIntFunctionWrapper(const char *ArgData, size_t ArgSize,
                                    void *SessionCtx, uintptr_t MsgCtx,
                                    shared::CYieldFn Yield) {
  WrapperFunction<rt::SPSRunAsIntFunctionSignature>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](ExecutorAddr MainAddr, int32_t Arg) -> int32_t {
        return runAsIntFunction(MainAddr.toPtr<int32_t (*)(int32_t)>(), Arg);
      });
}

void addTo(StringMap<ExecutorAddr> &M) {
  M[rt::MemoryWriteUInt8sWrapperName] = ExecutorAddr::fromPtr(
      &writeUIntsWrapper<tpctypes::UInt8Write,
                         shared::SPSMemoryAccessUInt8Write>);
  M[rt::MemoryWriteUInt16sWrapperName] = ExecutorAddr::fromPtr(
      &writeUIntsWrapper<tpctypes::UInt16Write,
                         shared::SPSMemoryAccessUInt16Write>);
  M[rt::MemoryWriteUInt32sWrapperName] = ExecutorAddr::fromPtr(
      &writeUIntsWrapper<tpctypes::UInt32Write,
                         shared::SPSMemoryAccessUInt32Write>);
  M[rt::MemoryWriteUInt64sWrapperName] = ExecutorAddr::fromPtr(
      &writeUIntsWrapper<tpctypes::UInt64Write,
                         shared::SPSMemoryAccessUInt64Write>);
  M[rt::MemoryWriteBuffersWrapperName] =
      ExecutorAddr::fromPtr(&writeBuffersWrapper);
  M[rt::MemoryWritePointersWrapperName] =
      ExecutorAddr::fromPtr(&writePointersWrapper);
  M[rt::RunAsMainWrapperName] = ExecutorAddr::fromPtr(&runAsMainWrapper);
  M[rt::RunAsVoidFunctionWrapperName] =
      ExecutorAddr::fromPtr(&runAsVoidFunctionWrapper);
  M[rt::RunAsIntFunctionWrapperName] =
      ExecutorAddr::fromPtr(&runAsIntFunctionWrapper);
}

} // end namespace rt_bootstrap
} // end namespace orc
} // end namespace llvm
