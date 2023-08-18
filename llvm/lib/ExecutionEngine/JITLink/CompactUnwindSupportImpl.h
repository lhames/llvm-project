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

#include "llvm/ADT/STLExtras.h"
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
    uint32_t Size = 0;
    uint32_t Encoding = 0;
    Symbol *LSDA = nullptr;
    Symbol *FDE = nullptr;
  };

  static constexpr uint32_t UNWIND_IS_NOT_FUNCTION_START = 0x80000000;
  static constexpr uint32_t UNWIND_HAS_LSDA              = 0x40000000;
  static constexpr uint32_t UNWIND_PERSONALITY_MASK      = 0x30000000;

  static constexpr size_t MaxPersonalities = 4;
  static constexpr size_t PersonalityShift = 28;

  static constexpr size_t UnwindInfoSectionHeaderSize = 4 * 7;

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

  /// This must be called prior to graph allocation. It reserves space for the
  /// unwind info block, and marks the compact unwind section as no-alloc.
  Error reserveUnwindInfoBlock(LinkGraph &G) {
    // Bail out early if no unwind info.
    Section *CUSec = G.findSectionByName(CompactUnwindSectionName);
    if (!CUSec)
      return Error::success();

    CUSec->setMemLifetimePolicy(orc::MemLifetimePolicy::NoAlloc);

    // Error out if there's already unwind-info in the graph: We have no idea
    // how to merge more.
    if (G.findSectionByName(UnwindInfoSectionName))
      return make_error<JITLinkError>("In " + G.getName() + ", " +
                                      UnwindInfoSectionName +
                                      " already exists");

    // Find the JITDylib base address so that we can calculate the offsets.
    DSOHandle = G.findExternalSymbolByName("___dso_handle");
    if (!DSOHandle)
      DSOHandle = &G.addExternalSymbol("___dso_handle", 0, false);

    // Count the number of LSDAs.
    size_t NumLSDAs = 0;
    for (auto *B : CUSec->blocks()) {
      for (auto &E : B->edges())
        if (E.getOffset() == CURecTraits::LSDAFieldOffset)
          ++NumLSDAs;
    }

    // Calculate the size of unwind-info.
    size_t NumEntries = CUSec->blocks_size();
    size_t NumSecondLevelPages = numSecondLevelPagesRequired(NumEntries);

    size_t UnwindInfoSectionSize =
      UnwindInfoSectionHeaderSize + // size of unwind info section header.
      MaxPersonalities * PersonalityEntrySize + // max number of personalities.
      NumSecondLevelPages * IndexEntrySize + // index array size.
      NumSecondLevelPages * SecondLevelPageHeaderSize + // 2nd level page hdrs.
      NumEntries * SecondLevelPageEntrySize + // 2nd level page entries.
      NumLSDAs * LSDAEntrySize; // LSDA entries.

    LLVM_DEBUG({
      dbgs() << "In " << G.getName() << ", reserving "
             << formatv("{0:x}", UnwindInfoSectionSize)
             << " bytes for " << UnwindInfoSectionName << "\n";
    });

    // Create the unwind-info section and reserve space for it.
    Section &UnwindInfoSec =
      G.createSection(UnwindInfoSectionName, orc::MemProt::Read);

    auto UnwindInfoSectionContent = G.allocateBuffer(UnwindInfoSectionSize);
    memset(UnwindInfoSectionContent.data(), 0, UnwindInfoSectionContent.size());
    G.createMutableContentBlock(UnwindInfoSec, UnwindInfoSectionContent,
                                orc::ExecutorAddr(), 8, 0);
    return Error::success();
  }

  Error translateToUnwindInfo(LinkGraph &G) {
    Section *CUSec = G.findSectionByName(CompactUnwindSectionName);
    if (!CUSec)
      return Error::success();

    Section *UnwindInfoSec = G.findSectionByName(UnwindInfoSectionName);
    if (!UnwindInfoSec)
      return make_error<JITLinkError>("In " + G.getName() + ", " +
                                      UnwindInfoSectionName +
                                      " missing after allocation");

    if (UnwindInfoSec->blocks_size() != 1)
      return make_error<JITLinkError>(
          "In " + G.getName() + ", " + UnwindInfoSectionName +
          " contains more than one block post-allocation");

    SmallVector<Symbol*, MaxPersonalities> Personalities;
    SmallVector<UnwindEntry> Entries;
    if (auto Err = prepareEntries(Entries, Personalities, G, *CUSec))
      return Err;

    return writeUnwindInfo(Entries, Personalities, G, *UnwindInfoSec, *CUSec);
  }

private:

  // Calculate the size of unwind-info.
  static constexpr size_t PersonalityEntrySize = 4;
  static constexpr size_t IndexEntrySize = 3 * 4;
  static constexpr size_t LSDAEntrySize = 2 * 4;
  static constexpr size_t SecondLevelPageSize = 4096;
  static constexpr size_t SecondLevelPageHeaderSize = 8;
  static constexpr size_t SecondLevelPageEntrySize = 8;
  static constexpr size_t NumFunctionsPerSecondLevelPage =
    (SecondLevelPageSize - SecondLevelPageHeaderSize) /
    SecondLevelPageEntrySize;

  size_t numSecondLevelPagesRequired(size_t NumEntries) {
    return (NumEntries + NumFunctionsPerSecondLevelPage - 1) /
      NumFunctionsPerSecondLevelPage;
  }

  Error prepareEntries(SmallVectorImpl<UnwindEntry> &Entries,
                       SmallVectorImpl<Symbol*> &Personalities,
                       LinkGraph &G, Section &CUSec) {

    // Loop over compact unwind blocks to find the list of entries.
    SmallVector<UnwindEntry> NonUniqued;
    NonUniqued.reserve(CUSec.blocks_size());

    for (auto *B : CUSec.blocks()) {
      UnwindEntry Entry;

      for (auto &E : B->edges()) {
        switch (E.getOffset()) {
        case CURecTraits::FnFieldOffset:
          // This could be the function-pointer, or the FDE keep-alive. Check
          // the type to decide.
          if (E.getKind() == Edge::KeepAlive)
            Entry.FDE = &E.getTarget();
          else
            Entry.Fn = &E.getTarget();
          break;
        case CURecTraits::PersonalityFieldOffset: {
          // Add the Personality to the Personalities map and update the
          // encoding.
          size_t PersonalityIdx = 0;
          for (; PersonalityIdx != Personalities.size(); ++PersonalityIdx)
            if (Personalities[PersonalityIdx] == &E.getTarget())
              break;
          if (PersonalityIdx == MaxPersonalities)
            return make_error<JITLinkError>(
                "In " + G.getName() +
                ", __compact_unwind contains too many personalities (max "
                + formatv("{}", MaxPersonalities) + ")");
          if (PersonalityIdx == Personalities.size())
            Personalities.push_back(&E.getTarget());

          Entry.Encoding |= PersonalityIdx << PersonalityShift;
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

      NonUniqued.push_back(Entry);
    }

    // Sort and unique the entries.
    llvm::sort(Entries, [](const UnwindEntry &LHS, const UnwindEntry &RHS) {
      return LHS.Fn->getAddress() < RHS.Fn->getAddress();
    });

    SmallVector<UnwindEntry> Uniqued;
    Uniqued.reserve(NonUniqued.size());

    Uniqued.push_back(Entries.front());
    for (size_t I = 1; I != Entries.size(); ++I) {
      auto &Next = Entries[I];
      auto &Last = Uniqued.back();

      bool NextNeedsDWARF = CURecTraits::encodingSpecifiesDWARF(Next.Encoding);
      bool CannotBeMerged = CURecTraits::encodingCannotBeMerged(Next.Encoding);
      if (NextNeedsDWARF || (Next.Encoding != Last.Encoding) ||
          CannotBeMerged || Next.LSDA || Last.LSDA)
        Uniqued.push_back(Next);
    }

    LLVM_DEBUG({
      dbgs() << "    Adding " << Uniqued.size()
             << " unwind info entries (removed "
             << (Entries.size() - Uniqued.size()) << " duplicates)\n";
    });

    return Error::success();
  }

  Error makePersonalityRangeError(LinkGraph &G, Section &Sec, Symbol &PSym) {
    std::string ErrMsg;
    {
      raw_string_ostream ErrStream(ErrMsg);
      ErrStream << "In " << G.getName() << " " << Sec.getName()
                << ", personality ";
      if (PSym.hasName())
        ErrStream << PSym.getName() << " ";
      ErrStream << "at " << PSym.getAddress()
                << " is out of 32-bit delta range from __dso_handle at "
                << DSOHandle->getAddress();
    }
    return make_error<JITLinkError>(std::move(ErrMsg));
  }

  Error writeUnwindInfo(SmallVectorImpl<UnwindEntry> &Entries,
                        SmallVectorImpl<Symbol*> &Personalities,
                        LinkGraph &G, Section &UnwindInfoSec, Section &CUSec) {

    auto Content = (*UnwindInfoSec.blocks().begin())->getMutableContent(G);
    BinaryStreamWriter Writer({ reinterpret_cast<uint8_t*>(Content.data()),
        Content.size() }, CURecTraits::Endianness);

    // unwind_info_section_header from
    // /usr/include/mach-o/compact_unwind_encoding.h:
    struct unwind_info_section_header {
      uint32_t    version;
      uint32_t    commonEncodingsArraySectionOffset;
      uint32_t    commonEncodingsArrayCount;
      uint32_t    personalityArraySectionOffset;
      uint32_t    personalityArrayCount;
      uint32_t    indexSectionOffset;
      uint32_t    indexCount;
      // compact_unwind_encoding_t[]
      // uint32_t personalities[]
      // unwind_info_section_header_index_entry[]
      // unwind_info_section_header_lsda_index_entry[]
    };

    uint32_t NumPersonalities = Personalities.size();
    auto NumIndexEntries = numSecondLevelPagesRequired(Entries.size());
    if (!isUInt<32>(NumIndexEntries))
      return make_error<JITLinkError>(
        "In " + G.getName() + " " + UnwindInfoSec.getName() +
        " too many 2nd level page entries (>2^32)");

    unwind_info_section_header Hdr = {
      .version = 1,
      .commonEncodingsArraySectionOffset = sizeof(Hdr),
      .commonEncodingsArrayCount = 0,
      .personalityArraySectionOffset = sizeof(Hdr),
      .personalityArrayCount = NumPersonalities,
      .indexSectionOffset =
          static_cast<uint32_t>(
              sizeof(Hdr) + NumPersonalities * PersonalityEntrySize),
      .indexCount = static_cast<uint32_t>(NumIndexEntries)
    };

    // Write __unwind_info header.
    cantFail(Writer.writeInteger(Hdr.version));
    cantFail(Writer.writeInteger(Hdr.commonEncodingsArraySectionOffset));
    cantFail(Writer.writeInteger(Hdr.commonEncodingsArrayCount));
    cantFail(Writer.writeInteger(Hdr.personalityArraySectionOffset));
    cantFail(Writer.writeInteger(Hdr.personalityArrayCount));
    cantFail(Writer.writeInteger(Hdr.indexSectionOffset));
    cantFail(Writer.writeInteger(Hdr.indexCount));

    // Skip common encodings: JITLink doesn't use them yet.

    // Write personalities.
    for (auto *PSym : Personalities) {
      auto Delta = PSym->getAddress() - DSOHandle->getAddress();
      if (!isUInt<32>(Delta))
        return makePersonalityRangeError(G, UnwindInfoSec, *PSym);
      cantFail(Writer.writeInteger<uint32_t>(Delta));
    }

    // Calculate the offset to the LSDAs.
    size_t SectionOffsetToLSDAs =
      Writer.getOffset() + NumIndexEntries * IndexEntrySize;

    // Count the LSDAs.
    size_t NumLSDAs = 0;
    for (auto &Entry : Entries)
      if (Entry.LSDA)
        ++NumLSDAs;

    // Find the offset to the 1st second-level page.
    size_t SectionOffsetToNextSecondLevelPage =
      SectionOffsetToLSDAs + NumLSDAs * LSDAEntrySize;

    size_t NextLSDAIdx = 0;
    for (size_t I = 0; I != NumIndexEntries; ++I) {
      auto FnAddr =
        Entries[I * NumFunctionsPerSecondLevelPage].Fn->getAddress();
      auto FnDelta = FnAddr - DSOHandle->getAddress();
      cantFail(Writer.writeInteger<uint32_t>(FnDelta));
      auto PageDelta =
        SectionOffsetToNextSecondLevelPage + I * SecondLevelPageSize;
      cantFail(Writer.writeInteger<uint32_t>(PageDelta));

      
    }

    return Error::success();
  }


  StringRef CompactUnwindSectionName;
  StringRef UnwindInfoSectionName;
  StringRef EHFrameSectionName;
  Symbol *DSOHandle;
};

} // end namespace jitlink
} // end namespace llvm

#undef DEBUG_TYPE

#endif // LIB_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORTIMPL_H
