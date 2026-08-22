//===-- LLJITJITLinkSelectionTest.cpp - Test JITLink default selection ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Verifies that LLJIT selects ObjectLinkingLayer (JITLink) by default for
// the host target, and that plugins can be installed on it.
//
//===----------------------------------------------------------------------===//

#include "OrcTestCommon.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::orc;

namespace {

// A minimal plugin that just records whether modifyPassConfig was called.
class PluginCallTracker : public ObjectLinkingLayer::Plugin {
public:
  PluginCallTracker(bool &WasCalled) : WasCalled(WasCalled) {}

  void modifyPassConfig(MaterializationResponsibility &MR,
                        jitlink::LinkGraph &G,
                        jitlink::PassConfiguration &Config) override {
    WasCalled = true;
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
  bool &WasCalled;
};

// Test that LLJIT selects JITLink (ObjectLinkingLayer) by default on
// the host platform.
TEST(LLJITJITLinkSelectionTest, DefaultLinkerIsJITLink) {
  // Initialize native target so LLJIT can create a JIT for the host.
  OrcNativeTarget::initialize();

  auto J = LLJITBuilder().create();
  if (!J) {
    // If we can't create an LLJIT (e.g., no target registered), skip.
    consumeError(J.takeError());
    GTEST_SKIP();
  }

  // The key assertion: the default linker for this host must be JITLink
  // (ObjectLinkingLayer), not RTDyld.
  auto *OLL = dyn_cast<ObjectLinkingLayer>(&(*J)->getObjLinkingLayer());
  ASSERT_NE(OLL, nullptr)
      << "LLJIT did not select ObjectLinkingLayer (JITLink) by default. "
         "Check the host architecture case in prepareForConstruction.";

  // Verify that a plugin can be installed and actually runs.
  bool PluginWasCalled = false;
  OLL->addPlugin(std::make_unique<PluginCallTracker>(PluginWasCalled));

  // Create a trivial module with a function that returns 42.
  auto Ctx = std::make_unique<LLVMContext>();
  auto M = std::make_unique<Module>("test", *Ctx);
  M->setTargetTriple((*J)->getTargetTriple());

  auto *FT = FunctionType::get(Type::getInt32Ty(*Ctx), false);
  auto *F = Function::Create(FT, GlobalValue::ExternalLinkage, "test_fn", *M);
  auto *BB = BasicBlock::Create(*Ctx, "entry", F);
  IRBuilder<> Builder(BB);
  Builder.CreateRet(Builder.getInt32(42));

  // Add the module and look up the function.
  ASSERT_THAT_ERROR((*J)->addIRModule(ThreadSafeModule(std::move(M),
                                                       std::move(Ctx))),
                    Succeeded());

  auto Sym = (*J)->lookup("test_fn");
  ASSERT_THAT_EXPECTED(Sym, Succeeded());

  // Verify the plugin was invoked during linking.
  EXPECT_TRUE(PluginWasCalled)
      << "Plugin was not called — ObjectLinkingLayer plugin system not working";

  // Call the function and verify it returns 42.
  auto *FnPtr = Sym->toPtr<int (*)()>();
  EXPECT_EQ(FnPtr(), 42);
}

} // end anonymous namespace
