//===-- SEHFrameRegistrationPluginTest.cpp - Unit tests -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/SEHFrameRegistrationPlugin.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::jitlink;
using namespace llvm::orc;

// Test: registerFrameInfo must skip .pdata sections that have no blocks
// (e.g. COMDAT sections whose blocks were dead-stripped during pruning).
// Empty sections produce a zero-sized SectionRange; registering them with
// RtlAddFunctionTable is invalid (zero entries at a null base address).
TEST(SEHFrameRegistrationPluginTest, SkipsEmptyPDataSections) {
  LinkGraph G("test.obj", std::make_shared<SymbolStringPool>(),
              Triple("x86_64-pc-windows-msvc"), SubtargetFeatures(),
              getGenericEdgeKindName);

  // Minimal .text with a block (0xC3 = x86 ret, content is arbitrary).
  auto &TextSec = G.createSection(".text", MemProt::Read | MemProt::Exec);
  char TextContent[] = {'\xc3'};
  auto &TextBlock = G.createContentBlock(TextSec, TextContent,
                                         ExecutorAddr(0x1000), 16, 0);
  G.addDefinedSymbol(TextBlock, 0, "main", 1, Linkage::Strong, Scope::Default,
                     true, false);

  // __ImageBase resolved (as COFFImageBaseResolution_x86_64 would do).
  auto &ImageBase = G.addExternalSymbol("__ImageBase", 0, false);
  ImageBase.getAddressable().setAddress(ExecutorAddr(0x1000));

  // A .pdata section with content (should produce an allocAction).
  auto &PDataSec = G.createSection(".pdata", MemProt::Read);
  char PDataContent[12] = {};
  G.createContentBlock(PDataSec, PDataContent, ExecutorAddr(0x2000), 4, 0);

  // Two empty .pdata$<symbol> sections (simulating dead-stripped COMDATs).
  G.createSection(".pdata$dead_comdat_1", MemProt::Read);
  G.createSection(".pdata$dead_comdat_2", MemProt::Read);

  SEHFrameRegistrationPlugin Plugin;
  EXPECT_THAT_ERROR(Plugin.registerFrameInfo(G), Succeeded());

  // Expect exactly one allocAction for the non-empty .pdata section.
  // The two empty sections must not generate allocActions.
  EXPECT_EQ(G.allocActions().size(), 1u);
}
