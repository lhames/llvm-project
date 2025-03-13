//===-- wrapper_function_utils.h - Utilities for wrapper funcs --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of the ORC runtime support library.
//
//===----------------------------------------------------------------------===//

#ifndef ORC_RT_WRAPPER_FUNCTION_UTILS_H
#define ORC_RT_WRAPPER_FUNCTION_UTILS_H

#include "error.h"
#include "executor_address.h"
#include "orc_rt/c_api.h"
#include "simple_packed_serialization.h"

#include <sstream>
#include <type_traits>

namespace orc_rt {

/// C++ wrapper function result: Same as orc_rt_WrapperFunctionResult but
/// auto-releases memory.
class WrapperFunctionResult {
public:
  /// Create a default WrapperFunctionResult.
  WrapperFunctionResult() { orc_rt_WrapperFunctionResultInit(&R); }

  /// Create a WrapperFunctionResult from a WrapperFunctionResult. This
  /// instance takes ownership of the result object and will automatically
  /// call dispose on the result upon destruction.
  WrapperFunctionResult(orc_rt_WrapperFunctionResult R) : R(R) {}

  WrapperFunctionResult(const WrapperFunctionResult &) = delete;
  WrapperFunctionResult &operator=(const WrapperFunctionResult &) = delete;

  WrapperFunctionResult(WrapperFunctionResult &&Other) {
    orc_rt_WrapperFunctionResultInit(&R);
    std::swap(R, Other.R);
  }

  WrapperFunctionResult &operator=(WrapperFunctionResult &&Other) {
    orc_rt_WrapperFunctionResult Tmp;
    orc_rt_WrapperFunctionResultInit(&Tmp);
    std::swap(Tmp, Other.R);
    std::swap(R, Tmp);
    return *this;
  }

  ~WrapperFunctionResult() { orc_rt_DisposeWrapperFunctionResult(&R); }

  /// Relinquish ownership of and return the
  /// orc_rt_WrapperFunctionResult.
  orc_rt_WrapperFunctionResult release() {
    orc_rt_WrapperFunctionResult Tmp;
    orc_rt_WrapperFunctionResultInit(&Tmp);
    std::swap(R, Tmp);
    return Tmp;
  }

  /// Get a pointer to the data contained in this instance.
  char *data() { return orc_rt_WrapperFunctionResultData(&R); }

  /// Returns the size of the data contained in this instance.
  size_t size() const { return orc_rt_WrapperFunctionResultSize(&R); }

  /// Returns true if this value is equivalent to a default-constructed
  /// WrapperFunctionResult.
  bool empty() const { return orc_rt_WrapperFunctionResultEmpty(&R); }

  /// Create a WrapperFunctionResult with the given size and return a pointer
  /// to the underlying memory.
  static WrapperFunctionResult allocate(size_t Size) {
    WrapperFunctionResult R;
    R.R = orc_rt_WrapperFunctionResultAllocate(Size);
    return R;
  }

  /// Copy from the given char range.
  static WrapperFunctionResult copyFrom(const char *Source, size_t Size) {
    return orc_rt_CreateWrapperFunctionResultFromRange(Source, Size);
  }

  /// Copy from the given null-terminated string (includes the null-terminator).
  static WrapperFunctionResult copyFrom(const char *Source) {
    return orc_rt_CreateWrapperFunctionResultFromString(Source);
  }

  /// Copy from the given std::string (includes the null terminator).
  static WrapperFunctionResult copyFrom(const std::string &Source) {
    return copyFrom(Source.c_str());
  }

  /// Create an out-of-band error by copying the given string.
  static WrapperFunctionResult createOutOfBandError(const char *Msg) {
    return orc_rt_CreateWrapperFunctionResultFromOutOfBandError(Msg);
  }

  /// Create an out-of-band error by copying the given string.
  static WrapperFunctionResult createOutOfBandError(const std::string &Msg) {
    return createOutOfBandError(Msg.c_str());
  }

  template <typename SPSArgListT, typename... ArgTs>
  static WrapperFunctionResult fromSPSArgs(const ArgTs &...Args) {
    auto Result = allocate(SPSArgListT::size(Args...));
    SPSOutputBuffer OB(Result.data(), Result.size());
    if (!SPSArgListT::serialize(OB, Args...))
      return createOutOfBandError(
          "Error serializing arguments to blob in call");
    return Result;
  }

  /// If this value is an out-of-band error then this returns the error message,
  /// otherwise returns nullptr.
  const char *getOutOfBandError() const {
    return orc_rt_WrapperFunctionResultGetOutOfBandError(&R);
  }

private:
  orc_rt_WrapperFunctionResult R;
};

namespace detail {

template <typename SPSArgListT, typename... ArgTs>
WrapperFunctionResult
serializeViaSPSToWrapperFunctionResult(const ArgTs &...Args) {
  auto Result = WrapperFunctionResult::allocate(SPSArgListT::size(Args...));
  SPSOutputBuffer OB(Result.data(), Result.size());
  if (!SPSArgListT::serialize(OB, Args...))
    return WrapperFunctionResult::createOutOfBandError(
        "Error serializing arguments to blob in call");
  return Result;
}

template <typename RetT> class WrapperFunctionHandlerCaller {
public:
  template <typename HandlerT, typename ArgTupleT, std::size_t... I>
  static decltype(auto) call(HandlerT &&H, ArgTupleT &Args,
                             std::index_sequence<I...>) {
    return std::forward<HandlerT>(H)(std::get<I>(Args)...);
  }
};

template <> class WrapperFunctionHandlerCaller<void> {
public:
  template <typename HandlerT, typename ArgTupleT, std::size_t... I>
  static SPSEmpty call(HandlerT &&H, ArgTupleT &Args,
                       std::index_sequence<I...>) {
    std::forward<HandlerT>(H)(std::get<I>(Args)...);
    return SPSEmpty();
  }
};

template <typename WrapperFunctionImplT,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper
    : public WrapperFunctionHandlerHelper<
          decltype(&std::remove_reference_t<WrapperFunctionImplT>::operator()),
          ResultSerializer, SPSTagTs...> {};

template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                   SPSTagTs...> {
public:
  using ArgTuple = std::tuple<std::decay_t<ArgTs>...>;
  using ArgIndices = std::make_index_sequence<std::tuple_size<ArgTuple>::value>;

  template <typename HandlerT>
  static WrapperFunctionResult apply(HandlerT &&H, const char *ArgData,
                                     size_t ArgSize) {
    ArgTuple Args;
    if (!deserialize(ArgData, ArgSize, Args, ArgIndices{}))
      return WrapperFunctionResult::createOutOfBandError(
          "Could not deserialize arguments for wrapper function call");

    auto HandlerResult = WrapperFunctionHandlerCaller<RetT>::call(
        std::forward<HandlerT>(H), Args, ArgIndices{});

    return ResultSerializer<decltype(HandlerResult)>::serialize(
        std::move(HandlerResult));
  }

private:
  template <std::size_t... I>
  static bool deserialize(const char *ArgData, size_t ArgSize, ArgTuple &Args,
                          std::index_sequence<I...>) {
    SPSInputBuffer IB(ArgData, ArgSize);
    return SPSArgList<SPSTagTs...>::deserialize(IB, std::get<I>(Args)...);
  }
};

// Map function pointers to function types.
template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (*)(ArgTs...), ResultSerializer,
                                   SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

// Map non-const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...), ResultSerializer,
                                   SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

// Map const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...) const,
                                   ResultSerializer, SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

template <typename WrapperFunctionImplT,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionAsyncHandlerHelper
    : public WrapperFunctionAsyncHandlerHelper<
          decltype(&std::remove_reference_t<WrapperFunctionImplT>::operator()),
          ResultSerializer, SPSTagTs...> {};

template <typename RetT, typename SendResultT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionAsyncHandlerHelper<RetT(SendResultT, ArgTs...),
                                        ResultSerializer, SPSTagTs...> {
public:
  using ArgTuple = std::tuple<std::decay_t<ArgTs>...>;
  using ArgIndices = std::make_index_sequence<std::tuple_size<ArgTuple>::value>;

  template <typename HandlerT, typename SendWrapperFunctionResultT>
  static void applyAsync(HandlerT &&H,
                         SendWrapperFunctionResultT &&SendWrapperFunctionResult,
                         const char *ArgData, size_t ArgSize) {
    ArgTuple Args;
    if (!deserialize(ArgData, ArgSize, Args, ArgIndices{})) {
      SendWrapperFunctionResult(WrapperFunctionResult::createOutOfBandError(
          "Could not deserialize arguments for wrapper function call"));
      return;
    }

    auto SendResult =
        [SendWFR = std::move(SendWrapperFunctionResult)](auto Result) mutable {
          using ResultT = decltype(Result);
          SendWFR(ResultSerializer<ResultT>::serialize(std::move(Result)));
        };

    callAsync(std::forward<HandlerT>(H), std::move(SendResult), std::move(Args),
              ArgIndices{});
  }

private:
  template <std::size_t... I>
  static bool deserialize(const char *ArgData, size_t ArgSize, ArgTuple &Args,
                          std::index_sequence<I...>) {
    SPSInputBuffer IB(ArgData, ArgSize);
    return SPSArgList<SPSTagTs...>::deserialize(IB, std::get<I>(Args)...);
  }

  template <typename HandlerT, typename SerializeAndSendResultT,
            typename ArgTupleT, std::size_t... I>
  static void callAsync(HandlerT &&H,
                        SerializeAndSendResultT &&SerializeAndSendResult,
                        ArgTupleT Args, std::index_sequence<I...>) {
    (void)Args; // Silence a buggy GCC warning.
    return std::forward<HandlerT>(H)(std::move(SerializeAndSendResult),
                                     std::move(std::get<I>(Args))...);
  }
};

// Map function pointers to function types.
template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionAsyncHandlerHelper<RetT (*)(ArgTs...), ResultSerializer,
                                        SPSTagTs...>
    : public WrapperFunctionAsyncHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                               SPSTagTs...> {};

// Map non-const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionAsyncHandlerHelper<RetT (ClassT::*)(ArgTs...),
                                        ResultSerializer, SPSTagTs...>
    : public WrapperFunctionAsyncHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                               SPSTagTs...> {};

// Map const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionAsyncHandlerHelper<RetT (ClassT::*)(ArgTs...) const,
                                        ResultSerializer, SPSTagTs...>
    : public WrapperFunctionAsyncHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                               SPSTagTs...> {};

template <typename SPSRetTagT, typename RetT> class ResultSerializer {
public:
  static WrapperFunctionResult serialize(RetT Result) {
    return WrapperFunctionResult::fromSPSArgs<SPSArgList<SPSRetTagT>>(Result);
  }
};

template <typename SPSRetTagT> class ResultSerializer<SPSRetTagT, Error> {
public:
  static WrapperFunctionResult serialize(Error Err) {
    return WrapperFunctionResult::fromSPSArgs<SPSArgList<SPSRetTagT>>(
        toSPSSerializable(std::move(Err)));
  }
};

template <typename SPSRetTagT, typename T>
class ResultSerializer<SPSRetTagT, Expected<T>> {
public:
  static WrapperFunctionResult serialize(Expected<T> E) {
    return WrapperFunctionResult::fromSPSArgs<SPSArgList<SPSRetTagT>>(
        toSPSSerializable(std::move(E)));
  }
};

template <typename SPSRetTagT, typename RetT> class ResultDeserializer {
public:
  static void makeSafe(RetT &Result) {}

  static Error deserialize(RetT &Result, const char *ArgData, size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    if (!SPSArgList<SPSRetTagT>::deserialize(IB, Result))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    return Error::success();
  }
};

template <> class ResultDeserializer<SPSError, Error> {
public:
  static void makeSafe(Error &Err) { cantFail(std::move(Err)); }

  static Error deserialize(Error &Err, const char *ArgData, size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    SPSSerializableError BSE;
    if (!SPSArgList<SPSError>::deserialize(IB, BSE))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    Err = fromSPSSerializable(std::move(BSE));
    return Error::success();
  }
};

template <typename SPSTagT, typename T>
class ResultDeserializer<SPSExpected<SPSTagT>, Expected<T>> {
public:
  static void makeSafe(Expected<T> &E) { cantFail(E.takeError()); }

  static Error deserialize(Expected<T> &E, const char *ArgData,
                           size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    SPSSerializableExpected<T> BSE;
    if (!SPSArgList<SPSExpected<SPSTagT>>::deserialize(IB, BSE))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    E = fromSPSSerializable(std::move(BSE));
    return Error::success();
  }
};

} // end namespace detail

template <typename SPSSignature> class WrapperFunction;

template <typename SPSRetTagT, typename... SPSTagTs>
class WrapperFunction<SPSRetTagT(SPSTagTs...)> {
private:
  template <typename RetT>
  using ResultSerializer = detail::ResultSerializer<SPSRetTagT, RetT>;

public:
  template <typename DispatchFn, typename RetT, typename... ArgTs>
  static Error call(DispatchFn &&Dispatch, RetT &Result, const ArgTs &...Args) {

    // RetT might be an Error or Expected value. Set the checked flag now:
    // we don't want the user to have to check the unused result if this
    // operation fails.
    detail::ResultDeserializer<SPSRetTagT, RetT>::makeSafe(Result);

    auto ArgBuffer =
        WrapperFunctionResult::fromSPSArgs<SPSArgList<SPSTagTs...>>(Args...);
    if (const char *ErrMsg = ArgBuffer.getOutOfBandError())
      return make_error<StringError>(ErrMsg);

    WrapperFunctionResult ResultBuffer =
        Dispatch(ArgBuffer.data(), ArgBuffer.size());

    if (auto ErrMsg = ResultBuffer.getOutOfBandError())
      return make_error<StringError>(ErrMsg);

    return detail::ResultDeserializer<SPSRetTagT, RetT>::deserialize(
        Result, ResultBuffer.data(), ResultBuffer.size());
  }

  /// Call an async wrapper function.
  /// Caller should be callable as
  /// void Fn(unique_function<void(WrapperFunctionResult)> SendResult,
  ///         WrapperFunctionResult ArgBuffer);
  template <typename AsyncCallerFn, typename SendDeserializedResultFn,
            typename... ArgTs>
  static void callAsync(AsyncCallerFn &&Caller,
                        SendDeserializedResultFn &&SendDeserializedResult,
                        const ArgTs &...Args) {
    using RetT = typename std::tuple_element<
        1, typename detail::WrapperFunctionHandlerHelper<
               std::remove_reference_t<SendDeserializedResultFn>,
               ResultSerializer, SPSRetTagT>::ArgTuple>::type;

    auto ArgBuffer =
        detail::serializeViaSPSToWrapperFunctionResult<SPSArgList<SPSTagTs...>>(
            Args...);
    if (auto *ErrMsg = ArgBuffer.getOutOfBandError()) {
      SendDeserializedResult(
          make_error<StringError>(ErrMsg),
          detail::ResultDeserializer<SPSRetTagT, RetT>::makeValue());
      return;
    }

    auto SendSerializedResult = [SDR = std::move(SendDeserializedResult)](
                                    WrapperFunctionResult R) mutable {
      RetT RetVal = detail::ResultDeserializer<SPSRetTagT, RetT>::makeValue();
      detail::ResultDeserializer<SPSRetTagT, RetT>::makeSafe(RetVal);

      if (auto *ErrMsg = R.getOutOfBandError()) {
        SDR(make_error<StringError>(ErrMsg), std::move(RetVal));
        return;
      }

      SPSInputBuffer IB(R.data(), R.size());
      if (auto Err = detail::ResultDeserializer<SPSRetTagT, RetT>::deserialize(
              RetVal, R.data(), R.size())) {
        SDR(std::move(Err), std::move(RetVal));
        return;
      }

      SDR(Error::success(), std::move(RetVal));
    };

    Caller(std::move(SendSerializedResult), ArgBuffer.data(), ArgBuffer.size());
  }

  // template <typename HandlerT>
  // static WrapperFunctionResult handle(const char *ArgData, size_t ArgSize,
  //                                     HandlerT &&Handler) {
  //   using WFHH =
  //       detail::WrapperFunctionHandlerHelper<std::remove_reference_t<HandlerT>,
  //                                            ResultSerializer, SPSTagTs...>;
  //   return WFHH::apply(std::forward<HandlerT>(Handler), ArgData, ArgSize);
  // }

  template <typename HandlerT, typename SendResultT>
  static void handleAsync(const char *ArgData, size_t ArgSize,
                          SendResultT &&SendResult, HandlerT &&Handler) {
    using WFAHH = detail::WrapperFunctionAsyncHandlerHelper<
        std::remove_reference_t<HandlerT>, ResultSerializer, SPSTagTs...>;
    WFAHH::applyAsync(std::forward<HandlerT>(Handler),
                      std::forward<SendResultT>(SendResult), ArgData, ArgSize);
  }

  /// Handle a call to an async wrapper function using a synchronous
  /// implementation.
  template <typename HandlerT, typename SendResultT>
  static void handleAsyncWithSync(const char *ArgData, size_t ArgSize,
                                  SendResultT &&SendResult,
                                  HandlerT &&Handler) {
    using WFHH =
        detail::WrapperFunctionHandlerHelper<std::remove_reference_t<HandlerT>,
                                             ResultSerializer, SPSTagTs...>;
    SendResult(WFHH::apply(std::forward<HandlerT>(Handler), ArgData, ArgSize));
  }

private:
  template <typename T> static const T &makeSerializable(const T &Value) {
    return Value;
  }

  static detail::SPSSerializableError makeSerializable(Error Err) {
    return detail::toSPSSerializable(std::move(Err));
  }

  template <typename T>
  static detail::SPSSerializableExpected<T> makeSerializable(Expected<T> E) {
    return detail::toSPSSerializable(std::move(E));
  }
};

template <typename... SPSTagTs>
class WrapperFunction<void(SPSTagTs...)>
    : private WrapperFunction<SPSEmpty(SPSTagTs...)> {
public:
  template <typename DispatchFn, typename... ArgTs>
  static Error call(DispatchFn &&Dispatch, const ArgTs &...Args) {
    SPSEmpty BE;
    return WrapperFunction<SPSEmpty(SPSTagTs...)>::call(
        std::forward<DispatchFn>(Dispatch), BE, Args...);
  }

  using WrapperFunction<SPSEmpty(SPSTagTs...)>::handle;
};

/// A function object that takes an ExecutorAddr as its first argument,
/// casts that address to a ClassT*, then calls the given method on that
/// pointer passing in the remaining function arguments. This utility
/// removes some of the boilerplate from writing wrappers for method calls.
///
///   @code{.cpp}
///   class MyClass {
///   public:
///     void myMethod(uint32_t, bool) { ... }
///   };
///
///   // SPS Method signature -- note MyClass object address as first argument.
///   using SPSMyMethodWrapperSignature =
///     SPSTuple<SPSExecutorAddr, uint32_t, bool>;
///
///   WrapperFunctionResult
///   myMethodCallWrapper(const char *ArgData, size_t ArgSize) {
///     return WrapperFunction<SPSMyMethodWrapperSignature>::handle(
///        ArgData, ArgSize, makeMethodWrapperHandler(&MyClass::myMethod));
///   }
///   @endcode
///
template <typename RetT, typename ClassT, typename... ArgTs>
class MethodWrapperHandler {
public:
  using MethodT = RetT (ClassT::*)(ArgTs...);
  MethodWrapperHandler(MethodT M) : M(M) {}
  RetT operator()(ExecutorAddr ObjAddr, ArgTs &...Args) {
    return (ObjAddr.toPtr<ClassT *>()->*M)(std::forward<ArgTs>(Args)...);
  }

private:
  MethodT M;
};

/// Create a MethodWrapperHandler object from the given method pointer.
template <typename RetT, typename ClassT, typename... ArgTs>
MethodWrapperHandler<RetT, ClassT, ArgTs...>
makeMethodWrapperHandler(RetT (ClassT::*Method)(ArgTs...)) {
  return MethodWrapperHandler<RetT, ClassT, ArgTs...>(Method);
}

class CYield {
public:
  CYield(void *SessionCtx, uintptr_t MsgCtx, orc_rt_YieldFn Yield)
      : SessionCtx(SessionCtx), MsgCtx(MsgCtx), Yield(Yield) {}

  CYield(CYield &&Other) {
    std::swap(SessionCtx, Other.SessionCtx);
    std::swap(MsgCtx, Other.MsgCtx);
    std::swap(Yield, Other.Yield);
  }

  CYield &operator=(CYield &&Other) {
    SessionCtx = Other.SessionCtx;
    MsgCtx = Other.MsgCtx;
    Yield = Other.Yield;
    Other.Yield = nullptr;
    return *this;
  }

  ~CYield() {
    if (Yield) {
      std::ostringstream ErrStream;
      ErrStream << "Async result sender (session = " << SessionCtx
                << ", message = " << reinterpret_cast<void *>(MsgCtx)
                << ") destroyed without receiving result";
      Yield(SessionCtx, MsgCtx,
            WrapperFunctionResult::createOutOfBandError(ErrStream.str().c_str())
                .release());
    }
  }
  void operator()(WrapperFunctionResult R) {
    Yield(SessionCtx, MsgCtx, R.release());
    Yield = nullptr;
  }

private:
  void *SessionCtx = nullptr;
  uintptr_t MsgCtx = 0;
  orc_rt_YieldFn Yield = nullptr;
};

} // namespace orc_rt

#endif // ORC_RT_WRAPPER_FUNCTION_UTILS_H
