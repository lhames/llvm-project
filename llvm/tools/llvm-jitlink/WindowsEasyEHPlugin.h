//===- WindowsEasyEHPlugin.h - Register COFF EH info in-process -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Register and deregister .pdata sections in-process using RtlAddFunctionTable
// and RtlDeleteFunctionTable.
//
//===----------------------------------------------------------------------===//

#ifndef WINDOWSEASYEHPLUGIN_H
#define WINDOWSEASYEHPLUGIN_H

#include "llvm/ExecutionEngine/JITLink/COFF.h"
#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>

#ifdef _WIN32
#include <windows.h>
#else
// For testing purposes only.
using DWORD = uint32_t;
using DWORD64 = uint64_t;
struct RUNTIME_FUNCTION {
  DWORD BeginAddress;
  DWORD EndAddress;
  union {
    DWORD UnwindInfoAddress;
    DWORD UnwindData;
  } DUMMYUNIONNAME;
};
using PRUNTIME_FUNCTION = RUNTIME_FUNCTION *;

static inline bool RtlAddFunctionTable(PRUNTIME_FUNCTION FunctionTable,
                                       DWORD Count, DWORD64 BaseAddress) {
  llvm::dbgs() << "RtlAddFunctionTable(" << static_cast<void *>(FunctionTable)
               << ", " << Count << ", " << llvm::formatv("{0:x})", BaseAddress)
               << ");\n";
  return true;
}

static inline bool RtlDeleteFunctionTable(PRUNTIME_FUNCTION FunctionTable) {
  llvm::dbgs() << "RtlDeleteFunctionTable("
               << static_cast<void *>(FunctionTable) << ");\n";
  return true;
}

#endif

namespace llvm::orc {

class WindowsEasyEHPlugin : public ObjectLinkingLayer::Plugin {
public:
  void modifyPassConfig(MaterializationResponsibility &MR,
                        jitlink::LinkGraph &LG,
                        jitlink::PassConfiguration &PassConfig) override {
    PassConfig.PostFixupPasses.push_back(
        [this](jitlink::LinkGraph &G) { return registerFrameInfo(G); });
  }

  Error notifyFailed(MaterializationResponsibility &MR) override {
    return Error::success();
  }

  Error notifyRemovingResources(JITDylib &JD, ResourceKey K) override {
    return Error::success();
  }

  void notifyTransferringResources(JITDylib &JD, ResourceKey DstKey,
                                   ResourceKey SrcKey) override {}

private:
  Error registerFrameInfo(jitlink::LinkGraph &G) {
    using namespace shared;

    auto *PDataSection = G.findSectionByName(".pdata");
    if (!PDataSection)
      return Error::success();

    ExecutorAddr Base(~uint64_t(0));
    ExecutorAddrRange PDataRange;
    if (auto *ImageBase = jitlink::GetImageBaseSymbol()(G)) {
      // If there's an __ImageBase symbol then use it to get the base address.
      Base = ImageBase->getAddress();
      PDataRange = jitlink::SectionRange(*PDataSection).getRange();
    } else {
      // No __ImageBase. Use the lowest address in this graph as a substitute.
      for (auto &Sec : G.sections()) {
        if (Sec.empty())
          continue;
        jitlink::SectionRange SR(Sec);
        Base = std::min(Base, SR.getStart());
        if (&Sec == PDataSection)
          PDataRange = SR.getRange();
      }
    }

    G.allocActions().push_back(
        {cantFail(WrapperFunctionCall::Create<
                  SPSArgList<SPSExecutorAddr, SPSExecutorAddrRange>>(
             ExecutorAddr::fromPtr(&registerPData), Base, PDataRange)),
         cantFail(WrapperFunctionCall::Create<SPSArgList<SPSExecutorAddr>>(
             ExecutorAddr::fromPtr(&deregisterPData), PDataRange.Start))});

    return Error::success();
  }

  static shared::CWrapperFunctionBuffer registerPData(const char *ArgData,
                                                      size_t ArgSize) {
    using namespace shared;
    return WrapperFunction<SPSError(SPSExecutorAddr, SPSExecutorAddrRange)>::
        handle(ArgData, ArgSize,
               [](ExecutorAddr Base, ExecutorAddrRange PDataRange) -> Error {
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
               })
            .release();
  }

  static shared::CWrapperFunctionBuffer deregisterPData(const char *ArgData,
                                                        size_t ArgSize) {
    using namespace shared;
    return WrapperFunction<SPSError(SPSExecutorAddr)>::handle(
               ArgData, ArgSize,
               [](ExecutorAddr PDataStart) -> Error {
                 if (RtlDeleteFunctionTable(
                         PDataStart.toPtr<PRUNTIME_FUNCTION>()))
                   return Error::success();
                 else
                   return make_error<StringError>(
                       "RtlDeleteFunctionTable returned error",
                       inconvertibleErrorCode());
               })
        .release();
  }
};

} // namespace llvm::orc

#endif // WINDOWSEASYEHPLUGIN_H
