//===-- MachOPlatform.h - Utilities for executing MachO in Orc --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities for executing JIT'd MachO in Orc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_MACHOPLATFORM_H
#define LLVM_EXECUTIONENGINE_ORC_MACHOPLATFORM_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/Shared/MachOPlatformTypes.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/TargetProcessControl.h"
#include "llvm/ExecutionEngine/Orc/WrapperFunctionManager.h"

#include <future>
#include <thread>
#include <vector>

namespace llvm {
namespace orc {

/// Mediates between MachO initialization and ExecutionSession state.
class MachOPlatform : public Platform {
public:
  using RuntimeSupportWrapperPlatformMethod =
      shared::WrapperFunctionResult (MachOPlatform::*)(ArrayRef<char>);

  using RuntimeSupportWrapperFunctionsMap =
      DenseMap<SymbolStringPtr, WrapperFunctionManager::WrapperFunction>;

  using InitializerSequence = shared::MachOPlatformInitializerSequence;
  using DeinitializerSequence = shared::MachOPlatformDeinitializerSequence;

  /// Try to create a MachOPlatform instance, adding the ORC runtime to the
  /// given JITDylib.
  ///
  /// The ORC runtime requires access to a number of symbols in libc++, and
  /// requires access to symbols in libobjc, and libswiftCore to support
  /// Objective-C and Swift code. It is up to the caller to ensure that the
  /// requried symbols can be referenced by code added to PlatformJD. The
  /// standard way to achieve this is to first attach dynamic library search
  /// generators for either the given process, or for the specific required
  /// libraries, to PlatformJD, then to create the platform instance:
  ///
  /// \code{.cpp}
  ///   auto &PlatformJD = ES.createBareJITDylib("stdlib");
  ///   PlatformJD.addGenerator(
  ///     ExitOnErr(TPCDynamicLibrarySearchGenerator
  ///                 ::GetForTargetProcess(TPC)));
  ///   ES.setPlatform(
  ///     ExitOnErr(MachOPlatform::Create(ES, ObjLayer, TPC, PlatformJD,
  ///                                     "/path/to/orc/runtime")));
  /// \endcode
  ///
  /// Alternatively, these symbols could be added to another JITDylib that
  /// PlatformJD links against.
  ///
  /// Clients are also responsible for ensuring that any JIT'd code that
  /// depends on runtime functions (including any code using TLV or static
  /// destructors) can reference the runtime symbols. This is usually achieved
  /// by linking any JITDylibs containing regular code against
  /// PlatformJD.
  ///
  /// By default, MachOPlatform will add the set of aliases returned by the
  /// standardPlatformAliases function. This includes both required aliases
  /// (e.g. __cxa_atexit -> __orc_rt_macho_cxa_atexit for static destructor
  /// support), and optional aliases that provide JIT versions of common
  /// functions (e.g. dlopen -> __orc_rt_macho_jit_dlopen). Clients can
  /// override these defaults by passing a non-None value for the
  /// RuntimeAliases function, in which case the client is responsible for
  /// setting up all aliases (including the required ones).
  static Expected<std::unique_ptr<MachOPlatform>>
  Create(ExecutionSession &ES, ObjectLinkingLayer &ObjLinkingLayer,
         TargetProcessControl &TPC, JITDylib &PlatformJD,
         const char *OrcRuntimePath,
         Optional<SymbolAliasMap> RuntimeAliases = None);

  ExecutionSession &getExecutionSession() const { return ES; }

  ObjectLinkingLayer &getObjectLinkingLayer() const { return ObjLinkingLayer; }

  const Triple &getTargetTriple() const { return TPC.getTargetTriple(); }

  Error shutdown() override;

  Error setupJITDylib(JITDylib &JD) override;
  Error notifyAdding(ResourceTracker &RT,
                     const MaterializationUnit &MU) override;

  /// Return the initializer sequence required to initialize the given JITDylib
  /// (and any uninitialized dependencies).
  Expected<InitializerSequence> getInitializerSequence(JITDylib &JD);

  /// Return the deinitializer sequence required to deinitialize the given
  /// JITDylib.
  Expected<DeinitializerSequence> getDeinitializerSequence(JITDylib &JD);

  /// Return the result of a dlsym style lookup based on the given dso_handle
  /// and symbol name.
  Expected<JITTargetAddress> dlsymLookup(JITTargetAddress DSOHandle,
                                         StringRef Symbol);

  /// Wrapper for jit_dlopen suitable for use with runWrapper dispatch.
  shared::WrapperFunctionResult
  rt_getInitializerSequenceWrapper(ArrayRef<char> ArgBuffer);

  /// Wrapper for jit_dlsym suitable for use with runWrapper dispatch.
  shared::WrapperFunctionResult rt_dlsymLookupWrapper(ArrayRef<char> ArgBuffer);

  /// Returns a WrapperFunctionManager::WrapperFunction wrapping the given
  /// method (invoking it on 'this').
  WrapperFunctionManager::WrapperFunction
  runtimeSupportMethod(RuntimeSupportWrapperPlatformMethod M) {
    return
        [this, M](ArrayRef<char> ArgBuffer) { return (this->*M)(ArgBuffer); };
  }

  /// Returns an AliasMap containing the default aliases for the MachOPlatform.
  /// This can be modified by clients when constructing the platform to add
  /// or remove aliases.
  static SymbolAliasMap standardPlatformAliases(ExecutionSession &ES);

  /// Returns the array of required CXX aliases.
  static ArrayRef<std::pair<const char *, const char *>> requiredCXXAliases();

  /// Returns the array of standard runtime utility aliases for MachO.
  static ArrayRef<std::pair<const char *, const char *>>
  standardRuntimeUtilityAliases();

private:
  // The MachOPlatformPlugin scans and manipulates LinkGraphs to enable the
  // following MachO features:
  //
  //   - Identification and reporting to MachOPlatform of of __mod_init_func,
  //     __objc_classlist, and __sel_ref sections. This is used by MachOPlatform
  //     to enable execution of initializers and registration with the ObjC
  //     runtime.
  //
  //   - Recording of __thread_vars sections. This is used by MachOPlatform to
  //     facilitate thread local variable initialization.
  //
  //   - Lowering of PCRel32TLV edges. This introduces GOT style entries
  //     pointing to TLV descriptors.
  //
  class MachOPlatformPlugin : public ObjectLinkingLayer::Plugin {
  public:
    MachOPlatformPlugin(MachOPlatform &MP) : MP(MP) {}

    void modifyPassConfig(MaterializationResponsibility &MR,
                          jitlink::LinkGraph &G,
                          jitlink::PassConfiguration &Config) override;

    LocalDependenciesMap getSyntheticSymbolLocalDependencies(
        MaterializationResponsibility &MR) override;

    // FIXME: We should be tentatively tracking scraped sections and discarding
    // if the MR fails.
    Error notifyFailed(MaterializationResponsibility &MR) override {
      return Error::success();
    }

    Error notifyRemovingResources(ResourceKey K) override {
      return Error::success();
    }

    void notifyTransferringResources(ResourceKey DstKey,
                                     ResourceKey SrcKey) override {}

  private:
    using InitSymbolDepMap =
        DenseMap<MaterializationResponsibility *, JITLinkSymbolVector>;

    void addInitializerSupportPasses(MaterializationResponsibility &MR,
                                     jitlink::PassConfiguration &Config);

    void addMachOHeaderSupportPasses(MaterializationResponsibility &MR,
                                     jitlink::PassConfiguration &Config);

    void addEHAndTLVSupportPasses(MaterializationResponsibility &MR,
                                  jitlink::PassConfiguration &Config);

    Error preserveInitSections(jitlink::LinkGraph &G,
                               MaterializationResponsibility &MR);

    Error processObjCImageInfo(jitlink::LinkGraph &G,
                               MaterializationResponsibility &MR);

    Error registerInitSections(jitlink::LinkGraph &G, JITDylib &JD);

    Error fixTLVSectionsAndEdges(jitlink::LinkGraph &G, JITDylib &JD);

    std::mutex PluginMutex;
    MachOPlatform &MP;
    DenseMap<JITDylib *, std::pair<uint32_t, uint32_t>> ObjCImageInfos;
    InitSymbolDepMap InitSymbolDeps;
  };

  static bool supportedTarget(const Triple &TT);

  MachOPlatform(ExecutionSession &ES, ObjectLinkingLayer &ObjLinkingLayer,
                TargetProcessControl &TPC, JITDylib &PlatformJD,
                std::unique_ptr<DefinitionGenerator> OrcRuntimeGenerator,
                Error &Err);

  // Connects JIT-side function tags in the runtime to their implementation
  // functions in MachOPlatform.
  Error associateRuntimeTagsWithJITSideFunctions(JITDylib &PlatformJD);

  // Records the addresses of runtime symbols used by the platform.
  Error bootstrapMachORuntime(JITDylib &PlatformJD);

  Error registerInitInfo(JITDylib &JD, JITTargetAddress ObjCImageInfoAddr,
                         ArrayRef<jitlink::Section *> InitSections);

  Error
  registerPerObjectSections(const shared::PerObjectRegisterableSections &PORS);

  Expected<uint64_t> createPThreadKey();

  ExecutionSession &ES;
  ObjectLinkingLayer &ObjLinkingLayer;
  TargetProcessControl &TPC;

  SymbolStringPtr MachOHeaderStartSymbol;
  std::atomic<bool> RuntimeBootstrapped{false};

  JITTargetAddress orc_rt_macho_platform_bootstrap = 0;
  JITTargetAddress orc_rt_macho_platform_shutdown = 0;
  JITTargetAddress orc_rt_macho_register_object_sections = 0;
  JITTargetAddress orc_rt_macho_create_pthread_key = 0;

  DenseMap<JITDylib *, SymbolLookupSet> RegisteredInitSymbols;

  // The Platform gets its own mutex to avoid locking the whole session when
  // aggregating data from the MachOPlatformPlugin.
  // FIXME: Should this mutex be broken up further?
  std::mutex PlatformMutex;
  DenseMap<JITDylib *, shared::MachOJITDylibInitializers> InitSeqs;
  std::vector<shared::PerObjectRegisterableSections> BootstrapPORSs;

  DenseMap<JITTargetAddress, JITDylib *> HeaderAddrToJITDylib;
  DenseMap<JITDylib *, uint64_t> JITDylibToPThreadKey;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_MACHOPLATFORM_H
