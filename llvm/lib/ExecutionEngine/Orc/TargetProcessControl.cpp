//===------ TargetProcessControl.cpp -- Target process control APIs -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcessControl.h"

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Process.h"

#include <mutex>

#define DEBUG_TYPE "orc"

namespace llvm {
namespace orc {

TargetProcessControl::MemoryAccess::~MemoryAccess() {}

TargetProcessControl::~TargetProcessControl() {}

SelfTargetProcessControl::SelfTargetProcessControl(
    std::shared_ptr<SymbolStringPool> SSP, Triple TargetTriple,
    unsigned PageSize, std::unique_ptr<jitlink::JITLinkMemoryManager> MemMgr)
    : TargetProcessControl(std::move(SSP)) {

  OwnedMemMgr = std::move(MemMgr);
  if (!OwnedMemMgr)
    OwnedMemMgr = std::make_unique<jitlink::InProcessMemoryManager>();

  this->TargetTriple = std::move(TargetTriple);
  this->PageSize = PageSize;
  this->MemMgr = OwnedMemMgr.get();
  this->MemAccess = this;
  this->JITDispatchInfo = {
      pointerToJITTargetAddress(jitDispatchViaWrapperFunctionManager),
      pointerToJITTargetAddress(&WFM)};
  if (this->TargetTriple.isOSBinFormatMachO())
    GlobalManglingPrefix = '_';
}

Expected<std::unique_ptr<SelfTargetProcessControl>>
SelfTargetProcessControl::Create(
    std::shared_ptr<SymbolStringPool> SSP,
    std::unique_ptr<jitlink::JITLinkMemoryManager> MemMgr) {
  auto PageSize = sys::Process::getPageSize();
  if (!PageSize)
    return PageSize.takeError();

  Triple TT(sys::getProcessTriple());

  return std::make_unique<SelfTargetProcessControl>(
      std::move(SSP), std::move(TT), *PageSize, std::move(MemMgr));
}

Expected<tpctypes::DylibHandle>
SelfTargetProcessControl::loadDylib(const char *DylibPath) {
  std::string ErrMsg;
  auto Dylib = std::make_unique<sys::DynamicLibrary>(
      sys::DynamicLibrary::getPermanentLibrary(DylibPath, &ErrMsg));
  if (!Dylib->isValid())
    return make_error<StringError>(std::move(ErrMsg), inconvertibleErrorCode());
  DynamicLibraries.push_back(std::move(Dylib));
  return pointerToJITTargetAddress(DynamicLibraries.back().get());
}

Expected<std::vector<tpctypes::LookupResult>>
SelfTargetProcessControl::lookupSymbols(ArrayRef<LookupRequest> Request) {
  std::vector<tpctypes::LookupResult> R;

  for (auto &Elem : Request) {
    auto *Dylib = jitTargetAddressToPointer<sys::DynamicLibrary *>(Elem.Handle);
    assert(llvm::any_of(DynamicLibraries,
                        [=](const std::unique_ptr<sys::DynamicLibrary> &DL) {
                          return DL.get() == Dylib;
                        }) &&
           "Invalid handle");

    R.push_back(std::vector<JITTargetAddress>());
    for (auto &KV : Elem.Symbols) {
      auto &Sym = KV.first;
      std::string Tmp((*Sym).data() + !!GlobalManglingPrefix,
                      (*Sym).size() - !!GlobalManglingPrefix);
      void *Addr = Dylib->getAddressOfSymbol(Tmp.c_str());
      if (!Addr && KV.second == SymbolLookupFlags::RequiredSymbol) {
        // FIXME: Collect all failing symbols before erroring out.
        SymbolNameVector MissingSymbols;
        MissingSymbols.push_back(Sym);
        return make_error<SymbolsNotFound>(std::move(MissingSymbols));
      }
      R.back().push_back(pointerToJITTargetAddress(Addr));
    }
  }

  return R;
}

Expected<int64_t>
SelfTargetProcessControl::runAsMain(JITTargetAddress MainFnAddr,
                                    ArrayRef<std::string> Args) {
  using MainTy = int (*)(int, char *[]);
  return orc::runAsMain(jitTargetAddressToFunction<MainTy>(MainFnAddr), Args);
}

Expected<shared::WrapperFunctionResult>
SelfTargetProcessControl::runWrapper(JITTargetAddress WrapperFnAddr,
                                     ArrayRef<char> ArgBuffer) {
  using WrapperFnTy =
      LLVMOrcSharedCWrapperFunctionResult (*)(const char *Data, uint64_t Size);
  auto *WrapperFn = jitTargetAddressToFunction<WrapperFnTy>(WrapperFnAddr);
  LLVM_DEBUG({
    dbgs() << "Calling wrapper function " << formatv("{0:x}", WrapperFnAddr)
           << " with arg buffer:\n";
    for (size_t I = 0; I != ArgBuffer.size(); ++I) {
      if (I % 16 == 0)
        dbgs() << " ";
      dbgs() << " " << formatv("{0:x2}", ArgBuffer[I]);
      if (I % 16 == 15)
        dbgs() << "\n";
    }
    if (ArgBuffer.size() % 16)
      dbgs() << "\n";
  });
  return WrapperFn(ArgBuffer.data(), ArgBuffer.size());
}

Error SelfTargetProcessControl::disconnect() { return Error::success(); }

void SelfTargetProcessControl::writeUInt8s(ArrayRef<tpctypes::UInt8Write> Ws,
                                           WriteResultFn OnWriteComplete) {
  for (auto &W : Ws)
    *jitTargetAddressToPointer<uint8_t *>(W.Address) = W.Value;
  OnWriteComplete(Error::success());
}

void SelfTargetProcessControl::writeUInt16s(ArrayRef<tpctypes::UInt16Write> Ws,
                                            WriteResultFn OnWriteComplete) {
  for (auto &W : Ws)
    *jitTargetAddressToPointer<uint16_t *>(W.Address) = W.Value;
  OnWriteComplete(Error::success());
}

void SelfTargetProcessControl::writeUInt32s(ArrayRef<tpctypes::UInt32Write> Ws,
                                            WriteResultFn OnWriteComplete) {
  for (auto &W : Ws)
    *jitTargetAddressToPointer<uint32_t *>(W.Address) = W.Value;
  OnWriteComplete(Error::success());
}

void SelfTargetProcessControl::writeUInt64s(ArrayRef<tpctypes::UInt64Write> Ws,
                                            WriteResultFn OnWriteComplete) {
  for (auto &W : Ws)
    *jitTargetAddressToPointer<uint64_t *>(W.Address) = W.Value;
  OnWriteComplete(Error::success());
}

void SelfTargetProcessControl::writeBuffers(ArrayRef<tpctypes::BufferWrite> Ws,
                                            WriteResultFn OnWriteComplete) {
  for (auto &W : Ws)
    memcpy(jitTargetAddressToPointer<char *>(W.Address), W.Buffer.data(),
           W.Buffer.size());
  OnWriteComplete(Error::success());
}

LLVMOrcSharedCWrapperFunctionResult
SelfTargetProcessControl::jitDispatchViaWrapperFunctionManager(
    void *Ctx, const void *FnTag, const char *Data, size_t Size) {
  auto R = static_cast<WrapperFunctionManager *>(Ctx)->runWrapper(
      pointerToJITTargetAddress(FnTag), {Data, Size});
  if (!R) {
    auto ErrMsg = toString(R.takeError());
    return shared::WrapperFunctionResult::createOutOfBandErrorFromString(
               ErrMsg.data())
        .release();
  }
  return R->release();
}

} // end namespace orc
} // end namespace llvm
