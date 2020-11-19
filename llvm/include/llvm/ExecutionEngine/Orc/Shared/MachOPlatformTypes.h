//===-- MachOPlatformTypes.h - MachOP types shared with runtime -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MachO types shared with the Orc Runtime.
//
// WARNING: This header should not include wrapper function headers and STL
// headers only. No other LLVM headers should be included. This allows this
// header to be shared with the ORC runtime.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_MACHOPLATFORMTYPES_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_MACHOPLATFORMTYPES_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdString.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdUnorderedMap.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdVector.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {

// FIXME: Break JITTargetAddress out of JITSymbol to avoid this.
using JITTargetAddress = uint64_t;

namespace orc {
namespace shared {

struct SectionExtent {
  SectionExtent() = default;
  SectionExtent(JITTargetAddress StartAddress, JITTargetAddress EndAddress)
      : StartAddress(StartAddress), EndAddress(EndAddress) {}
  JITTargetAddress StartAddress = 0;
  JITTargetAddress EndAddress = 0;
};

struct PerObjectRegisterableSections {
  SectionExtent EHFrame;
  SectionExtent ThreadData;
};

class MachOJITDylibInitializers {

  template <typename ChannelT, typename WireType, typename ConcreteType,
            typename _>
  friend class SerializationTraits;

public:
  using SectionExtentList = std::vector<SectionExtent>;

  using InitSectionsMap = std::unordered_map<std::string, SectionExtentList>;

  /// Default constructor for an empty initialezrs struct.
  /// This is intended for use by deserializers who will overwrite the value
  /// later.
  MachOJITDylibInitializers() = default;

  /// Create a MachOJITDylibInitializers struct for a JITDylib with the given
  /// name.
  MachOJITDylibInitializers(std::string Name, JITTargetAddress MachOHeader)
      : Name(std::move(Name)), MachOHeader(MachOHeader) {}

  const std::string &getName() const { return Name; }

  JITTargetAddress getMachOHeader() const { return MachOHeader; }

  void setObjCImageInfoAddr(JITTargetAddress ObjCImageInfoAddr) {
    assert(!ObjCImageInfoAddr && "ObjCImageInfoAddr already set");
    this->ObjCImageInfoAddr = ObjCImageInfoAddr;
  }

  JITTargetAddress getObjCImageInfoAddr() const { return ObjCImageInfoAddr; }

  const InitSectionsMap &getInitSections() const { return InitSections; }

  void setInitSections(InitSectionsMap InitSections) {
    this->InitSections = std::move(InitSections);
  }

  void addSection(const std::string &Name, shared::SectionExtent Extent) {
    InitSections[Name].push_back(Extent);
  }

private:
  std::string Name;
  JITTargetAddress MachOHeader = 0;
  JITTargetAddress ObjCImageInfoAddr = 0;
  InitSectionsMap InitSections;
};

class MachOJITDylibDeinitializers {};

// FIXME: Initializers should be held in a tree reflecting links-against
// relationships.
using MachOPlatformInitializerSequence = std::vector<MachOJITDylibInitializers>;

using MachOPlatformDeinitializerSequence =
    std::vector<MachOJITDylibDeinitializers>;

using BlobSectionExtent = BlobTuple<BlobTargetAddress, BlobTargetAddress>;

/// Blob serialization for SectionExtent.
template <> class BlobSerializationTraits<BlobSectionExtent, SectionExtent> {
public:
  static size_t size(const SectionExtent &S) {
    return BlobSectionExtent::AsArgList::size(S.StartAddress, S.EndAddress);
  }

  static bool serialize(BlobOutputBuffer &BOB, const SectionExtent &S) {
    return BlobSectionExtent::AsArgList::serialize(BOB, S.StartAddress,
                                                   S.EndAddress);
  }

  static bool deserialize(BlobInputBuffer &BIB, SectionExtent &S) {
    return BlobSectionExtent::AsArgList::deserialize(BIB, S.StartAddress,
                                                     S.EndAddress);
  }
};

using BlobPerObjectRegisterableSections =
    BlobTuple<BlobSectionExtent, BlobSectionExtent>;

/// Blob serialization for PerObjectRegisterableSections.
template <>
class BlobSerializationTraits<BlobPerObjectRegisterableSections,
                              PerObjectRegisterableSections> {
public:
  static size_t size(const PerObjectRegisterableSections &PORS) {
    return BlobPerObjectRegisterableSections::AsArgList::size(PORS.EHFrame,
                                                              PORS.ThreadData);
  }

  static bool serialize(BlobOutputBuffer &BOB,
                        const PerObjectRegisterableSections &PORS) {
    return BlobPerObjectRegisterableSections::AsArgList::serialize(
        BOB, PORS.EHFrame, PORS.ThreadData);
  }

  static bool deserialize(BlobInputBuffer &BIB,
                          PerObjectRegisterableSections &PORS) {
    return BlobPerObjectRegisterableSections::AsArgList::deserialize(
        BIB, PORS.EHFrame, PORS.ThreadData);
  }
};

using BlobSectionExtentList = BlobSequence<BlobSectionExtent>;

using BlobMachOJITDylibInitializers =
    BlobTuple<BlobString, BlobTargetAddress, BlobTargetAddress,
              BlobMap<BlobString, BlobSectionExtentList>>;

template <>
class BlobSerializationTraits<BlobMachOJITDylibInitializers,
                              MachOJITDylibInitializers> {
public:
  static size_t size(const MachOJITDylibInitializers &MJDI) {
    return BlobMachOJITDylibInitializers::AsArgList::size(
        MJDI.getName(), MJDI.getMachOHeader(), MJDI.getObjCImageInfoAddr(),
        MJDI.getInitSections());
  }

  static bool serialize(BlobOutputBuffer &BOB,
                        const MachOJITDylibInitializers &MJDI) {
    return BlobMachOJITDylibInitializers::AsArgList::serialize(
        BOB, MJDI.getName(), MJDI.getMachOHeader(), MJDI.getObjCImageInfoAddr(),
        MJDI.getInitSections());
  }

  static bool deserialize(BlobInputBuffer &BIB,
                          MachOJITDylibInitializers &MJDI) {
    std::string Name;
    JITTargetAddress MachOHeader;
    JITTargetAddress ObjCImageInfoAddr;
    MachOJITDylibInitializers::InitSectionsMap InitSections;
    if (!BlobMachOJITDylibInitializers::AsArgList::deserialize(
            BIB, Name, MachOHeader, ObjCImageInfoAddr, InitSections))
      return false;
    MJDI = MachOJITDylibInitializers(std::move(Name), MachOHeader);
    MJDI.setObjCImageInfoAddr(std::move(ObjCImageInfoAddr));
    MJDI.setInitSections(std::move(InitSections));
    return true;
  }
};

using BlobMachOPlatformInitializerSequence =
    BlobSequence<BlobMachOJITDylibInitializers>;

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_MACHOPLATFORMTYPES_H
