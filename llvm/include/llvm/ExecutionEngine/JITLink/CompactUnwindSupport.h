//===-- CompactUnwindSupport.h - Compact Unwind format support --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compact Unwind format support.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORT_H
#define LLVM_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORT_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"

namespace llvm {
namespace jitlink {

/// Split blocks in an __LD,__compact_unwind section on record boundaries.
/// When this function returns edges within each record are guaranteed to be
/// sorted by offset.
Error splitCompactUnwindBlocks(LinkGraph &G, Section &CompactUnwindSection,
                               size_t RecordSize);

/// Manages compact-unwind blocks across the pass pipeline, from splitting and
/// recording address ranges, to fixing up blocks, to constructing the final
/// unwind-info sections.
class CompactUnwindManager {
protected:

  struct CURecInfo {
    Block *RecBlock = nullptr;
    orc::ExecutorAddrRange CoveredRange;
    bool NeedsDWARF = false;
  };

public:

  template <typename CURecTraits>
  static std::shared_ptr<CompactUnwindManager>
  Create(StringRef CompactUnwindSectionName,
         StringRef FinalUnwindInfoSectionName);

  virtual ~CompactUnwindManager() {}

  /// Return the compact-unwind blocks covering the given range that require
  /// DWARF eh-frames for the range.
  ///
  /// * A "None" result indicates that compact unwind did not fully-cover the
  /// given range, but no records needed DWARF (i.e. the eh-frame should be
  /// kept).
  /// * An empty set indicates that compact unwind did fully cover the given
  /// range, but no compact unwind record needed DWARF (i.e. the eh-frame
  /// can be dropped).
  /// * A non-empty set will contain the compact unwind records that require
  /// the DWARF eh-frame covering the range (i.e. the compact-unwind ranges
  /// should have delta edges added pointing back to the eh-frame record).
  std::optional<SmallVector<Block*>>
  findRecordsNeedingDwarf(orc::ExecutorAddrRange R);

  virtual Error splitAndParseCompactUnwindRecords(LinkGraph &G) = 0;

  virtual void addFDEEdgesToRecordsNeedingDwarf(
      Symbol &FDESymbol, const SmallVector<Block*> &Records) = 0;

protected:

  CompactUnwindManager(StringRef CompactUnwindSectionName)
    : CompactUnwindSectionName(CompactUnwindSectionName) {}

  StringRef CompactUnwindSectionName;
  SmallVector<CURecInfo, 16> ParsedRecords;
};

} // end namespace jitlink
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORT_H
