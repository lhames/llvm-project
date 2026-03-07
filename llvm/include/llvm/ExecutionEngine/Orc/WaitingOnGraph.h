//===------ WaitingOnGraph.h - ORC symbol dependence graph ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Defines WaitingOnGraph and related utilities.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_EXECUTIONENGINE_ORC_WAITINGONGRAPH_H
#define LLVM_EXECUTIONENGINE_ORC_WAITINGONGRAPH_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <vector>

namespace llvm::orc::detail {

class WaitingOnGraphTest;

/// WaitingOnGraph class template.
///
/// This type is intended to provide efficient dependence tracking for Symbols
/// in an ORC program.
///
/// WaitingOnGraph models a directed graph with four partitions:
///   1. Not-yet-emitted nodes: Nodes identified as waited-on in an emit
///      operation.
///   2. Emitted nodes: Nodes emitted and waiting on some non-empty set of
///      other nodes.
///   3. Ready nodes: Nodes emitted and not waiting on any other nodes
///      (either because they weren't waiting on any nodes when they were
///      emitted, or because all transitively waited-on nodes have since
///      been emitted).
///   4. Failed nodes: Nodes that have been marked as failed-to-emit, and
///      nodes that were found to transitively wait-on some failed node.
///
/// Nodes are added to the graph by *emit* and *fail* operations.
///
/// The *emit* operation takes a bipartite *local dependence graph* as an
/// argument and returns...
///   a. the set of nodes (both existing and newly added from the local
///      dependence graph) whose waiting-on set is the empty set, and...
///   b. the set of newly added nodes that are found to depend on failed
///      nodes.
///
/// The *fail* operation takes a set of failed nodes and returns the set of
/// Emitted nodes that were waiting on the failed nodes.
///
/// The concrete representation adopts several approaches for efficiency:
///
/// 1. Only *Emitted* and *Not-yet-emitted* nodes are represented explicitly.
///    *Ready* and *Failed* nodes are represented by the values returned by the
///    GetExternalStateFn argument to *emit*.
///
/// 2. Labels are (*Container*, *Element*) pairs that are intended to represent
///    ORC symbols (ORC uses types Container = JITDylib,
///    Element = NonOwningSymbolStringPtr). The internal representation of the
///    graph is optimized on the assumption that there are many more Elements
///    (symbol names) than Containers (JITDylibs) used to construct the labels.
///    (Consider for example the common case where most JIT'd code is placed in
///    a single "main" JITDylib).
///
/// 3. The data structure stores *SuperNodes* which have multiple labels. This
///    reduces the number of nodes and edges in the graph in the common case
///    where many JIT symbols have the same set of dependencies. SuperNodes are
///    coalesced when their dependence sets become equal.
///
/// 4. The *simplify* method can be applied to an initial *local dependence
///    graph* (as a list of SuperNodes) to eliminate any internal dependence
///    relationships that would have to be propagated internally by *emit*.
///    Access to the WaitingOnGraph is assumed to be guarded by a mutex (ORC
///    will access it from multiple threads) so this allows some pre-processing
///    to be performed outside the mutex.
template <typename ContainerIdT, typename ElementIdT> class WaitingOnGraph {
  friend class WaitingOnGraphTest;

public:
  using ContainerId = ContainerIdT;
  using ElementId = ElementIdT;

  class ElementSet : public DenseSet<ElementId> {
    friend class ElementSetTest;

  public:
    using DenseSet<ElementId>::DenseSet;

    /// Merge the elements of Other into this set. Returns true if any new
    /// elements are added.
    bool merge(const ElementSet &Other, bool AssertNoOverlap = false) {
      size_t OrigSize = this->size();
      this->insert(Other.begin(), Other.end());
      assert((!AssertNoOverlap || this->size() == (OrigSize + Other.size())) &&
             "merge of overlapping elements");
      return this->size() != OrigSize;
    }

    /// Remove all elements in Other from this set. Returns true if any
    /// elements were removed.
    bool remove(const ElementSet &Other) {
      size_t OrigSize = this->size();

      // Early out for empty sets.
      if (OrigSize == 0 || Other.empty())
        return false;

      // TODO: Tweak condition to account for SmallVector cost. We may want to
      //       prefer iterating over elements if the size difference is small.
      if (OrigSize > Other.size()) {
        for (auto &Elem : Other)
          this->erase(Elem);
      } else {
        SmallVector<ElementId> ToRemove;
        for (auto &Elem : *this)
          if (Other.count(Elem))
            ToRemove.push_back(Elem);
        for (auto &Elem : ToRemove)
          this->erase(Elem);
      }
      return this->size() < OrigSize;
    }

    /// Remove all elements for which Pred returns true.
    /// Returns true if any elements were removed.
    template <typename Pred> bool remove_if(Pred &&P) {
      if (this->empty())
        return false;

      SmallVector<ElementId> ToRemove;
      for (auto &Elem : *this)
        if (P(Elem))
          ToRemove.push_back(Elem);

      for (auto &Elem : ToRemove)
        this->erase(Elem);

      return !ToRemove.empty();
    }
  };

  class ContainerElementsMap : public DenseMap<ContainerId, ElementSet> {
    friend class ContainerElementsMapTest;

  public:
    using DenseMap<ContainerId, ElementSet>::DenseMap;

    /// Merge the elements of Other into this map. Returns true if any new
    /// elements are added.
    bool merge(const ContainerElementsMap &Other,
               bool AssertNoElementsOverlap = false) {
      bool Changed = false;
      for (auto &[Container, Elements] : Other)
        Changed |= (*this)[Container].merge(Elements, AssertNoElementsOverlap);
      return Changed;
    }

    /// Remove all elements in Other from this map. Returns true if any
    /// elements were removed.
    bool remove(const ContainerElementsMap &Other) {
      bool Changed = false;
      for (auto &[Container, Elements] : Other) {
        assert(!Elements.empty() && "Stale row for Container in Other");
        auto I = this->find(Container);
        if (I == this->end())
          continue;
        Changed |= I->second.remove(Elements);
        if (I->second.empty())
          this->erase(Container);
      }
      return Changed;
    }

    /// Call V on each (Container, Elements) pair in this map.
    ///
    /// V should return true if it modifies any elements.
    ///
    /// Returns true if V returns true for any pair.
    template <typename Visitor> bool visit(Visitor &&V) {
      if (this->empty())
        return false;

      bool Changed = false;
      SmallVector<ContainerId> ToRemove;
      for (auto &[Container, Elements] : *this) {
        assert(!Elements.empty() && "empty row for container");
        if (V(Container, Elements)) {
          Changed = true;
          if (Elements.empty())
            ToRemove.push_back(Container);
        }
      }

      for (auto &Container : ToRemove)
        this->erase(Container);

      return Changed;
    }
  };

  class SuperNode;

private:
  using ElemToSuperNodeMap =
      DenseMap<ContainerId, DenseMap<ElementId, SuperNode *>>;

  struct SuperNodeInfo {
    std::unique_ptr<SuperNode> OwnedSN;
    DenseSet<SuperNode *> DependantSNs;
  };

  using SuperNodeDepsMap = DenseMap<SuperNode *, SuperNodeInfo>;

public:
  class SuperNode {
    friend class WaitingOnGraph;
    friend class WaitingOnGraphTest;

  public:
    SuperNode(ContainerElementsMap Defs, ContainerElementsMap Deps)
        : Defs(std::move(Defs)), Deps(std::move(Deps)) {}
    ContainerElementsMap &defs() { return Defs; }
    const ContainerElementsMap &defs() const { return Defs; }
    ContainerElementsMap &deps() { return Deps; }
    const ContainerElementsMap &deps() const { return Deps; }

  private:
    /// Add a mapping from the Defs in this SuperNode to SN (which may or may
    /// not be the same as this).
    void mapDefsTo(ElemToSuperNodeMap &ElemToSN, SuperNode *SN) {
      assert(!Defs.empty() && "Empty defs!?");
      for (auto &[Container, Elements] : Defs) {
        assert(!Elements.empty() && "Empty elements for container?");
        auto &ContainerElemToSN = ElemToSN[Container];
        for (auto &Elem : Elements)
          ContainerElemToSN[Elem] = SN;
      }
      assert((!SN->RegisteredElemToSN || SN->RegisteredElemToSN == &ElemToSN) &&
             "SN defs split across maps");
      SN->RegisteredElemToSN = &ElemToSN;
    }

    /// Add a mapping from the Defs in this SuperNode to this.
    /// (Equivalent to `SN.mapDefsTo(ElemToSN, &SN);`)
    void mapDefsToThis(ElemToSuperNodeMap &ElemToSN) {
      mapDefsTo(ElemToSN, this);
    }

    /// Remove a mapping from the Defs in this SuperNode from the registered
    /// ElemToSuperNodeMap.
    void unmapDefsFromThis() {
      // If this node's defs aren't mapped then bail out.
      if (!RegisteredElemToSN)
        return;

      for (auto &[Container, Elements] : Defs) {
        auto I = RegisteredElemToSN->find(Container);
        assert(I != RegisteredElemToSN->end() && "Container not in map");
        auto &ContainerElemToSN = I->second;
        for (auto &Elem : Elements) {
          assert(ContainerElemToSN[Elem] == this && "Mapping not present");
          ContainerElemToSN.erase(Elem);
        }
        if (ContainerElemToSN.empty())
          RegisteredElemToSN->erase(I);
      }
      RegisteredElemToSN = nullptr;
    }

    /// For all Defs of this node that are defined by some node in ElemToSN,
    /// remove the Def from this map and add this SuperNode to the list of
    /// dependants of the defining node.
    ///
    /// Returns true if SuperNodeDeps was changed.
    bool hoistDeps(SuperNodeDepsMap &SuperNodeDeps,
                   ElemToSuperNodeMap &ElemToSN) {
      return Deps.visit([&](ContainerId &Container, ElementSet &Elements) {
        auto I = ElemToSN.find(Container);
        if (I == ElemToSN.end())
          return false;

        auto &ContainerElemToSN = I->second;
        return Elements.remove_if([&](const ElementId &Elem) {
          auto J = ContainerElemToSN.find(Elem);
          if (J == ContainerElemToSN.end())
            return false;

          auto *DefSN = J->second;
          if (DefSN != this)
            SuperNodeDeps[DefSN].DependantSNs.insert(this);
          return true;
        });
      });
    }

    /// Partially merge SN into this node for SCC processing. Absorbs SN's
    /// Deps (building the union of external deps for the SCC) and Failed
    /// flag, but defers Defs merging to completeMergeAndRemap/Unmap.
    void preMerge(std::unique_ptr<SuperNode> SN) {
      assert(SN);
      Deps.merge(SN->Deps);
      SN->Deps.clear(); // We're not going to use these any further.
      Failed |= SN->Failed;
      NodesToMerge.push_back(std::move(SN));
    }

    /// Merge SN (and any nodes pre-merged into it) into this SuperNode for
    /// coalescing. Discards Deps (since we only coalesce when deps are
    /// identical).
    void mergeForCoalescing(std::unique_ptr<SuperNode> SN) {
      assert(SN);
      while (!SN->NodesToMerge.empty()) {
        assert(SN->NodesToMerge.back()->NodesToMerge.empty() &&
               "to-merge tree has depth > 1");
        assert(SN->NodesToMerge.back()->Deps.empty() &&
               "pre-merged node has non-empty deps");
        NodesToMerge.push_back(SN->NodesToMerge.pop_back_val());
      }
      SN->Deps.clear();
      NodesToMerge.push_back(std::move(SN));
    }

    // Merge any Defs from pre-merged nodes into this Node's Defs, mapping all
    // Defs to this.
    void completeMergeAndRemap(ElemToSuperNodeMap &ElemToSN) {
      if (!RegisteredElemToSN)
        mapDefsToThis(ElemToSN);
      else
        assert(RegisteredElemToSN == &ElemToSN && "SN defs split across maps");
      while (!NodesToMerge.empty()) {
        auto SN = NodesToMerge.pop_back_val();
        SN->mapDefsTo(ElemToSN, this);
        Defs.merge(std::move(SN->Defs), true);
      }
    }

    /// Merge any Defs from pre-merged nodes into this Node's Defs, unmapping
    /// all Defs from this.
    void completeMergeAndUnmap() {
      unmapDefsFromThis();
      while (!NodesToMerge.empty()) {
        auto SN = NodesToMerge.pop_back_val();
        SN->unmapDefsFromThis();
        Defs.merge(std::move(SN->Defs), true);
      }
    }

    void abandonMapping() { RegisteredElemToSN = nullptr; }

    ContainerElementsMap Defs;
    ContainerElementsMap Deps;

    SmallVector<std::unique_ptr<SuperNode>> NodesToMerge;

    ElemToSuperNodeMap *RegisteredElemToSN = nullptr;

    struct SCCInfo {
      size_t Index = 0;
      size_t LowLink = 0;
      bool OnStack = false;
      SuperNode *Root = nullptr;
    };

    SCCInfo SCC;
    bool Failed = false;
  };

private:
  /// Fast visit with removal.
  ///
  /// Visits the elements of Vec, removing each element for which V returns
  /// true.
  ///
  /// This is O(1) in the number of elements removed, but does not preserve
  /// element order.
  template <typename Vector, typename Visitor>
  static void visitWithRemoval(Vector &Vec, Visitor &&V) {
    for (size_t I = 0; I != Vec.size();) {
      if (V(Vec[I])) {
        if (I != Vec.size() - 1)
          std::swap(Vec[I], Vec.back());
        Vec.pop_back();
      } else
        ++I;
    }
  }

  class Coalescer {
  public:
    std::unique_ptr<SuperNode> addOrCreateSuperNode(ContainerElementsMap Defs,
                                                    ContainerElementsMap Deps) {
      auto H = getHash(Deps);
      if (auto *ExistingSN = findCanonicalSuperNode(H, Deps)) {
        ExistingSN->Defs.merge(Defs, /* AssertNoElementsOverlap */ true);
        return nullptr;
      }

      auto NewSN =
          std::make_unique<SuperNode>(std::move(Defs), std::move(Deps));
      CanonicalSNs[H].push_back(NewSN.get());
      assert(!SNHashes.count(NewSN.get()));
      SNHashes[NewSN.get()] = H;
      return NewSN;
    }

    std::unique_ptr<SuperNode> coalesce(std::unique_ptr<SuperNode> SN,
                                        ElemToSuperNodeMap &ElemToSN) {
      assert(!SNHashes.count(SN.get()) &&
             "Elements of SNs should be new to the coalescer");
      auto H = getHash(SN->Deps);
      if (auto *CanonicalSN = findCanonicalSuperNode(H, SN->Deps)) {
        CanonicalSN->mergeForCoalescing(std::move(SN));
        CanonicalSN->completeMergeAndRemap(ElemToSN);
        return nullptr;
      }
      SN->completeMergeAndRemap(ElemToSN);
      CanonicalSNs[H].push_back(SN.get());
      SNHashes[SN.get()] = H;
      return SN;
    }

    /// Remove all coalescing information.
    ///
    /// This resets the Coalescer to the same functional state that it was
    /// constructed in.
    void clear() {
      CanonicalSNs.clear();
      SNHashes.clear();
    }

    /// Remove the given node from the Coalescer.
    void erase(SuperNode *SN) {
      hash_code H;

      {
        // Look up hash. We expect to find it in SNHashes.
        auto I = SNHashes.find(SN);
        assert(I != SNHashes.end() && "SN not tracked by coalescer");
        H = I->second;
        SNHashes.erase(I);
      }

      // Now remove from CanonicalSNs.
      auto I = CanonicalSNs.find(H);
      assert(I != CanonicalSNs.end() && "Hash not in CanonicalSNs");
      auto &SNs = I->second;

      size_t J = 0;
      for (; J != SNs.size(); ++J)
        if (SNs[J] == SN)
          break;

      assert(J < SNs.size() && "SN not in CanonicalSNs map");
      std::swap(SNs[J], SNs.back());
      SNs.pop_back();

      if (SNs.empty())
        CanonicalSNs.erase(I);
    }

  private:
    hash_code getHash(const ContainerElementsMap &M) {
      SmallVector<ContainerId> SortedContainers;
      SortedContainers.reserve(M.size());
      for (auto &[Container, Elems] : M)
        SortedContainers.push_back(Container);
      llvm::sort(SortedContainers);
      hash_code Hash(0);
      for (auto &Container : SortedContainers) {
        auto &ContainerElems = M.at(Container);
        SmallVector<ElementId> SortedElems(ContainerElems.begin(),
                                           ContainerElems.end());
        llvm::sort(SortedElems);
        Hash = hash_combine(Hash, Container, hash_combine_range(SortedElems));
      }
      return Hash;
    }

    SuperNode *findCanonicalSuperNode(hash_code H,
                                      const ContainerElementsMap &M) {
      for (auto *SN : CanonicalSNs[H])
        if (SN->Deps == M)
          return SN;
      return nullptr;
    }

    DenseMap<hash_code, SmallVector<SuperNode *>> CanonicalSNs;
    DenseMap<SuperNode *, hash_code> SNHashes;
  };

  struct ProcessedResult {
    std::vector<std::unique_ptr<SuperNode>> NewPendingSNs;
    std::vector<std::unique_ptr<SuperNode>> ReadySNs;
    std::vector<std::unique_ptr<SuperNode>> FailedSNs;
  };

public:
  /// Build SuperNodes from (definition-set, dependence-set) pairs.
  ///
  /// Coalesces definition-sets with identical dependence-sets.
  class SuperNodeBuilder {
  public:
    void add(ContainerElementsMap Defs, ContainerElementsMap Deps) {
      if (Defs.empty())
        return;
      Deps.remove(Defs); // Remove any self-reference.
      if (auto SN = C.addOrCreateSuperNode(std::move(Defs), std::move(Deps)))
        SNs.push_back(std::move(SN));
    }
    std::vector<std::unique_ptr<SuperNode>> takeSuperNodes() {
      C.clear();
      return std::move(SNs);
    }

  private:
    Coalescer C;
    std::vector<std::unique_ptr<SuperNode>> SNs;
  };

  class SimplifyResult {
    friend class WaitingOnGraph;
    friend class WaitingOnGraphTest;

  public:
    const std::vector<std::unique_ptr<SuperNode>> &superNodes() const {
      return SNs;
    }

  private:
    SimplifyResult(std::vector<std::unique_ptr<SuperNode>> SNs,
                   ElemToSuperNodeMap ElemToSN)
        : SNs(std::move(SNs)), ElemToSN(std::move(ElemToSN)) {}

    void clear() {
      SNs.clear();
      ElemToSN.clear();
    }

    std::vector<std::unique_ptr<SuperNode>> SNs;
    ElemToSuperNodeMap ElemToSN;
  };

  /// Preprocess a list of SuperNodes to remove all intra-SN dependencies.
  static SimplifyResult simplify(std::vector<std::unique_ptr<SuperNode>> SNs) {
    // Build ElemToSN map.
    ElemToSuperNodeMap ElemToSN;
    for (auto &SN : SNs)
      SN->mapDefsToThis(ElemToSN);

    SuperNodeDepsMap SuperNodeDeps;

    // hoistDeps will build the graph and remove any intra-simplify
    // dependencies.
    hoistDeps(SNs, SuperNodeDeps, ElemToSN);

    // Transfer SN ownership into SuperNodeDepsGraph.
    while (!SNs.empty()) {
      auto SN = std::move(SNs.back());
      SNs.pop_back();
      SuperNodeDeps[SN.get()].OwnedSN = std::move(SN);
    }

    // Identify and merge SCCs and build worklist.
    auto Worklist = mergeSCCsAndBuildWorklist(SuperNodeDeps);

    // Use worklist to propagate deps.
    propagateDeps(std::move(Worklist), SuperNodeDeps);

    // Run nodes through the coalescer and collect.
    Coalescer C;
    for (auto &[_, SNInfo] : SuperNodeDeps)
      if (auto SN = C.coalesce(std::move(SNInfo.OwnedSN), ElemToSN))
        SNs.push_back(std::move(SN));

    return {std::move(SNs), std::move(ElemToSN)};
  }

  struct EmitResult {
    std::vector<std::unique_ptr<SuperNode>> Ready;
    std::vector<std::unique_ptr<SuperNode>> Failed;
  };

  enum class ExternalState { None, Ready, Failed };

  /// Add the given SuperNodes to the graph, returning any SuperNodes that
  /// move to the Ready or Failed states as a result.
  /// The GetExternalState function is used to represent SuperNodes that have
  /// already become Ready or Failed (since such nodes are not explicitly
  /// represented in the graph).
  template <typename GetExternalStateFn>
  EmitResult emit(SimplifyResult SR, GetExternalStateFn &&GetExternalState) {
    // Remove ready dependencies. Mark failed nodes.
    processExternalDeps(SR.SNs, GetExternalState);

    SuperNodeDepsMap SuperNodeDeps;

    // Lift PendingSNs whose dep sets will be modified into SuperNodeDeps.
    visitWithRemoval(PendingSNs, [&](std::unique_ptr<SuperNode> &SN) {
      if (SN->hoistDeps(SuperNodeDeps, SR.ElemToSN)) {
        CoalesceToPendingSNs.erase(SN.get());
        SuperNodeDeps[SN.get()].OwnedSN = std::move(SN);
        return true;
      }
      return false;
    });

    // Lift new SNs into SuperNodeDeps.
    for (auto &SN : SR.SNs) {
      SN->abandonMapping();
      SN->hoistDeps(SuperNodeDeps, ElemToPendingSN);
      SuperNodeDeps[SN.get()].OwnedSN = std::move(SN);
    }

    // TODO: If hoists only happen in one direction or another (new -> pending,
    //       or pending -> new) then we can't have a cycle. In that case it
    //       might be worth bypassing mergeSCCsANdBuildWorklist.

    SR.clear();

    auto Worklist = mergeSCCsAndBuildWorklist(SuperNodeDeps);
    propagateDeps(std::move(Worklist), SuperNodeDeps);

    auto PR = processNodes(std::move(SuperNodeDeps));

    for (auto &NewPendingSN : PR.NewPendingSNs) {
      assert(NewPendingSN);
      if (auto SN = CoalesceToPendingSNs.coalesce(std::move(NewPendingSN),
                                                  ElemToPendingSN))
        PendingSNs.push_back(std::move(SN));
    }

    return {std::move(PR.ReadySNs), std::move(PR.FailedSNs)};
  }

  /// Identify the given symbols as Failed.
  /// The elements of the Failed map will not be included in the returned
  /// result, so clients should take whatever actions are needed to mark
  /// this as failed in their external representation.
  std::vector<std::unique_ptr<SuperNode>>
  fail(const ContainerElementsMap &Failed) {
    std::vector<std::unique_ptr<SuperNode>> FailedSNs;

    visitWithRemoval(PendingSNs, [&](std::unique_ptr<SuperNode> &SN) {
      for (auto &[Container, Elements] : SN->Deps) {
        auto I = Failed.find(Container);
        if (I == Failed.end())
          continue;

        auto &FailedElems = I->second;
        for (auto &Elem : Elements) {
          if (FailedElems.count(Elem)) {
            CoalesceToPendingSNs.erase(SN.get());
            SN->unmapDefsFromThis();
            FailedSNs.push_back(std::move(SN));
            return true;
          }
        }
      }
      return false;
    });

    return FailedSNs;
  }

  bool validate(raw_ostream &Log) {
    bool AllGood = true;
    auto ErrLog = [&]() -> raw_ostream & {
      AllGood = false;
      return Log;
    };

    size_t DefCount = 0;
    for (auto &PendingSN : PendingSNs) {
      if (PendingSN->Deps.empty())
        ErrLog() << "Pending SN " << PendingSN.get() << " has empty dep set.\n";
      else {
        bool BadElem = false;
        for (auto &[Container, Elems] : PendingSN->Deps) {
          auto I = ElemToPendingSN.find(Container);
          if (I == ElemToPendingSN.end())
            continue;
          if (Elems.empty())
            ErrLog() << "Pending SN " << PendingSN.get()
                     << " has dependence map entry for " << Container
                     << " with empty element set.\n";
          for (auto &Elem : Elems) {
            if (I->second.count(Elem)) {
              ErrLog() << "Pending SN " << PendingSN.get()
                       << " has dependence on emitted element ( " << Container
                       << ", " << Elem << ")\n";
              BadElem = true;
              break;
            }
          }
          if (BadElem)
            break;
        }
      }

      for (auto &[Container, Elems] : PendingSN->Defs) {
        if (Elems.empty())
          ErrLog() << "Pending SN " << PendingSN.get()
                   << " has def map entry for " << Container
                   << " with empty element set.\n";
        DefCount += Elems.size();
        auto I = ElemToPendingSN.find(Container);
        if (I == ElemToPendingSN.end())
          ErrLog() << "Pending SN " << PendingSN.get() << " has "
                   << Elems.size() << " defs in container " << Container
                   << " not covered by ElemsToPendingSN.\n";
        else {
          for (auto &Elem : Elems) {
            auto J = I->second.find(Elem);
            if (J == I->second.end())
              ErrLog() << "Pending SN " << PendingSN.get() << " has element ("
                       << Container << ", " << Elem
                       << ") not covered by ElemsToPendingSN.\n";
            else if (J->second != PendingSN.get())
              ErrLog() << "ElemToPendingSN value invalid for (" << Container
                       << ", " << Elem << ")\n";
          }
        }
      }
    }

    size_t DefCount2 = 0;
    for (auto &[Container, Elems] : ElemToPendingSN)
      DefCount2 += Elems.size();

    assert(DefCount2 >= DefCount);
    if (DefCount2 != DefCount)
      ErrLog() << "ElemToPendingSN contains extra elements.\n";

    return AllGood;
  }

private:
  // Replace individual dependencies with supernode dependencies.
  static void hoistDeps(std::vector<std::unique_ptr<SuperNode>> &SNs,
                        SuperNodeDepsMap &SuperNodeDeps,
                        ElemToSuperNodeMap &ElemToSN) {
    // For all SNs...
    for (auto &SN : SNs)
      SN->hoistDeps(SuperNodeDeps, ElemToSN);
  }

  /// Using Tarjan's algorithm, identify SCCs in SuperNodeDeps and merge the
  /// nodes of each SCC into a single root node (this method only merges the
  /// Deps of the nodes. The defs will be merged separately).
  ///
  /// Since all elements of an SCC will end up with the same Deps set this
  /// should speed up propagation.
  ///
  /// Also builds a reverse-DFS worklist that can be used as an optimal ordering
  /// by propagateDeps.
  static SmallVector<SuperNode *>
  mergeSCCsAndBuildWorklist(SuperNodeDepsMap &SuperNodeDeps) {
    // Tarjan's algorithm for SCCs, modified to avoid recursion and coalesce
    // the SCCs.
    size_t Index = 0;
    SmallVector<SuperNode *> Worklist;
    SmallVector<SuperNode *> Stack;

    struct SCCFrame {
      SuperNode *SN = nullptr;
      DenseSet<SuperNode *> *Dependants = nullptr;
      typename DenseSet<SuperNode *>::iterator DepItr;
    };
    SmallVector<SCCFrame> SCCStack;

    auto Visit = [&](SuperNode *SN) {
      assert(!SN->SCC.Index && "SN already visited");
      SN->SCC.Index = SN->SCC.LowLink = ++Index;
      SN->SCC.OnStack = true;
      Stack.push_back(SN);
      SCCStack.push_back({});
      SCCStack.back().SN = SN;
      auto I = SuperNodeDeps.find(SN);
      if (I != SuperNodeDeps.end()) {
        SCCStack.back().Dependants = &I->second.DependantSNs;
        SCCStack.back().DepItr = SCCStack.back().Dependants->begin();
      }
    };

    for (auto &[Root, SNDepInfo] : SuperNodeDeps) {
      if (Root->SCC.Index) // Non-zero index serves as "visited" flag.
        continue;

      Visit(Root);

      while (!SCCStack.empty()) {
        auto &Frame = SCCStack.back();
        auto *SN = Frame.SN;
        if (Frame.Dependants && Frame.DepItr != Frame.Dependants->end()) {
          // If there are any dependants of SN then process them.
          auto *DepSN = *Frame.DepItr++;
          if (!DepSN->SCC.Index)
            Visit(DepSN); // Visit if not visited already.
          else if (DepSN->SCC.OnStack)
            SN->SCC.LowLink = std::min(SN->SCC.LowLink, DepSN->SCC.Index);
        } else {

          // Found an SCC root.
          if (SN->SCC.LowLink == SN->SCC.Index) {
            while (Stack.back() != SN) {
              auto *DepSN = Stack.pop_back_val();
              DepSN->SCC.OnStack = false;
              DepSN->SCC.Root = SN;
            }

            assert(Stack.back() == SN);
            Stack.pop_back();
            SN->SCC.OnStack = false;
            Worklist.push_back(SN);
          }

          SCCStack.pop_back();
          if (!SCCStack.empty())
            SCCStack.back().SN->SCC.LowLink =
                std::min(SCCStack.back().SN->SCC.LowLink, SN->SCC.LowLink);
        }
      }
    }

    // Merge nodes and remap elements of DependantSNs to account for the merge.
    SmallVector<SuperNode *> ToRemove;
    for (auto &[SN, SNInfo] : SuperNodeDeps) {
      if (auto *RootSN = SN->SCC.Root) {
        assert(SNInfo.OwnedSN);
        RootSN->preMerge(std::move(SNInfo.OwnedSN));
        // RootSN is always already in SuperNodeDeps (it was placed there
        // before calling this method), so this won't insert or rehash.
        auto &RootSNInfo = SuperNodeDeps[RootSN];
        for (auto *DepSN : SNInfo.DependantSNs) {
          auto *DepRootSN = DepSN->SCC.Root ? DepSN->SCC.Root : DepSN;
          if (DepRootSN != RootSN)
            RootSNInfo.DependantSNs.insert(DepRootSN);
        }
        ToRemove.push_back(SN);
      } else {
        DenseSet<SuperNode *> NewDependantSNs;

        for (auto &DepSN : SNInfo.DependantSNs) {
          auto *DepRootSN = DepSN->SCC.Root ? DepSN->SCC.Root : DepSN;
          if (DepRootSN != SN)
            NewDependantSNs.insert(DepRootSN);
        }

        SNInfo.DependantSNs = std::move(NewDependantSNs);
        SN->SCC = {};
      }
    }

    // Remove merged nodes.
    for (auto *SN : ToRemove)
      SuperNodeDeps.erase(SN);

    return Worklist;
  }

  /// Propagate deps and failure status through the dependency graph.
  /// The worklist is in reverse topological order (dependencies before
  /// dependants when popped from the back), guaranteeing single-pass
  /// convergence for a DAG.
  static void propagateDeps(SmallVector<SuperNode *> Worklist,
                            SuperNodeDepsMap &SuperNodeDeps) {

    if (Worklist.empty())
      return;

    while (!Worklist.empty()) {
      auto *SN = Worklist.pop_back_val();

      auto I = SuperNodeDeps.find(SN);
      if (I == SuperNodeDeps.end())
        continue;

      auto &DependantSNs = I->second.DependantSNs;
      for (auto &DepSN : DependantSNs) {
        DepSN->Failed |= SN->Failed;
        DepSN->Deps.merge(SN->Deps);
      }
    }
  }

  template <typename GetExternalStateFn>
  static void processExternalDeps(std::vector<std::unique_ptr<SuperNode>> &SNs,
                                  GetExternalStateFn &GetExternalState) {
    for (auto &SN : SNs)
      SN->Deps.visit([&](ContainerId &Container, ElementSet &Elements) {
        return Elements.remove_if([&](ElementId &Elem) {
          switch (GetExternalState(Container, Elem)) {
          case ExternalState::None:
            return false;
          case ExternalState::Ready:
            return true;
          case ExternalState::Failed:
            SN->Failed = true;
            return true;
          }
        });
      });
  }

  /// Returns the tuple of (NewPending, Ready, Failed) nodes.
  ProcessedResult processNodes(SuperNodeDepsMap SuperNodeDeps) {
    ProcessedResult PR;

    for (auto &[SN, SNInfo] : SuperNodeDeps) {

      if (!SNInfo.OwnedSN)
        continue;

      if (SN->Failed) {
        SN->completeMergeAndUnmap();
        PR.FailedSNs.push_back(std::move(SNInfo.OwnedSN));
      } else if (SN->Deps.empty()) {
        SN->completeMergeAndUnmap();
        PR.ReadySNs.push_back(std::move(SNInfo.OwnedSN));
      } else {
        // No complete-merge here: The Coalescer will do that.
        PR.NewPendingSNs.push_back(std::move(SNInfo.OwnedSN));
      }
    }

    return PR;
  }

  std::vector<std::unique_ptr<SuperNode>> PendingSNs;
  ElemToSuperNodeMap ElemToPendingSN;
  Coalescer CoalesceToPendingSNs;
};

} // namespace llvm::orc::detail

#endif // LLVM_EXECUTIONENGINE_ORC_WAITINGONGRAPH_H
