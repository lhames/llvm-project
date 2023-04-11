//=------- CompactUnwindSupport.cpp - Compact Unwind format support -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compact Unwind support.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/JITLink/CompactUnwindSupport.h"

#define DEBUG_TYPE "jitlink"

namespace llvm {
namespace jitlink {

Error splitCompactUnwindBlocks(LinkGraph &G, Section &CompactUnwindSection,
                               size_t RecordSize) {
  LLVM_DEBUG({
      dbgs() << "In " << G.getName() << ", splitting compact unwind records in "
             << CompactUnwindSection.getName() << ":\n";
    });
  DenseMap<Block *, LinkGraph::SplitBlockCache> Caches;
  {
    // Pre-build the split caches.
    for (auto *B : CompactUnwindSection.blocks())
      Caches[B] = LinkGraph::SplitBlockCache::value_type();
    for (auto *Sym : CompactUnwindSection.symbols())
      Caches[&Sym->getBlock()]->push_back(Sym);
    for (auto *B : CompactUnwindSection.blocks())
      llvm::sort(*Caches[B], [](const Symbol *LHS, const Symbol *RHS) {
        return LHS->getOffset() > RHS->getOffset();
      });
  }

  // Iterate over blocks (we do this by iterating over Caches entries rather
  // than Section->blocks() as we will be inserting new blocks along the way,
  // which would invalidate iterators in the latter sequence.
  for (auto &KV : Caches) {
    auto &B = *KV.first;
    auto &BCache = KV.second;

    if (B.isZeroFill())
      return make_error<JITLinkError>(
          "In " + G.getName() + ", compact unwind block at " +
          formatv("{0:x}", B.getAddress()) + " is zero-fill");

    if (B.getSize() % RecordSize)
      return make_error<JITLinkError>(
          "In " + G.getName() + ", compact unwind block at " +
          formatv("{0:x}", B.getAddress()) +
          " is not a multiple of record size");

    size_t NumRecords = B.getSize() / RecordSize;
    LLVM_DEBUG({
        dbgs() << "  Splitting block at " << B.getAddress() << " into "
               << NumRecords << " records\n";
      });
    for (size_t I = 0; I != NumRecords; ++I)
      G.splitBlock(B, RecordSize, &BCache);
  }

  return Error::success();
}

} // end namespace jitlink
} // end namespace llvm
