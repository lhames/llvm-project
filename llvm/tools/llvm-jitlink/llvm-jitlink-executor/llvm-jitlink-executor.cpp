//===- llvm-jitlink-executor.cpp - Out-of-proc executor for llvm-jitlink -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Simple out-of-process executor for llvm-jitlink.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Shared/FDRawByteChannel.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/OrcRPCTPCServer.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h"
#include "llvm/ExecutionEngine/Orc/TargetProcess/TargetExecutionUtils.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <future>
#include <sstream>
#include <thread>

#ifdef LLVM_ON_UNIX

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#endif

using namespace llvm;
using namespace llvm::orc;

using JITLinkExecutorEndpoint =
    shared::MultiThreadedRPCEndpoint<shared::FDRawByteChannel>;
using JITLinkExecutorServer = OrcRPCTPCServer<JITLinkExecutorEndpoint>;

ExitOnError ExitOnErr;

LLVM_ATTRIBUTE_USED void linkComponents() {
  errs() << (void *)&llvm_orc_registerEHFrameSectionWrapper
         << (void *)&llvm_orc_deregisterEHFrameSectionWrapper
         << (void *)&llvm_orc_registerJITLoaderGDBWrapper;
}

void printErrorAndExit(Twine ErrMsg) {
  errs() << "error: " << ErrMsg.str() << "\n\n"
         << "Usage:\n"
         << "  llvm-jitlink-executor filedescs=<infd>,<outfd> [args...]\n"
         << "  llvm-jitlink-executor listen=<host>:<port> [args...]\n";
  exit(1);
}

int openListener(std::string Host, std::string PortStr) {
#ifndef LLVM_ON_UNIX
  // FIXME: Add TCP support for Windows.
  printErrorAndExit("listen option not supported");
  return 0;
#else
  addrinfo Hints{};
  Hints.ai_family = AF_INET;
  Hints.ai_socktype = SOCK_STREAM;
  Hints.ai_flags = AI_PASSIVE;

  addrinfo *AI;
  if (int EC = getaddrinfo(nullptr, PortStr.c_str(), &Hints, &AI)) {
    errs() << "Error setting up bind address: " << gai_strerror(EC) << "\n";
    exit(1);
  }

  // Create a socket from first addrinfo structure returned by getaddrinfo.
  int SockFD;
  if ((SockFD = socket(AI->ai_family, AI->ai_socktype, AI->ai_protocol)) < 0) {
    errs() << "Error creating socket: " << std::strerror(errno) << "\n";
    exit(1);
  }

  // Avoid "Address already in use" errors.
  const int Yes = 1;
  if (setsockopt(SockFD, SOL_SOCKET, SO_REUSEADDR, &Yes, sizeof(int)) == -1) {
    errs() << "Error calling setsockopt: " << std::strerror(errno) << "\n";
    exit(1);
  }

  // Bind the socket to the desired port.
  if (bind(SockFD, AI->ai_addr, AI->ai_addrlen) < 0) {
    errs() << "Error on binding: " << std::strerror(errno) << "\n";
    exit(1);
  }

  // Listen for incomming connections.
  static constexpr int ConnectionQueueLen = 1;
  listen(SockFD, ConnectionQueueLen);

  outs() << "Listening at " << Host << ":" << PortStr << "\n";

#if defined(_AIX)
  assert(Hi_32(AI->ai_addrlen) == 0 && "Field is a size_t on 64-bit AIX");
  socklen_t AddrLen = Lo_32(AI->ai_addrlen);
  return accept(SockFD, AI->ai_addr, &AddrLen);
#else
  return accept(SockFD, AI->ai_addr, &AI->ai_addrlen);
#endif

#endif // LLVM_ON_UNIX
}

static bool UseTestResultOverride = false;
static int64_t TestResultOverride = 0;

extern "C" void llvm_jitlink_setTestResultOverride(int64_t Value) {
  TestResultOverride = Value;
  UseTestResultOverride = true;
}

int main(int argc, char *argv[]) {

  ExitOnErr.setBanner(std::string(argv[0]) + ": ");

  int InFD = 0;
  int OutFD = 0;

  if (argc < 2)
    printErrorAndExit("insufficient arguments");
  else {
    StringRef Arg1 = argv[1];
    StringRef SpecifierType, Specifier;
    std::tie(SpecifierType, Specifier) = Arg1.split('=');
    if (SpecifierType == "filedescs") {
      StringRef FD1Str, FD2Str;
      std::tie(FD1Str, FD2Str) = Specifier.split(',');
      if (FD1Str.getAsInteger(10, InFD))
        printErrorAndExit(FD1Str + " is not a valid file descriptor");
      if (FD2Str.getAsInteger(10, OutFD))
        printErrorAndExit(FD2Str + " is not a valid file descriptor");
    } else if (SpecifierType == "listen") {
      StringRef Host, PortStr;
      std::tie(Host, PortStr) = Specifier.split(':');

      int Port = 0;
      if (PortStr.getAsInteger(10, Port))
        printErrorAndExit("port number '" + PortStr +
                          "' is not a valid integer");

      InFD = OutFD = openListener(Host.str(), PortStr.str());
      outs() << "Connection established. Running OrcRPCTPCServer...\n";
    } else
      printErrorAndExit("invalid specifier type \"" + SpecifierType + "\"");
  }

  ExitOnErr.setBanner(std::string(argv[0]) + ":");

  using JITLinkExecutorEndpoint =
      shared::MultiThreadedRPCEndpoint<shared::FDRawByteChannel>;

  shared::registerStringError<shared::FDRawByteChannel>();

  shared::FDRawByteChannel C(InFD, OutFD);
  JITLinkExecutorEndpoint EP(C, true);

  struct MainInvocation {
    std::function<Error(Expected<int64_t>)> SendResult;
    JITTargetAddress MainAddr;
    std::vector<std::string> Args;
  };

  auto MIP = std::make_unique<std::promise<MainInvocation>>();
  auto MIF = MIP->get_future();

  JITLinkExecutorServer Server(
      EP, [&](std::function<Error(Expected<int64_t>)> SendResult,
              JITTargetAddress MainAddr, std::vector<std::string> Args) {
        if (MIP) {
          MIP->set_value({std::move(SendResult), MainAddr, std::move(Args)});
          MIP = nullptr;
        } else
          ExitOnErr(SendResult(make_error<StringError>(
              "runMain already invoked", inconvertibleErrorCode())));
        return Error::success();
      });

  std::thread ListenerThread([&]() { ExitOnErr(Server.run()); });

  auto MI = MIF.get();

  using MainTy = int (*)(int, char *[]);
  int64_t Result = runAsMain(jitTargetAddressToPointer<MainTy>(MI.MainAddr),
                             MI.Args, StringRef("llvm-jitlink-executor"));

  // If the executing code set a test result override then use that.
  if (UseTestResultOverride)
    Result = TestResultOverride;

  ExitOnErr(MI.SendResult(Result));

  ListenerThread.join();

  return 0;
}
