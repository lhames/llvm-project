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
      ExecutionSession &ES, HandleRemoveFunction HandleRemove,
      HandleTransferFunction HandleTransfer = HandleTransferFunction())
      : ES(ES), HandleRemove(std::move(HandleRemove)),
        HandleTransfer(std::move(HandleTransfer)) {

    // If HandleTransfer is not supplied then use the default.
    if (!HandleTransfer)
      HandleTransfer = [this](ResourceKey DstKey, ResourceKey SrcKey) {
        transferResources(DstKey, SrcKey);
      };

    ES.registerResourceManager(*this);
  }

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
       BasicDefineAndRemoveWithoutMaterialization) {

  bool ResourceManagerGotRemove = false;
  SimpleResourceManager<> SRM(ES, [&](ResourceKey K) -> Error {
    ES.runSessionLocked([&]() {
      ResourceManagerGotRemove = true;
      EXPECT_EQ(SRM.getRecordedResources().size(), 0U)
          << "Unexpected resources recorded";
      SRM.removeResource(K);
    });
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

  EXPECT_TRUE(ResourceManagerGotRemove)
      << "ResourceManager did not receive handleRemoveResources";
  EXPECT_TRUE(MaterializationUnitDestroyed)
      << "MaterializationUnit not destroyed in response to removal";
}

TEST_F(ResourceTrackerStandardTest, BasicDefineAndRemoveWithMaterialization) {

  bool ResourceManagerGotRemove = false;
  SimpleResourceManager<> SRM(ES, [&](ResourceKey K) -> Error {
    ES.runSessionLocked([&]() {
      ResourceManagerGotRemove = true;
      EXPECT_EQ(SRM.getRecordedResources().size(), 1U)
          << "Unexpected number of resources recorded";
      EXPECT_EQ(SRM.getRecordedResources().count(K), 1U)
          << "Unexpected recorded resource";
      SRM.removeResource(K);
    });
    return Error::success();
  });

  bool MaterializationUnitDestroyed = false;
  auto MU = std::make_unique<SimpleMaterializationUnit>(
      SymbolFlagsMap({{Foo, FooSym.getFlags()}}),
      [&](std::unique_ptr<MaterializationResponsibility> R) {
        R->withResourceKeyDo([&](ResourceKey K) { SRM.recordResource(K); });
        cantFail(R->notifyResolved({{Foo, FooSym}}));
        cantFail(R->notifyEmitted());
      },
      nullptr, SimpleMaterializationUnit::DiscardFunction(),
      [&]() { MaterializationUnitDestroyed = true; });

  auto RT = JD.createResourceTracker();
  cantFail(JD.define(std::move(MU), RT));
  cantFail(ES.lookup({&JD}, Foo));
  cantFail(RT->remove());

  EXPECT_TRUE(ResourceManagerGotRemove)
      << "ResourceManager did not receive handleRemoveResources";
  EXPECT_TRUE(MaterializationUnitDestroyed)
      << "MaterializationUnit not destroyed in response to removal";
}

} // namespace
