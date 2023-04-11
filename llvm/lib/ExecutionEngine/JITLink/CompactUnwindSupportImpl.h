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
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"

#define DEBUG_TYPE "jitlink"

namespace llvm {
namespace jitlink {

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
};

/// Architecture specific implementation of CompactUnwindManager.
template <typename CURecTraits>
class CompactUnwindManager {
private:

    struct UnwindEntry {
      Symbol *Fn = nullptr;
      Symbol *FDE = nullptr;
      Symbol *LSDA = nullptr;
      Symbol *Personality = nullptr;
      uint32_t Encoding = 0;
    };

public:

  CompactUnwindManager(StringRef CompactUnwindSectionName,
                       StringRef UnwindInfoSectionName,
                       StringRef EHFrameSectionName)
    : CompactUnwindSectionName(CompactUnwindSectionName),
      UnwindInfoSectionName(UnwindInfoSectionName),
      EHFrameSectionName(EHFrameSectionName) {}

  // Split records, add keep-alive edges.
  //
  // This must be called *after* __eh_frame has been processed, as it assumes
  // that eh-frame records have been split up and keep-alive edges have been
  // inserted.
  Error prepareForPrune(LinkGraph &G) {
    LLVM_DEBUG(dbgs() << "Compact unwind preparing for prune:\n");

    Section *CUSec = G.findSectionByName(CompactUnwindSectionName);
    if (!CUSec || CUSec->empty()) {
      LLVM_DEBUG({
        dbgs() << "  No " <<CompactUnwindSectionName << " or section empty.\n";
      });
      return Error::success();
    }

    Section *EHFrameSec = G.findSectionByName(EHFrameSectionName);

    if (auto Err = splitCompactUnwindBlocks(G, *CUSec, CURecTraits::Size))
      return Err;

    LLVM_DEBUG({
        dbgs() << "  Processing " << CUSec->blocks_size() << " blocks in "
               << CompactUnwindSectionName << "\n";
      });
    for (auto *B : CUSec->blocks()) {

      // Find target function edge.
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

      auto &Fn = PCBeginEdge->getTarget();

      if (!Fn.isDefined()) {
        LLVM_DEBUG({
          dbgs() << "In " << CompactUnwindSectionName << " for "
                 << G.getName()
                 << " encountered unexpected pc-edge to undefined symbol "
                 << Fn.getName() << "\n";
        });
        continue;
      } else {
        LLVM_DEBUG({
            dbgs() << "    Found record for target ";
            if (Fn.hasName())
              dbgs() << Fn.getName();
            else
              dbgs() << "<anon @ " << Fn.getAddress();
            dbgs() << ">\n";
          });
      }

      auto PCRangeSize = CURecTraits::readPCRangeSize(B->getContent());
      bool NeedsDWARF =
        CURecTraits::encodingSpecifiesDWARF(
            CURecTraits::readEncoding(B->getContent()));

      auto &CURecSym = G.addAnonymousSymbol(*B, 0, CURecTraits::Size, false,
                                            false);

      bool KeepAliveAlreadyAdded = false;
      if (EHFrameSec) {
        Edge *FDEEdge = nullptr;
        for (auto &E : Fn.getBlock().edges()) {
          if (E.getOffset() == 0 &&
              E.getKind() == Edge::KeepAlive &&
              E.getTarget().isDefined() &&
              &E.getTarget().getBlock().getSection() == EHFrameSec) {
            FDEEdge = &E;
            break;
          }
        }

        if (FDEEdge) {
          // Found an FDE edge. Switch it to point to this CU rec, and add a
          // keep-alive from the CU rec back to the FDE.
          auto &FDE = FDEEdge->getTarget();
          LLVM_DEBUG({
            dbgs()
              << "      Record needs DWARF. Adding KeepAlive edge to FDE at "
              << FDE.getAddress() << "\n";
          });
          FDEEdge->setTarget(CURecSym);
          B->addEdge(Edge::KeepAlive, 0, FDE, 0);
          KeepAliveAlreadyAdded = true;
        } else {
          if (NeedsDWARF)
            return make_error<JITLinkError>(
                "In " + G.getName() + ", compact unwind recard ot " +
                formatv("{0:x}", B->getAddress()) +
                " needs DWARF, but no FDE was found");
        }
      } else {
        if (NeedsDWARF)
          return make_error<JITLinkError>(
              "In " + G.getName() + ", compact unwind recard ot " +
              formatv("{0:x}", B->getAddress()) + " needs DWARF, but no " +
              EHFrameSectionName + " section exists");
      }

      if (!KeepAliveAlreadyAdded) {
        // No FDE edge. We'll need to add a new edge from the function back
        // to the CU record.
        Fn.getBlock().addEdge(Edge::KeepAlive, 0, CURecSym, 0);
      }
    }

    return Error::success();
  }

  Error translateToUnwindInfo(LinkGraph &G) {
    Section *CUSec = G.findSectionByName(CompactUnwindSectionName);
    if (!CUSec)
      return Error::success();

    if (G.findSectionByName(UnwindInfoSectionName))
      return make_error<JITLinkError>("In " + G.getName() + ", " +
                                      UnwindInfoSectionName +
                                      " already exists");
    Section &UnwindInfoSec =
      G.createSection(UnwindInfoSectionName, orc::MemProt::Read);

    Section *EHFrameSec = G.findSectionByName(EHFrameSectionName);

    SmallVector<Symbol*, 4> Personalities;
    SmallVector<UnwindEntry> Entries;

    for (auto *B : CUSec->blocks()) {

      UnwindEntry Entry;

      for (auto &E : B->edges()) {
        switch (E.getOffset()) {
        case CURecTraits::FnFieldOffset:
          Entry.Fn = &E.getTarget();
          break;
        case CURecTraits::EncodingFieldOffset:
          Entry.FDE = &E.getTarget();
          break;
        case CURecTraits::PersonalityFieldOffset: {
          Entry.Personality = &E.getTarget();
          auto I = llvm::find(Personalities, Entry.Personality);
          if (I == Personalities.end())
            Personalities.push_back(Entry.Personality);
          break;
        }
        case CURecTraits::LSDAFieldOffset:
          Entry.LSDA = &E.getTarget();
          break;
        default:
          return make_error<JITLinkError>(
              "In " + G.getName() + ", compact unwind record at " +
              formatv("{0:x}", B->getAddress()) +
              " has unrecognized edge at offset " +
              formatv("{0:x}", E.getOffset()));
        }
      }

      Entries.push_back(Entry);
    }

    if (Personalities.size() > 3)
      return make_error<JITLinkError>(
          "In " + G.getName() + ", " + CompactUnwindSectionName +
          " contains " + Twine(Personalities.size()) +
          " encodings (maximum 4)");

    llvm::sort(Entries, [](const UnwindEntry &LHS, const UnwindEntry &RHS) {
      return LHS.Fn->getAddress() < RHS.Fn->getAddress();
    });

    Entries = removeDuplicates(std::move(Entries));

    
    return Error::success();
  }

  Error applyFixups(LinkGraph &G) {
    return Error::success();
  };

private:

  SmallVector<UnwindEntry> removeDuplicates(SmallVector<UnwindEntry> Entries) {
    assert(!Entries.empty() && "Entries list must be non-empty");
    LLVM_DEBUG(dbgs() << "  Removing duplicate entries...\n");
    SmallVector<UnwindEntry> Uniqued;
    Uniqued.push_back(Entries.front());
    for (size_t I = 1; I != Entries.size(); ++I) {
      auto &Next = Entries[I];
      auto &Last = Uniqued.back();

      bool NextNeedsDWARF = CURecTraits::encodingSpecifiesDWARF(Next.Encoding);
      bool CannotBeMerged = CURecTraits::encodingCannotBeMerged(Next.Encoding);
      if (NextNeedsDWARF || (Next.Encoding != Last.Encoding) ||
          (Next.Personality != Last.Personality) || CannotBeMerged ||
          Next.LSDA || Last.LSDA)
        Uniqued.push_back(Next);
    }

    LLVM_DEBUG({
      dbgs() << "    Removed " << (Entries.size() - Uniqued.size())
             << " duplicate entries.\n";
    });

    return Uniqued;
  }

  StringRef CompactUnwindSectionName;
  StringRef UnwindInfoSectionName;
  StringRef EHFrameSectionName;
};

} // end namespace jitlink
} // end namespace llvm

#undef DEBUG_TYPE

#endif // LIB_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORTIMPL_H
