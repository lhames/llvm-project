//===--- LLVMSideWrapperFunctionUtils.h - For LLVM-side types ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Serialization for LLVM-side data types.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_LLVMSIDEWRAPPERFUNCTIONUTILS_H
#define LLVM_EXECUTIONENGINE_ORC_LLVMSIDEWRAPPERFUNCTIONUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/TargetProcessControl.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsErrors.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdList.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdString.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdUnorderedMap.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdVector.h"

namespace llvm {
namespace orc {
namespace shared {

/// Allow serialization of BlobSequences to/from ArrayRefs.
template <typename BlobElementT, typename T>
class TrivialBlobSequenceSerialization<BlobElementT, ArrayRef<T>> {
public:
  static constexpr bool available = true;
};

/// Allow serialization of BlobSequences to/from MutableArrayRefs.
template <typename BlobElementT, typename T>
class TrivialBlobSequenceSerialization<BlobElementT, MutableArrayRef<T>> {
public:
  static constexpr bool available = true;
};

/// Allow serialization of BlobStrings to/from StringRef.
template <> class TrivialBlobSequenceSerialization<char, llvm::StringRef> {
public:
  static constexpr bool available = true;
};

/// Allow serialization to BlobStrings from SymbolStringPtrs.
template <>
class BlobSerializationTraits<BlobString, SymbolStringPtr> {
public:
  static size_t size(const SymbolStringPtr &S) {
    return BlobArgList<BlobString>::size(*S);
  }

  static bool serialize(BlobOutputBuffer &BOB, const SymbolStringPtr &S) {
    return BlobArgList<BlobString>::serialize(BOB, *S);
  }
};

using SerializableError = std::pair<bool, std::string>;

/// FIXME: Use std::variant once we have it.
template <typename T>
using SerializableExpected = std::tuple<bool, T, std::string>;

inline BlobErrorHelper toBlobSerializable(Error Err) {
  if (Err)
    return {true, toString(std::move(Err))};
  return {false, {}};
}

inline Error fromBlobSerializable(BlobErrorHelper BEH) {
  if (BEH.HasError)
    return make_error<StringError>(BEH.ErrMsg, inconvertibleErrorCode());
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
    return make_error<StringError>(BEH.ErrMsg, inconvertibleErrorCode());
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

template <> class ResultSerializer<BlobError, Error> {
public:
  static WrapperFunctionResult serialize(Error Err) {
    WrapperFunctionResult R;
    if (!BlobArgList<BlobError>::toBlob(R, toBlobSerializable(std::move(Err))))
      return WrapperFunctionResult::createOutOfBandErrorFromStringLiteral(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename BlobT, typename T>
class ResultSerializer<BlobExpected<BlobT>, Expected<T>> {
public:
  static WrapperFunctionResult serialize(Expected<T> E) {
    WrapperFunctionResult R;
    if (!BlobArgList<BlobExpected<BlobT>>::toBlob(
            R, toBlobSerializable(std::move(E))))
      return WrapperFunctionResult::createOutOfBandErrorFromStringLiteral(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename BlobRetT, typename RetT> class ResultDeserializer {
public:
  static Error deserialize(RetT &Result, const char *ArgData, size_t ArgSize) {
    BlobInputBuffer BIB(ArgData, ArgSize);
    if (!BlobArgList<BlobRetT>::deserialize(BIB, Result))
      return make_error<StringError>(
          "Error deserializing return value from blob in call",
          inconvertibleErrorCode());
    return Error::success();
  }
};

template <> class ResultDeserializer<BlobError, Error> {
public:
  static Error deserialize(Error &Err, const char *ArgData, size_t ArgSize) {
    BlobInputBuffer BIB(ArgData, ArgSize);
    BlobErrorHelper BEH;
    if (!BlobArgList<BlobError>::deserialize(BIB, BEH)) {
      // If deserialization fails then the error result needn't be checked,
      // so consume it.
      consumeError(std::move(Err));
      return make_error<StringError>(
          "Error deserializing return value from blob in call",
          inconvertibleErrorCode());
    }
    ErrorAsOutParameter _(&Err);
    Err = fromBlobSerializable(std::move(BEH));
    return Error::success();
  }
};

template <typename BlobT, typename T>
class ResultDeserializer<BlobExpected<BlobT>, Expected<T>> {
public:
  static Error deserialize(Expected<T> &E, const char *ArgData,
                           size_t ArgSize) {
    BlobInputBuffer BIB(ArgData, ArgSize);
    BlobExpectedHelper<T> BEH;
    if (!BlobArgList<BlobExpected<BlobT>>::deserialize(BIB, BEH))
      return make_error<StringError>(
          "Error deserializing return value from blob in call",
          inconvertibleErrorCode());
    // E cannot be an error value.
    cantFail(E.takeError());
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
  static Error call(TargetProcessControl &TPC, JITTargetAddress FnTag,
                    RetT &Result, const ArgTs &...Args) {
    WrapperFunctionResult ArgBlob;
    if (!BlobArgList<BlobTs...>::toBlob(ArgBlob, Args...))
      return make_error<StringError>(
          "Error serializing arguments to blob in call",
          inconvertibleErrorCode());
    auto ResultBlob = TPC.runWrapper(FnTag, {ArgBlob.data(), ArgBlob.size()});
    if (!ResultBlob)
      return ResultBlob.takeError();
    if (auto ErrMsg = ResultBlob->getOutOfBandError())
      return make_error<StringError>(ErrMsg, inconvertibleErrorCode());

    return detail::ResultDeserializer<BlobRetT, RetT>::deserialize(
        Result, ResultBlob->data(), ResultBlob->size());
  }

  template <typename HandlerT>
  static WrapperFunctionResult handle(HandlerT &&Handler, const char *ArgData,
                                      size_t ArgSize) {
    using WFHH =
        WrapperFunctionHandlerHelper<HandlerT, ResultSerializer, BlobTs...>;
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
  static Error call(TargetProcessControl &TPC, JITTargetAddress FnTag,
                    const ArgTs &...Args) {
    BlobEmpty BE;
    return WrapperFunction<BlobEmpty(BlobTs...)>::call(TPC, FnTag, BE, Args...);
  }

  using WrapperFunction<BlobEmpty(BlobTs...)>::handle;
};

/// Return an ArrayRef covering the content of the given WrapperFunctionResult.
inline ArrayRef<char> toArrayRef(const WrapperFunctionResult &R) {
  return {R.data(), R.size()};
}

/// A utility to create a callable object for a wrapper function.
template <typename RetT, typename BlobSignature>
class WrapperFunctionCaller {
public:

  static Expected<WrapperFunctionCaller>
  Create(ExecutionSession &ES, TargetProcessControl &TPC,
         JITDylib &JD, SymbolStringPtr FunctionName) {
    if (auto FnTag = ES.lookup({&JD}, FunctionName))
      return WrapperFunctionCaller(TPC, FnTag->getAddress());
    else
      return FnTag.takeError();
  }

  template <typename... ArgTs>
  Expected<RetT> operator()(const ArgTs &... Args) {
    RetT Result;
    if (auto Err =
        WrapperFunction<BlobSignature>::call(TPC, FnTag, Result, Args...))
      return std::move(Err);
    return std::move(Result);
  }

private:

  WrapperFunctionCaller(TargetProcessControl &TPC, JITTargetAddress FnTag)
    : TPC(TPC), FnTag(FnTag) {}
  TargetProcessControl &TPC;
  JITTargetAddress FnTag = 0;
};

/// Specialization of WrapperFunctionCaller for void functions.
template <typename... BlobArgTs>
class WrapperFunctionCaller<void, void(BlobArgTs...)> {

  static Expected<WrapperFunctionCaller>
  Create(ExecutionSession &ES, TargetProcessControl &TPC,
         JITDylib &JD, SymbolStringPtr FunctionName) {
    if (auto FnTag = ES.lookup({&JD}, FunctionName))
      return WrapperFunctionCaller(TPC, FnTag->getAddress());
    else
      return FnTag.takeError();
  }

  template <typename... ArgTs>
  Error operator()(const ArgTs &... Args) {
    return WrapperFunction<void(BlobArgTs...)>::call(TPC, FnTag, Args...);
  }

private:

  WrapperFunctionCaller(TargetProcessControl &TPC, JITTargetAddress FnTag)
    : TPC(TPC), FnTag(FnTag) {}
  TargetProcessControl &TPC;
  JITTargetAddress FnTag = 0;
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_LLVMSIDEWRAPPERFUNCTIONUTILS_H
