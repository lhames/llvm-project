//===- wrapper_function_call.h -----------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// ORC Runtime support for dynamic loading features on COFF-based platforms.
//
//===----------------------------------------------------------------------===//

#ifndef ORC_RT_WRAPPER_FUNCTION_CALL_H
#define ORC_RT_WRAPPER_FUNCTION_CALL_H

#include "unique_function.h"
#include "wrapper_function_utils.h"

namespace orc_rt {

/// Represents a call to a wrapper function.
class WrapperFunctionCall {
public:
  // FIXME: Switch to a SmallVector<char, 24> once ORC runtime has a
  // smallvector.
  using ArgDataBufferType = std::vector<char>;

  /// Create a WrapperFunctionCall using the given SPS serializer to serialize
  /// the arguments.
  template <typename SPSSerializer, typename... ArgTs>
  static Expected<WrapperFunctionCall> Create(ExecutorAddr FnAddr,
                                              const ArgTs &...Args) {
    ArgDataBufferType ArgData;
    ArgData.resize(SPSSerializer::size(Args...));
    SPSOutputBuffer OB(ArgData.empty() ? nullptr : ArgData.data(),
                       ArgData.size());
    if (SPSSerializer::serialize(OB, Args...))
      return WrapperFunctionCall(FnAddr, std::move(ArgData));
    return make_error<StringError>("Cannot serialize arguments for "
                                   "AllocActionCall");
  }

  WrapperFunctionCall() = default;

  /// Create a WrapperFunctionCall from a target function and arg buffer.
  WrapperFunctionCall(ExecutorAddr FnAddr, ArgDataBufferType ArgData)
      : FnAddr(FnAddr), ArgData(std::move(ArgData)) {}

  /// Returns the address to be called.
  const ExecutorAddr &getCallee() const { return FnAddr; }

  /// Returns the argument data.
  const ArgDataBufferType &getArgData() const { return ArgData; }

  /// WrapperFunctionCalls convert to true if the callee is non-null.
  explicit operator bool() const { return !!FnAddr; }

  /// Run async call returning raw WrapperFunctionResult.
  void run(void *SessionCtx, uintptr_t MsgCtx, orc_rt_YieldFn Yield) const {
    FnAddr.toPtr<orc_rt_WrapperFn>()(ArgData.data(), ArgData.size(), SessionCtx,
                                     MsgCtx, Yield);
  }

private:
  ExecutorAddr FnAddr;
  std::vector<char> ArgData;
};

using SPSWrapperFunctionCall = SPSTuple<SPSExecutorAddr, SPSSequence<char>>;

template <>
class SPSSerializationTraits<SPSWrapperFunctionCall, WrapperFunctionCall> {
public:
  static size_t size(const WrapperFunctionCall &WFC) {
    return SPSArgList<SPSExecutorAddr, SPSSequence<char>>::size(
        WFC.getCallee(), WFC.getArgData());
  }

  static bool serialize(SPSOutputBuffer &OB, const WrapperFunctionCall &WFC) {
    return SPSArgList<SPSExecutorAddr, SPSSequence<char>>::serialize(
        OB, WFC.getCallee(), WFC.getArgData());
  }

  static bool deserialize(SPSInputBuffer &IB, WrapperFunctionCall &WFC) {
    ExecutorAddr FnAddr;
    WrapperFunctionCall::ArgDataBufferType ArgData;
    if (!SPSWrapperFunctionCall::AsArgList::deserialize(IB, FnAddr, ArgData))
      return false;
    WFC = WrapperFunctionCall(FnAddr, std::move(ArgData));
    return true;
  }
};

/// Given a sequence of WrapperFunctionCalls, each of which returns an
/// SPS-serialized Error result, run all calls (or until first error) and then
/// call the OnComplete handler.
void runErrorWFCSequence(unique_function<void(Error)> OnComplete,
                         std::vector<WrapperFunctionCall> WFCs);

} // namespace orc_rt

#endif // ORC_RT_WRAPPER_FUNCTION_CALL_H
