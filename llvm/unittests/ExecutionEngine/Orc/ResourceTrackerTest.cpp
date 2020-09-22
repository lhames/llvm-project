//===------ ResourceTrackerTest.cpp - Unit tests ResourceTracker API
//-------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OrcTestCommon.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/OrcError.h"
#include "llvm/Testing/Support/Error.h"

using namespace llvm;
using namespace llvm::orc;

class ResourceTrackerStandardTest : public CoreAPIsBasedStandardTest {};

namespace {

template <typename ResourceT = unsigned>
class SimpleResourceManager : public ResourceManager {
public:
  using HandleRemoveFunction = unique_function<Error(ResourceKey)>;

  using HandleTransferFunction =
      unique_function<void(ResourceKey, ResourceKey)>;

  using RecordedResourcesMap = DenseMap<ResourceKey, ResourceT>;

  SimpleResourceManager(
      ExecutionSession &ES,
      HandleRemoveFunction HandleRemove = HandleRemoveFunction(),
      HandleTransferFunction HandleTransfer = HandleTransferFunction())
      : ES(ES), HandleRemove(std::move(HandleRemove)),
        HandleTransfer(std::move(HandleTransfer)) {

    // If HandleRemvoe is not supplied then use the default.
    if (!this->HandleRemove)
      this->HandleRemove = [&](ResourceKey K) -> Error {
        ES.runSessionLocked([&] { removeResource(K); });
        return Error::success();
      };

    // If HandleTransfer is not supplied then use the default.
    if (!this->HandleTransfer)
      this->HandleTransfer = [this](ResourceKey DstKey, ResourceKey SrcKey) {
        transferResources(DstKey, SrcKey);
      };

    ES.registerResourceManager(*this);
  }

  SimpleResourceManager(const SimpleResourceManager &) = delete;
  SimpleResourceManager &operator=(const SimpleResourceManager &) = delete;
  SimpleResourceManager(SimpleResourceManager &&) = delete;
  SimpleResourceManager &operator=(SimpleResourceManager &&) = delete;

  ~SimpleResourceManager() { ES.deregisterResourceManager(*this); }

  /// Create an association between the given key and resource.
  void recordResource(ResourceKey K, ResourceT Val = ResourceT()) {
    assert(!Resources.count(K) && "Resource already recorded in this manager");
    Resources[K] = std::move(Val);
  }

  /// Remove the resource associated with K from the map if present.
  void removeResource(ResourceKey K) { Resources.erase(K); }

  /// Transfer resources from DstKey to SrcKey.
  template <typename MergeOp = std::plus<ResourceT>>
  void transferResources(ResourceKey DstKey, ResourceKey SrcKey,
                         MergeOp Merge = MergeOp()) {
    auto SI = Resources.find(SrcKey);
    assert(SI != Resources.end() && "No resource associated with SrcKey");
    auto &DstResourceRef = Resources[DstKey];
    ResourceT DstResources;
    std::swap(DstResourceRef, DstResources);
    DstResourceRef = Merge(std::move(DstResources), std::move(SI->second));
    Resources.erase(SI);
  }

  /// Return a reference to the Resources map.
  RecordedResourcesMap &getRecordedResources() { return Resources; }
  const RecordedResourcesMap &getRecordedResources() const { return Resources; }

  Error handleRemoveResources(ResourceKey K) override {
    return HandleRemove(K);
  }

  void handleTransferResources(ResourceKey DstKey,
                               ResourceKey SrcKey) override {
    HandleTransfer(DstKey, SrcKey);
  }

  static void transferNotAllowed(ResourceKey DstKey, ResourceKey SrcKey) {
    llvm_unreachable("Resource transfer not allowed");
  }

private:
  ExecutionSession &ES;
  HandleRemoveFunction HandleRemove;
  HandleTransferFunction HandleTransfer;
  RecordedResourcesMap Resources;
};

TEST_F(ResourceTrackerStandardTest,
       BasicDefineAndRemoveAllBeforeMaterializing) {

  bool ResourceManagerGotRemove = false;
  SimpleResourceManager<> SRM(ES, [&](ResourceKey K) -> Error {
    ResourceManagerGotRemove = true;
    EXPECT_EQ(SRM.getRecordedResources().size(), 0U)
        << "Unexpected resources recorded";
    SRM.removeResource(K);
    return Error::success();
  });

  bool MaterializationUnitDestroyed = false;
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](std::unique_ptr<MaterializationResponsibility> R) {
        llvm_unreachable("Never called");
      },
      nullptr, SimpleMaterializationUnit::DiscardFunction(),
      [&]() { MaterializationUnitDestroyed = true; });

  auto RT = JD.createResourceTracker();
  cantFail(JD.define(std::move(MU), RT));
  cantFail(RT->remove());
  auto SymFlags = cantFail(JD.lookupFlags(
      LookupKind::Static, JITDylibLookupFlags::MatchExportedSymbolsOnly,
      SymbolLookupSet(Foo)));

  EXPECT_EQ(SymFlags.size(), 0U)
      << "Symbols should have been removed from the symbol table";
  EXPECT_TRUE(ResourceManagerGotRemove)
      << "ResourceManager did not receive handleRemoveResources";
  EXPECT_TRUE(MaterializationUnitDestroyed)
      << "MaterializationUnit not destroyed in response to removal";
}

TEST_F(ResourceTrackerStandardTest, BasicDefineAndRemoveAllAfterMaterializing) {

  bool ResourceManagerGotRemove = false;
  SimpleResourceManager<> SRM(ES, [&](ResourceKey K) -> Error {
    ResourceManagerGotRemove = true;
    EXPECT_EQ(SRM.getRecordedResources().size(), 1U)
        << "Unexpected number of resources recorded";
    EXPECT_EQ(SRM.getRecordedResources().count(K), 1U)
        << "Unexpected recorded resource";
    SRM.removeResource(K);
    return Error::success();
  });

  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](std::unique_ptr<MaterializationResponsibility> R) {
        R->withResourceKeyDo([&](ResourceKey K) { SRM.recordResource(K); });
        cantFail(R->notifyResolved({{Foo, FooSym}}));
        cantFail(R->notifyEmitted());
      });

  auto RT = JD.createResourceTracker();
  cantFail(JD.define(std::move(MU), RT));
  cantFail(ES.lookup({&JD}, Foo));
  cantFail(RT->remove());
  auto SymFlags = cantFail(JD.lookupFlags(
      LookupKind::Static, JITDylibLookupFlags::MatchExportedSymbolsOnly,
      SymbolLookupSet(Foo)));

  EXPECT_EQ(SymFlags.size(), 0U)
      << "Symbols should have been removed from the symbol table";
  EXPECT_TRUE(ResourceManagerGotRemove)
      << "ResourceManager did not receive handleRemoveResources";
}

TEST_F(ResourceTrackerStandardTest, BasicDefineAndRemoveAllWhileMaterializing) {

  bool ResourceManagerGotRemove = false;
  SimpleResourceManager<> SRM(ES, [&](ResourceKey K) -> Error {
    ResourceManagerGotRemove = true;
    EXPECT_EQ(SRM.getRecordedResources().size(), 0U)
        << "Unexpected resources recorded";
    SRM.removeResource(K);
    return Error::success();
  });

  std::unique_ptr<MaterializationResponsibility> MR;
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](std::unique_ptr<MaterializationResponsibility> R) {
        MR = std::move(R);
      });

  auto RT = JD.createResourceTracker();
  cantFail(JD.define(std::move(MU), RT));

  ES.lookup(
      LookupKind::Static, makeJITDylibSearchOrder(&JD), SymbolLookupSet(Foo),
      SymbolState::Ready,
      [](Expected<SymbolMap> Result) {
        EXPECT_THAT_EXPECTED(Result, Failed<FailedToMaterialize>())
            << "Lookup failed unexpectedly";
      },
      NoDependenciesToRegister);

  cantFail(RT->remove());
  auto SymFlags = cantFail(JD.lookupFlags(
      LookupKind::Static, JITDylibLookupFlags::MatchExportedSymbolsOnly,
      SymbolLookupSet(Foo)));

  EXPECT_EQ(SymFlags.size(), 0U)
      << "Symbols should have been removed from the symbol table";
  EXPECT_TRUE(ResourceManagerGotRemove)
      << "ResourceManager did not receive handleRemoveResources";

  // TODO: Plumb ResourceTrackerDefunct error, test for that explicitly below
  // (rather than just "failed")..
  EXPECT_THAT_ERROR(MR->notifyResolved({{Foo, FooSym}}), Failed())
      << "notifyResolved on MR with remove tracker should have failed";

  MR->failMaterialization();
}

TEST_F(ResourceTrackerStandardTest, JITDylibClear) {
  SimpleResourceManager<> SRM(ES);

  // Add materializer for Foo.
  cantFail(JD.define(std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](std::unique_ptr<MaterializationResponsibility> R) {
        R->withResourceKeyDo(
            [&](ResourceKey K) { ++SRM.getRecordedResources()[K]; });
        cantFail(R->notifyResolved({{Foo, FooSym}}));
        cantFail(R->notifyEmitted());
      })));

  // Add materializer for Bar.
  cantFail(JD.define(std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Bar, BarSym.getFlags()}}),
      [&](std::unique_ptr<MaterializationResponsibility> R) {
        R->withResourceKeyDo(
            [&](ResourceKey K) { ++SRM.getRecordedResources()[K]; });
        cantFail(R->notifyResolved({{Bar, BarSym}}));
        cantFail(R->notifyEmitted());
      })));

  EXPECT_TRUE(SRM.getRecordedResources().empty())
      << "Expected no resources recorded yet.";

  cantFail(
      ES.lookup(makeJITDylibSearchOrder(&JD), SymbolLookupSet({Foo, Bar})));

  auto JDResourceKey = JD.getDefaultResourceTracker()->getKeyUnsafe();
  EXPECT_EQ(SRM.getRecordedResources().size(), 1U)
      << "Expected exactly one entry (for JD's ResourceKey)";
  EXPECT_EQ(SRM.getRecordedResources().count(JDResourceKey), 1U)
      << "Expected an entry for JD's ResourceKey";
  EXPECT_EQ(SRM.getRecordedResources()[JDResourceKey], 2U)
      << "Expected value of 2 for JD's ResourceKey "
         "(+1 for each of Foo and Bar)";

  cantFail(JD.clear());

  EXPECT_TRUE(SRM.getRecordedResources().empty())
      << "Expected no resources recorded after clear";
}

} // namespace
