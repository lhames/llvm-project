//===---------- ExecutorSharedMemoryMapperService.cpp -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/TargetProcess/ExecutorSharedMemoryMapperService.h"
#include "llvm/Config/llvm-config.h" // for LLVM_ON_UNIX
#include "llvm/ExecutionEngine/Orc/Shared/OrcRTBridge.h"
#include "llvm/Support/MSVCErrorWorkarounds.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/WindowsError.h"
#include <future>
#include <sstream>

#if defined(LLVM_ON_UNIX)
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#if defined(__MVS__)
#include "llvm/Support/BLAKE3.h"
#include <sys/shm.h>
#endif
#include <unistd.h>
#endif

namespace llvm {
namespace orc {
namespace rt_bootstrap {

#if defined(_WIN32)
static DWORD getWindowsProtectionFlags(MemProt MP) {
  if (MP == MemProt::Read)
    return PAGE_READONLY;
  if (MP == MemProt::Write ||
      MP == (MemProt::Write | MemProt::Read)) {
    // Note: PAGE_WRITE is not supported by VirtualProtect
    return PAGE_READWRITE;
  }
  if (MP == (MemProt::Read | MemProt::Exec))
    return PAGE_EXECUTE_READ;
  if (MP == (MemProt::Read | MemProt::Write | MemProt::Exec))
    return PAGE_EXECUTE_READWRITE;
  if (MP == MemProt::Exec)
    return PAGE_EXECUTE;

  return PAGE_NOACCESS;
}
#endif

Expected<std::pair<ExecutorAddr, std::string>>
ExecutorSharedMemoryMapperService::reserve(uint64_t Size) {
#if (defined(LLVM_ON_UNIX) && !defined(__ANDROID__)) || defined(_WIN32)

#if defined(LLVM_ON_UNIX)

  std::string SharedMemoryName;
  {
    std::stringstream SharedMemoryNameStream;
    SharedMemoryNameStream << "/jitlink_" << sys::Process::getProcessId() << '_'
                           << (++SharedMemoryCount);
    SharedMemoryName = SharedMemoryNameStream.str();
  }

#if defined(__MVS__)
  ArrayRef<uint8_t> Data(
      reinterpret_cast<const uint8_t *>(SharedMemoryName.c_str()),
      SharedMemoryName.size());
  auto HashedName = BLAKE3::hash<sizeof(key_t)>(Data);
  key_t Key = *reinterpret_cast<key_t *>(HashedName.data());
  int SharedMemoryId =
      shmget(Key, Size, IPC_CREAT | IPC_EXCL | __IPC_SHAREAS | 0700);
  if (SharedMemoryId < 0)
    return errorCodeToError(errnoAsErrorCode());

  void *Addr = shmat(SharedMemoryId, nullptr, 0);
  if (Addr == reinterpret_cast<void *>(-1))
    return errorCodeToError(errnoAsErrorCode());
#else
  int SharedMemoryFile =
      shm_open(SharedMemoryName.c_str(), O_RDWR | O_CREAT | O_EXCL, 0700);
  if (SharedMemoryFile < 0)
    return errorCodeToError(errnoAsErrorCode());

  // by default size is 0
  if (ftruncate(SharedMemoryFile, Size) < 0)
    return errorCodeToError(errnoAsErrorCode());

  void *Addr = mmap(nullptr, Size, PROT_NONE, MAP_SHARED, SharedMemoryFile, 0);
  if (Addr == MAP_FAILED)
    return errorCodeToError(errnoAsErrorCode());

  close(SharedMemoryFile);
#endif

#elif defined(_WIN32)

  std::string SharedMemoryName;
  {
    std::stringstream SharedMemoryNameStream;
    SharedMemoryNameStream << "jitlink_" << sys::Process::getProcessId() << '_'
                           << (++SharedMemoryCount);
    SharedMemoryName = SharedMemoryNameStream.str();
  }

  std::wstring WideSharedMemoryName(SharedMemoryName.begin(),
                                    SharedMemoryName.end());
  HANDLE SharedMemoryFile = CreateFileMappingW(
      INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, Size >> 32,
      Size & 0xffffffff, WideSharedMemoryName.c_str());
  if (!SharedMemoryFile)
    return errorCodeToError(mapWindowsError(GetLastError()));

  void *Addr = MapViewOfFile(SharedMemoryFile,
                             FILE_MAP_ALL_ACCESS | FILE_MAP_EXECUTE, 0, 0, 0);
  if (!Addr) {
    CloseHandle(SharedMemoryFile);
    return errorCodeToError(mapWindowsError(GetLastError()));
  }

#endif

  {
    std::lock_guard<std::mutex> Lock(Mutex);
    Reservations[Addr].Size = Size;
#if defined(_WIN32)
    Reservations[Addr].SharedMemoryFile = SharedMemoryFile;
#endif
  }

  return std::make_pair(ExecutorAddr::fromPtr(Addr),
                        std::move(SharedMemoryName));
#else
  return make_error<StringError>(
      "SharedMemoryMapper is not supported on this platform yet",
      inconvertibleErrorCode());
#endif
}

Expected<ExecutorAddr> ExecutorSharedMemoryMapperService::initialize(
    ExecutorAddr Reservation, tpctypes::SharedMemoryFinalizeRequest &FR) {
#if (defined(LLVM_ON_UNIX) && !defined(__ANDROID__)) || defined(_WIN32)

  ExecutorAddr MinAddr(~0ULL);

  // Contents are already in place
  for (auto &Segment : FR.Segments) {
    if (Segment.Addr < MinAddr)
      MinAddr = Segment.Addr;

#if defined(LLVM_ON_UNIX)

#if defined(__MVS__)
      // TODO Is it possible to change the protection level?
#else
    int NativeProt = 0;
    if ((Segment.RAG.Prot & MemProt::Read) == MemProt::Read)
      NativeProt |= PROT_READ;
    if ((Segment.RAG.Prot & MemProt::Write) == MemProt::Write)
      NativeProt |= PROT_WRITE;
    if ((Segment.RAG.Prot & MemProt::Exec) == MemProt::Exec)
      NativeProt |= PROT_EXEC;

    if (mprotect(Segment.Addr.toPtr<void *>(), Segment.Size, NativeProt))
      return errorCodeToError(errnoAsErrorCode());
#endif

#elif defined(_WIN32)

    DWORD NativeProt = getWindowsProtectionFlags(Segment.RAG.Prot);

    if (!VirtualProtect(Segment.Addr.toPtr<void *>(), Segment.Size, NativeProt,
                        &NativeProt))
      return errorCodeToError(mapWindowsError(GetLastError()));

#endif

    if ((Segment.RAG.Prot & MemProt::Exec) == MemProt::Exec)
      sys::Memory::InvalidateInstructionCache(Segment.Addr.toPtr<void *>(),
                                              Segment.Size);
  }

  // Run finalization actions and get deinitlization action list.
  std::vector<shared::WrapperFunctionCall> DeinitializeActions;
  {
    std::promise<MSVCPExpected<std::vector<shared::WrapperFunctionCall>>> P;
    auto F = P.get_future();
    shared::runFinalizeActions(
        std::move(FR.Actions),
        [&](Expected<std::vector<shared::WrapperFunctionCall>> R) {
          P.set_value(std::move(R));
        });
    if (auto DeinitializeActionsOrErr = F.get())
      DeinitializeActions = std::move(*DeinitializeActionsOrErr);
    else
      return DeinitializeActionsOrErr.takeError();
  }

  {
    std::lock_guard<std::mutex> Lock(Mutex);
    Allocations[MinAddr].DeinitializationActions =
        std::move(DeinitializeActions);
    Reservations[Reservation.toPtr<void *>()].Allocations.push_back(MinAddr);
  }

  return MinAddr;

#else
  return make_error<StringError>(
      "SharedMemoryMapper is not supported on this platform yet",
      inconvertibleErrorCode());
#endif
}

void ExecutorSharedMemoryMapperService::deinitialize(
    unique_function<void(Error)> OnComplete,
    const std::vector<ExecutorAddr> &Bases) {

  if (Bases.empty())
    return OnComplete(Error::success());

  std::vector<std::pair<void *, Allocation>> AllocPairs;

  Reservation *R = nullptr;
  for (auto &[RBase, REntry] : Reservations) {
    if (ExecutorAddrRange::fromPtrRange(RBase, REntry.Size)
            .contains(Bases.front())) {
      R = &REntry;
      break;
    }
  }

  Error Err = Error::success();
  if (R) {
    std::lock_guard<std::mutex> Lock(Mutex);

    for (auto Base : Bases) {
      auto I = Allocations.find(Base);

      {
        auto J = llvm::find(R->Allocations, Base);
        if (J != R->Allocations.end())
          R->Allocations.erase(J);
        else
          Err = joinErrors(std::move(Err),
                           make_error<StringError>(
                               "Reservation does not contain an entry for " +
                                   formatv("{0:x}", Base.getValue()),
                               inconvertibleErrorCode()));
      }

      if (I != Allocations.end()) {
        AllocPairs.push_back(
            std::make_pair(Base.toPtr<void *>(), std::move(I->second)));
        Allocations.erase(I);
      } else
        Err = joinErrors(
            std::move(Err),
            make_error<StringError>("No allocation entry found for " +
                                        formatv("{0:x}", Base.getValue()),
                                    inconvertibleErrorCode()));
    }
  } else
    Err = joinErrors(
        std::move(Err),
        make_error<StringError>("No reservation found covering " +
                                    formatv("{0:x}", Bases.front().getValue()),
                                inconvertibleErrorCode()));

  deinitializeSeq(std::move(OnComplete), std::move(AllocPairs), std::move(Err));
}

void ExecutorSharedMemoryMapperService::release(
    unique_function<void(Error)> OnComplete, std::vector<ExecutorAddr> Bases) {
#if (defined(LLVM_ON_UNIX) && !defined(__ANDROID__)) || defined(_WIN32)
  releaseSeq(std::move(OnComplete), std::move(Bases), Error::success());
#else
  OnComplete(make_error<StringError>(
      "SharedMemoryMapper is not supported on this platform yet",
      inconvertibleErrorCode()));
#endif
}

Error ExecutorSharedMemoryMapperService::shutdown() {
  if (Reservations.empty())
    return Error::success();

  std::vector<ExecutorAddr> ReservationAddrs;
  ReservationAddrs.reserve(Reservations.size());
  for (const auto &R : Reservations)
    ReservationAddrs.push_back(ExecutorAddr::fromPtr(R.getFirst()));

  std::promise<MSVCPError> ErrP;
  auto ErrF = ErrP.get_future();
  release([&](Error Err) { ErrP.set_value(std::move(Err)); },
          std::move(ReservationAddrs));

  return ErrF.get();
}

void ExecutorSharedMemoryMapperService::addBootstrapSymbols(
    StringMap<ExecutorAddr> &M) {
  M[rt::ExecutorSharedMemoryMapperServiceInstanceName] =
      ExecutorAddr::fromPtr(this);
  M[rt::ExecutorSharedMemoryMapperServiceReserveWrapperName] =
      ExecutorAddr::fromPtr(&reserveWrapper);
  M[rt::ExecutorSharedMemoryMapperServiceInitializeWrapperName] =
      ExecutorAddr::fromPtr(&initializeWrapper);
  M[rt::ExecutorSharedMemoryMapperServiceDeinitializeWrapperName] =
      ExecutorAddr::fromPtr(&deinitializeWrapper);
  M[rt::ExecutorSharedMemoryMapperServiceReleaseWrapperName] =
      ExecutorAddr::fromPtr(&releaseWrapper);
}

void ExecutorSharedMemoryMapperService::deinitializeSeq(
    unique_function<void(Error)> OnComplete,
    std::vector<std::pair<void *, Allocation>> Allocs, Error Err) {
  if (Allocs.empty())
    return OnComplete(std::move(Err));

  auto DeallocActions = std::move(Allocs.back().second.DeinitializationActions);
  Allocs.pop_back();

  runDeallocActions(
      std::move(DeallocActions),
      [this, Allocs = std::move(Allocs), PreviousErrs = std::move(Err),
       OnComplete = std::move(OnComplete)](Error Err) mutable {
        deinitializeSeq(std::move(OnComplete), std::move(Allocs),
                        joinErrors(std::move(PreviousErrs), std::move(Err)));
      });
}

void ExecutorSharedMemoryMapperService::releaseSeq(
    unique_function<void(Error)> OnComplete, std::vector<ExecutorAddr> Bases,
    Error Err) {
  if (Bases.empty())
    return OnComplete(std::move(Err));

  void *Base = Bases.back().toPtr<void *>();
  Bases.pop_back();
  std::optional<Reservation> R;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    auto I = Reservations.find(Base);

    if (LLVM_LIKELY(I != Reservations.end())) {
      R = std::move(I->second);
      Reservations.erase(I);
    }
  }

  if (LLVM_LIKELY(R)) {
    auto Allocs = std::move(R->Allocations);
    deinitialize(
        [this, R = std::move(*R), OnComplete = std::move(OnComplete), Base,
         Bases = std::move(Bases),
         PrevErr = std::move(Err)](Error Err) mutable {
          releaseSeq(
              std::move(OnComplete), std::move(Bases),
              joinErrors(std::move(PrevErr),
                         joinErrors(std::move(Err), releaseMem(Base, R))));
        },
        Allocs);
  } else {
    releaseSeq(std::move(OnComplete), std::move(Bases),
               joinErrors(std::move(Err),
                          make_error<StringError>("Unrecognized release base " +
                                                      formatv("{0:x}", Base),
                                                  inconvertibleErrorCode())));
  }
}

Error ExecutorSharedMemoryMapperService::releaseMem(void *Base,
                                                    const Reservation &R) {
#if defined(LLVM_ON_UNIX)

#if defined(__MVS__)
  (void)Size;

  if (shmdt(Base) < 0)
    return errorCodeToError(errnoAsErrorCode());
#else
  if (munmap(Base, R.Size) != 0)
    return errorCodeToError(errnoAsErrorCode());

  return Error::success();
#endif

#elif defined(_WIN32) // end #if defined(LLVM_ON_UNIX)
  (void)Size;

  Error Err = !UnmapViewOfFile(Base) ? mapWindowsError(GetLastError())
                                     : Error::success();

  CloseHandle(R.SharedMemoryFile);

  return Err;

#endif // defined(_WIN32)

  llvm_unreachable("Unsupported platform");
}

void ExecutorSharedMemoryMapperService::reserveWrapper(const char *ArgData,
                                                       size_t ArgSize,
                                                       void *SessionCtx,
                                                       uintptr_t MsgCtx,
                                                       shared::CYieldFn Yield) {
  using namespace shared;
  WrapperFunction<rt::SPSExecutorSharedMemoryMapperServiceReserveSignature>::
      handleAsyncWithSync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                          makeMethodWrapperHandler(
                              &ExecutorSharedMemoryMapperService::reserve));
}

void ExecutorSharedMemoryMapperService::initializeWrapper(
    const char *ArgData, size_t ArgSize, void *SessionCtx, uintptr_t MsgCtx,
    shared::CYieldFn Yield) {
  using namespace shared;
  WrapperFunction<rt::SPSExecutorSharedMemoryMapperServiceInitializeSignature>::
      handleAsyncWithSync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                          makeMethodWrapperHandler(
                              &ExecutorSharedMemoryMapperService::initialize));
}

void ExecutorSharedMemoryMapperService::deinitializeWrapper(
    const char *ArgData, size_t ArgSize, void *SessionCtx, uintptr_t MsgCtx,
    shared::CYieldFn Yield) {
  using namespace shared;
  WrapperFunction<
      rt::SPSExecutorSharedMemoryMapperServiceDeinitializeSignature>::
      handleAsync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                  makeAsyncMethodWrapperHandler(
                      &ExecutorSharedMemoryMapperService::deinitialize));
}

void ExecutorSharedMemoryMapperService::releaseWrapper(const char *ArgData,
                                                       size_t ArgSize,
                                                       void *SessionCtx,
                                                       uintptr_t MsgCtx,
                                                       shared::CYieldFn Yield) {
  using namespace shared;
  WrapperFunction<rt::SPSExecutorSharedMemoryMapperServiceReleaseSignature>::
      handleAsync(ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
                  makeAsyncMethodWrapperHandler(
                      &ExecutorSharedMemoryMapperService::release));
}

} // namespace rt_bootstrap
} // end namespace orc
} // end namespace llvm
