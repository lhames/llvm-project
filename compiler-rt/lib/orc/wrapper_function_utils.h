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
// Mostly functionality lifted from LLVM Support, plus some generic ORC-runtime
// utils.
//
//===----------------------------------------------------------------------===//

#ifndef ORC_RT_WRAPPER_FUNCTION_UTILS_H
#define ORC_RT_WRAPPER_FUNCTION_UTILS_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsErrors.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdString.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdVector.h"

#include "adt.h"
#include "dispatch.h"
#include "error.h"
#include "stl_extras.h"

#include <type_traits>

namespace llvm {
namespace orc {
namespace shared {

/// Serialization for string_views.
///
/// Serialization is as for regular strings. Deserialization points directly
/// into the blob.
template <> class BlobSerializationTraits<BlobString, __orc_rt::string_view> {
public:
  static size_t size(const __orc_rt::string_view &S) {
    return BlobArgList<uint64_t>::size(static_cast<uint64_t>(S.size())) +
           S.size();
  }

  static bool serialize(BlobOutputBuffer &BOB, const __orc_rt::string_view &S) {
    if (!BlobArgList<uint64_t>::serialize(BOB, static_cast<uint64_t>(S.size())))
      return false;
    return BOB.write(S.data(), S.size());
  }

  static bool deserialize(BlobInputBuffer &BIB, __orc_rt::string_view &S) {
    const char *Data = nullptr;
    uint64_t Size;
    if (!BlobArgList<uint64_t>::deserialize(BIB, Size))
      return false;
    Data = BIB.data();
    if (!BIB.skip(Size))
      return false;
    S = {Data, Size};
    return true;
  }
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

namespace __orc_rt {

using WrapperFunctionResult = llvm::orc::shared::WrapperFunctionResult;

template <typename BlobElementT>
using BlobSequence = llvm::orc::shared::BlobSequence<BlobElementT>;

using BlobString = llvm::orc::shared::BlobString;

using BlobEmpty = llvm::orc::shared::BlobEmpty;

using BlobError = llvm::orc::shared::BlobError;

template <typename BlobT>
using BlobExpected = llvm::orc::shared::BlobExpected<BlobT>;

using BlobErrorHelper = llvm::orc::shared::BlobErrorHelper;

template <typename T>
using BlobExpectedHelper = llvm::orc::shared::BlobExpectedHelper<T>;

using BlobTargetAddress = llvm::orc::shared::BlobTargetAddress;

template <typename... BlobElementTs>
using BlobTuple = llvm::orc::shared::BlobTuple<BlobElementTs...>;

using SerializableError = std::pair<bool, std::string>;

template <typename T>
using SerializableExpected = std::tuple<bool, T, std::string>;

inline BlobErrorHelper toBlobSerializable(Error Err) {
  if (Err)
    return {true, toString(std::move(Err))};
  return {false, {}};
}

inline Error fromBlobSerializable(BlobErrorHelper BEH) {
  if (BEH.HasError)
    return make_error<StringError>(BEH.ErrMsg);
  return Error::success();
}

template <typename T> BlobExpectedHelper<T> toBlobSerializable(Expected<T> E) {
  if (E)
    return {true, std::move(*E), {}};
  else
    return {false, {}, toString(E.takeError())};
}

template <typename T>
Expected<T> fromBlobSerializable(BlobExpectedHelper<T> BEH) {
  if (BEH.HasValue)
    return std::move(BEH.Value);
  else
    return make_error<StringError>(BEH.ErrMsg);
}

namespace detail {

template <typename BlobRetT, typename RetT> class ResultSerializer {
public:
  static WrapperFunctionResult serialize(RetT Result) {
    WrapperFunctionResult R;
    if (!llvm::orc::shared::BlobArgList<BlobRetT>::toBlob(R, Result))
      return WrapperFunctionResult::createOutOfBandErrorFromStringLiteral(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename BlobRetT> class ResultSerializer<BlobRetT, Error> {
public:
  static WrapperFunctionResult serialize(Error Err) {
    WrapperFunctionResult R;
    if (!llvm::orc::shared::BlobArgList<BlobRetT>::toBlob(
            R, toBlobSerializable(std::move(Err))))
      return WrapperFunctionResult::createOutOfBandErrorFromStringLiteral(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename BlobRetT, typename T>
class ResultSerializer<BlobRetT, Expected<T>> {
public:
  static WrapperFunctionResult serialize(Expected<T> E) {
    WrapperFunctionResult R;
    if (!llvm::orc::shared::BlobArgList<BlobRetT>::toBlob(
            R, toBlobSerializable(std::move(E))))
      return WrapperFunctionResult::createOutOfBandErrorFromStringLiteral(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename BlobRetT, typename RetT> class ResultDeserializer {
public:
  static void makeSafe(RetT &Result) {}

  static Error deserialize(RetT &Result, const char *ArgData, size_t ArgSize) {
    llvm::orc::shared::BlobInputBuffer BIB(ArgData, ArgSize);
    if (!llvm::orc::shared::BlobArgList<BlobRetT>::deserialize(BIB, Result))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    return Error::success();
  }
};

template <> class ResultDeserializer<BlobError, Error> {
public:
  static void makeSafe(Error &Err) { cantFail(std::move(Err)); }

  static Error deserialize(Error &Err, const char *ArgData, size_t ArgSize) {
    llvm::orc::shared::BlobInputBuffer BIB(ArgData, ArgSize);
    BlobErrorHelper BEH;
    if (!llvm::orc::shared::BlobArgList<BlobError>::deserialize(BIB, BEH))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    Err = fromBlobSerializable(std::move(BEH));
    return Error::success();
  }
};

template <typename BlobT, typename T>
class ResultDeserializer<BlobExpected<BlobT>, Expected<T>> {
public:
  static void makeSafe(Expected<T> &E) { cantFail(E.takeError()); }

  static Error deserialize(Expected<T> &E, const char *ArgData,
                           size_t ArgSize) {
    llvm::orc::shared::BlobInputBuffer BIB(ArgData, ArgSize);
    BlobExpectedHelper<T> BEH;
    if (!llvm::orc::shared::BlobArgList<BlobExpected<BlobT>>::deserialize(BIB,
                                                                          BEH))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    E = fromBlobSerializable(std::move(BEH));
    return Error::success();
  }
};

} // end namespace detail

template <typename BlobSignature> class WrapperFunction;

template <typename BlobRetT, typename... BlobTs>
class WrapperFunction<BlobRetT(BlobTs...)> {
private:
  template <typename RetT>
  using ResultSerializer = detail::ResultSerializer<BlobRetT, RetT>;

public:
  template <typename RetT, typename... ArgTs>
  static Error call(const void *FnTag, RetT &Result, const ArgTs &...Args) {

    // RetT might be an Error or Expected value. Set the checked flag now:
    // we don't want the user to have to check the unused result if this
    // operation fails.
    detail::ResultDeserializer<BlobRetT, RetT>::makeSafe(Result);

    if (!&__orc_rt_jit_dispatch_ctx)
      return make_error<StringError>("__orc_jtjit_dispatch_ctx not set");

    WrapperFunctionResult ArgBlob;
    if (!llvm::orc::shared::BlobArgList<BlobTs...>::toBlob(ArgBlob, Args...))
      return make_error<StringError>(
          "Error serializing arguments to blob in call");
    WrapperFunctionResult ResultBlob = __orc_rt_jit_dispatch(
        &__orc_rt_jit_dispatch_ctx, FnTag, ArgBlob.data(), ArgBlob.size());
    if (auto ErrMsg = ResultBlob.getOutOfBandError())
      return make_error<StringError>(ErrMsg);

    return detail::ResultDeserializer<BlobRetT, RetT>::deserialize(
        Result, ResultBlob.data(), ResultBlob.size());
  }

  template <typename HandlerT>
  static WrapperFunctionResult handle(HandlerT &&Handler, const char *ArgData,
                                      size_t ArgSize) {
    using WFHH = llvm::orc::shared::WrapperFunctionHandlerHelper<
        HandlerT, ResultSerializer, BlobTs...>;
    return WFHH::apply(std::forward<HandlerT>(Handler), ArgData, ArgSize);
  }

private:
  template <typename T> static const T &makeSerializable(const T &Value) {
    return Value;
  }

  static BlobErrorHelper makeSerializable(Error Err) {
    return toBlobSerializable(std::move(Err));
  }

  template <typename T>
  static BlobExpectedHelper<T> makeSerializable(Expected<T> E) {
    return toBlobSerializable(std::move(E));
  }
};

template <typename... BlobTs>
class WrapperFunction<void(BlobTs...)>
    : private WrapperFunction<BlobEmpty(BlobTs...)> {
public:
  template <typename... ArgTs>
  static Error call(const void *FnTag, const ArgTs &...Args) {
    BlobEmpty BE;
    return WrapperFunction<BlobEmpty(BlobTs...)>::call(FnTag, BE, Args...);
  }

  using WrapperFunction<BlobEmpty(BlobTs...)>::handle;
};

void dumpBuffer(const char *Buffer, size_t Size);
void dumpBuffer(WrapperFunctionResult &R);

} // end namespace __orc_rt

#endif // ORC_RT_WRAPPER_FUNCTION_UTILS_H
