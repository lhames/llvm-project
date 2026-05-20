//===----- SEHFrameRegistrationPlugin.cpp - Windows SEH registration ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/SEHFrameRegistrationPlugin.h"

#include "llvm/ExecutionEngine/JITLink/COFF.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>

#ifdef _WIN32
#include <windows.h>
#endif

#define DEBUG_TYPE "orc"

using namespace llvm::jitlink;

namespace llvm::orc {

// Calls RtlAddFunctionTable to register .pdata entries with the OS unwinder.
// ArgData/ArgSize contain the serialized base address and .pdata range.
// Returns a serialized error result indicating success or failure.
shared::CWrapperFunctionBuffer registerPData(const char *ArgData,
                                             size_t ArgSize) {
  using namespace shared;
  return WrapperFunction<SPSError(SPSExecutorAddr, SPSExecutorAddrRange)>::
      handle(ArgData, ArgSize,
             [](ExecutorAddr Base, ExecutorAddrRange PDataRange) -> Error {
#ifdef _WIN32
               constexpr size_t RecordSize = sizeof(RUNTIME_FUNCTION);
               if (PDataRange.size() % RecordSize)
                 return make_error<StringError>(
                     ".pdata section does not contain an integer number of "
                     "RUNTIME_FUNCTION records",
                     inconvertibleErrorCode());
               size_t Count = PDataRange.size() / RecordSize;
               if (Count > std::numeric_limits<uint32_t>::max())
                 return make_error<StringError>(
                     ".pdata section contains too many records",
                     inconvertibleErrorCode());
               if (RtlAddFunctionTable(
                       PDataRange.Start.toPtr<PRUNTIME_FUNCTION>(), Count,
                       Base.getValue()))
                 return Error::success();
               else
                 return make_error<StringError>(
                     "RtlAddFunctionTable returned error",
                     inconvertibleErrorCode());
#else
               return make_error<StringError>(
                   "SEH registration not supported on this platform",
                   inconvertibleErrorCode());
#endif
             })
          .release();
}

// Calls RtlDeleteFunctionTable to unregister .pdata entries from the OS unwinder.
// ArgData/ArgSize contain the serialized .pdata start address.
// Returns a serialized error result indicating success or failure.
shared::CWrapperFunctionBuffer deregisterPData(const char *ArgData,
                                               size_t ArgSize) {
  using namespace shared;
  return WrapperFunction<SPSError(SPSExecutorAddr)>::handle(
             ArgData, ArgSize,
             [](ExecutorAddr PDataStart) -> Error {
#ifdef _WIN32
               if (RtlDeleteFunctionTable(
                       PDataStart.toPtr<PRUNTIME_FUNCTION>()))
                 return Error::success();
               else
                 return make_error<StringError>(
                     "RtlDeleteFunctionTable returned error",
                     inconvertibleErrorCode());
#else
               return make_error<StringError>(
                   "SEH deregistration not supported on this platform",
                   inconvertibleErrorCode());
#endif
             })
      .release();
}

void SEHFrameRegistrationPlugin::modifyPassConfig(
    MaterializationResponsibility &MR, jitlink::LinkGraph &LG,
    jitlink::PassConfiguration &PassConfig) {
  PassConfig.PostFixupPasses.push_back(
      [this](jitlink::LinkGraph &G) { return registerFrameInfo(G); });
}

Error SEHFrameRegistrationPlugin::registerFrameInfo(jitlink::LinkGraph &G) {
  using namespace shared;

  auto *ImageBase = jitlink::GetImageBaseSymbol()(G);
  if (!ImageBase)
    return Error::success();

  // Register each .pdata prefixed section (includes COMDAT
  // .pdata$<suffix>).
  for (auto &PDataSection : G.sections()) {
    if (!PDataSection.getName().starts_with(".pdata"))
      continue;

    ExecutorAddr Base(ImageBase->getAddress());
    ExecutorAddrRange PDataRange(jitlink::SectionRange(PDataSection).getRange());

    G.allocActions().push_back(
        {cantFail(WrapperFunctionCall::Create<
                  SPSArgList<SPSExecutorAddr, SPSExecutorAddrRange>>(
             ExecutorAddr::fromPtr(&registerPData), Base, PDataRange)),
         cantFail(WrapperFunctionCall::Create<SPSArgList<SPSExecutorAddr>>(
             ExecutorAddr::fromPtr(&deregisterPData), PDataRange.Start))});
  }
  return Error::success();
}

} // namespace llvm::orc
