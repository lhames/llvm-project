//===- SEHFrameRegistrationPlugin.h - Register COFF EH info in-process -*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_SEHFRAMEREGISTRATIONPLUGIN_H
#define LLVM_EXECUTIONENGINE_ORC_SEHFRAMEREGISTRATIONPLUGIN_H

#include "llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h"

namespace llvm::orc {

/// Registers .pdata sections with the Windows unwinder via RtlAddFunctionTable.
///
/// This plugin enables SEH-based stack unwinding for JIT'd code on Windows
/// by registering unwind metadata at finalization and deregistering it on
/// deallocation.
class LLVM_ABI SEHFrameRegistrationPlugin : public LinkGraphLinkingLayer::Plugin {
public:
  /// Adds a pass to PassConfig that registers .pdata sections in LG with the
  /// OS unwinder.
  void modifyPassConfig(MaterializationResponsibility &MR,
                        jitlink::LinkGraph &LG,
                        jitlink::PassConfiguration &PassConfig) override;

  Error notifyFailed(MaterializationResponsibility &MR) override {
    return Error::success();
  }

  Error notifyRemovingResources(JITDylib &JD, ResourceKey K) override {
    return Error::success();
  }

  void notifyTransferringResources(JITDylib &JD, ResourceKey DstKey,
                                   ResourceKey SrcKey) override {}

private:
  /// Registers .pdata sections in G with the OS unwinder.
  /// Returns an error if registration setup fails.
  Error registerFrameInfo(jitlink::LinkGraph &G);
};

} // namespace llvm::orc

#endif // LLVM_EXECUTIONENGINE_ORC_SEHFRAMEREGISTRATIONPLUGIN_H
