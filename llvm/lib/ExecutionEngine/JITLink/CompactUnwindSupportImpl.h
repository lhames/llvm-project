//===- CompactUnwindSupportImpl.h - Compact Unwind format impl --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compact Unwind format support implementation details.
//
//===----------------------------------------------------------------------===//

#ifndef LIB_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORTIMPL_H
#define LIB_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORTIMPL_H

#include "llvm/ExecutionEngine/JITLink/CompactUnwindSupport.h"
#include "llvm/Support/Endian.h"

namespace llvm {
namespace jitlink {

struct CompactUnwindRecordEdges {
  Edge *Fn = nullptr;
  Edge *FDE = nullptr;
  Edge *Personality = nullptr;
  Edge *LSDA = nullptr;
};

/// CRTP base for compact unwind traits classes. Automatically provides derived
/// constants.
template <typename CRTPImpl>
struct CompactUnwindTraits {
  static constexpr size_t Size = 3 * CRTPImpl::PointerSize + 2 * 4;
  static constexpr size_t FnFieldOffset = 0;
  static constexpr size_t SizeFieldOffset =
    FnFieldOffset + CRTPImpl::PointerSize;
  static constexpr size_t EncodingFieldOffset = SizeFieldOffset + 4;
  static constexpr size_t PersonalityFieldOffset = EncodingFieldOffset + 4;
  static constexpr size_t LSDAFieldOffset =
    PersonalityFieldOffset + CRTPImpl::PointerSize;

  static uint32_t readPCRangeSize(ArrayRef<char> RecordContent) {
    assert(SizeFieldOffset + 4 <= RecordContent.size() &&
           "Truncated CU record?");
    return support::endian::read32<CRTPImpl::Endianness>(
        RecordContent.data() + SizeFieldOffset);
  }

  static uint32_t readEncoding(ArrayRef<char> RecordContent) {
    assert(EncodingFieldOffset + 4 <= RecordContent.size() &&
           "Truncated CU record?");
    return support::endian::read32<CRTPImpl::Endianness>(
        RecordContent.data() + EncodingFieldOffset);
  }

  static Expected<CompactUnwindRecordEdges> getEdges(LinkGraph &G, Block &B) {
    CompactUnwindRecordEdges Edges;

    dbgs() << "    -- field offsets: fn = " << FnFieldOffset << ", encoding = "
           << EncodingFieldOffset << ", personality = " << PersonalityFieldOffset
           << ", lsda = " << LSDAFieldOffset << "\n";

    dbgs() << "    -- block edges at: ";
    for (auto &E : B.edges())
      dbgs() << E.getOffset() << " ";
    dbgs() << "\n";

    for (auto &E : B.edges()) {
      switch (E.getOffset()) {
      case FnFieldOffset:
        Edges.Fn = &E;
        break;
      case EncodingFieldOffset:
        Edges.FDE = &E;
        break;
      case PersonalityFieldOffset:
        Edges.Personality = &E;
        break;
      case LSDAFieldOffset:
        Edges.LSDA = &E;
        break;
      default:
        return make_error<JITLinkError>(
            "In " + G.getName() + ", compact unwind record at " +
            formatv("{0:x}", B.getAddress()) +
            " has unrecognized edge at offset " +
            formatv("{0:x}", E.getOffset()));
      }
    }
    return Edges;
  }
};

/// Architecture specific implementation of CompactUnwindManager.
template <typename CURecTraits>
class CompactUnwindManagerImpl : public CompactUnwindManager {
public:
  CompactUnwindManagerImpl(StringRef CompactUnwindSectionName)
    : CompactUnwindManager(CompactUnwindSectionName) {}

  Error splitAndParseCompactUnwindRecords(LinkGraph &G) override {
    auto *CUSec = G.findSectionByName(CompactUnwindSectionName);
    if (!CUSec)
      return Error::success();

    if (auto Err = splitCompactUnwindBlocks(G, *CUSec, CURecTraits::Size))
      return Err;

    for (auto *B : CUSec->blocks()) {
      Edge *PCBeginEdge = nullptr;
      for (auto &E : B->edges()) {
        if (E.getOffset() == CURecTraits::FnFieldOffset) {
          PCBeginEdge = &E;
          break;
        }
      }

      if (!PCBeginEdge)
        return make_error<JITLinkError>(
            "In " + G.getName() + ", compact unwind record at " +
            formatv("{0:x}", B->getAddress()) + " has no pc-begin edge");

      if (!PCBeginEdge->getTarget().isDefined())
        return make_error<JITLinkError>(
            "In " + G.getName() + ", compact unwind record at " +
            formatv("{0:x}", B->getAddress()) + " points at external symbol " +
            PCBeginEdge->getTarget().getName());

      auto PCBegin = PCBeginEdge->getTarget().getAddress();
      auto PCRangeSize = CURecTraits::readPCRangeSize(B->getContent());
      bool NeedsDWARF = CURecTraits::encodingSpecifiesDWARF(B->getContent());

      ParsedRecords.push_back({B, { PCBegin, PCBegin + PCRangeSize }, NeedsDWARF });
    }

    return Error::success();
  }

  void addFDEEdgesToRecordsNeedingDwarf(
      Symbol &FDESymbol, const SmallVector<Block*> &Records) override {
    for (auto *B : Records)
      CURecTraits::addFDEEdge(FDESymbol, *B);
  }
};

template <typename CURecTraits>
std::shared_ptr<CompactUnwindManager>
CompactUnwindManager::Create(StringRef CompactUnwindSectionName,
                             StringRef FinalUnwindInfoSectionName) {
  return std::make_shared<CompactUnwindManagerImpl<CURecTraits>>(
      CompactUnwindSectionName);
}

} // end namespace jitlink
} // end namespace llvm

#endif // LIB_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORTIMPL_H
