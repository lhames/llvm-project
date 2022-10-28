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

std::optional<SmallVector<Block*>>
CompactUnwindManager::findRecordsNeedingDwarf(orc::ExecutorAddrRange R) {
  auto CUItr = llvm::upper_bound(ParsedRecords, R.Start,
                                 [](orc::ExecutorAddr Addr,
                                    const CURecInfo &Rec) {
                                   return Addr < Rec.CoveredRange.Start;
                                 });

  if (CUItr == ParsedRecords.begin())
    return std::nullopt;

  // CUItr points to the range with the highest start address less than or
  // equal to R.Start.
  CUItr = std::prev(CUItr);

  // If CUItr ends before R starts then R must start in a gap and is not
  // covered.
  if (CUItr->CoveredRange.End <= R.Start)
    return std::nullopt;

  // Walk subsequent records to find all records that cover R.
  bool ContainsGap = false;
  CURecInfo *LastRec = nullptr;
  SmallVector<Block*> RecordsNeedingDWARF;
  for (; CUItr != ParsedRecords.end() && CUItr->CoveredRange.Start < R.End;
       ++CUItr) {

    // If we encounter a gap in the compact-unwind records or a record that
    // needs the FDE then bail out early.
    if (LastRec && LastRec->CoveredRange.End != CUItr->CoveredRange.Start)
      ContainsGap = true;
    LastRec = &*CUItr;

    if (CUItr->NeedsDWARF)
      RecordsNeedingDWARF.push_back(CUItr->RecBlock);
  }

  // Check that the last record in the list fully covers R.
  if (std::prev(CUItr)->CoveredRange.End < R.End)
    ContainsGap = true;

  // If there were records needing DWARF, or if there were no records but
  // the range was fully covered, then return the (potentially empty)
  // RecordsNeedingDWARF set.
  if (!RecordsNeedingDWARF.empty() || !ContainsGap)
    return RecordsNeedingDWARF;

  // RecordsNeedingDWARF was empty but there was a gap, so return 'None'.
  return std::nullopt;
}

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
