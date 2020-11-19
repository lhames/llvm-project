//===- WrapperFunctionUtils.h - Serialization for Wrapper Funcs -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Serialization / deserialization machinery for wrapper functions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILS_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILS_H

#include "llvm-c/OrcShared.h"
#include "llvm/Support/SwapByteOrder.h"

#include <string>
#include <utility>

namespace llvm {
namespace orc {
namespace shared {

/// C++ wrapper function result: Same as CWrapperFunctionResult but
/// auto-releases memory.
class WrapperFunctionResult {
public:
  /// Create a default WrapperFunctionResult.
  WrapperFunctionResult() { zeroInit(R); }

  /// Create a WrapperFunctionResult from a CWrapperFunctionResult. This
  /// instance takes ownership of the result object and will automatically
  /// call the Destroy member upon destruction.
  WrapperFunctionResult(LLVMOrcSharedCWrapperFunctionResult R) : R(R) {}

  WrapperFunctionResult(const WrapperFunctionResult &) = delete;
  WrapperFunctionResult &operator=(const WrapperFunctionResult &) = delete;

  WrapperFunctionResult(WrapperFunctionResult &&Other) {
    zeroInit(R);
    std::swap(R, Other.R);
  }

  WrapperFunctionResult &operator=(WrapperFunctionResult &&Other) {
    LLVMOrcSharedCWrapperFunctionResult Tmp;
    zeroInit(Tmp);
    std::swap(Tmp, Other.R);
    std::swap(R, Tmp);
    return *this;
  }

  ~WrapperFunctionResult() {
    if (R.Destroy)
      R.Destroy(R.Data, R.Size);
  }

  /// Relinquish ownership of and return the
  /// LLVMOrcSharedCWrapperFunctionResult.
  LLVMOrcSharedCWrapperFunctionResult release() {
    LLVMOrcSharedCWrapperFunctionResult Tmp;
    zeroInit(Tmp);
    std::swap(R, Tmp);
    return Tmp;
  }

  /// Get an ArrayRef covering the data in the result.
  const char *data() const {
    return R.Size > sizeof(R.Data) ? R.Data.ValuePtr : R.Data.Value;
  }

  /// Returns the size of the data contained in this instance.
  size_t size() const { return R.Size; }

  /// Returns true if this value is equivalent to a default-constructed
  /// WrapperFunctionResult.
  bool empty() const {
    return R.Size == 0 && R.Data.ValuePtr == nullptr && R.Destroy == nullptr;
  }

  /// Copy from the given char range.
  static WrapperFunctionResult copyFrom(const char *Source, size_t Size) {
    LLVMOrcSharedCWrapperFunctionResult R;
    R.Size = Size;
    char *Data;
    if (R.Size > sizeof(R.Data)) {
      Data = new char[R.Size];
      R.Data.ValuePtr = Data;
      R.Destroy = destroyWithArrayDelete;
    } else {
      Data = R.Data.Value;
      R.Destroy = nullptr;
    }
    memcpy(Data, Source, Size);
    return R;
  }

  /// Copy from the given null-terminated string (includes the null-terminator).
  static WrapperFunctionResult copyFrom(const char *Source) {
    return copyFrom(Source, strlen(Source) + 1);
  }

  /// Copy from the given std::string (includes the null terminator).
  static WrapperFunctionResult copyFrom(const std::string &Source) {
    return copyFrom(Source.data(), Source.size() + 1);
  }

  /// Create a WrapperFunctionResult from a constant string. Does not copy or
  /// free the input string.
  static WrapperFunctionResult fromStringLiteral(const char *Msg) {
    LLVMOrcSharedCWrapperFunctionResult R;
    R.Size = strlen(Msg) + 1;
    R.Destroy = nullptr;
    if (R.Size > sizeof(R.Data))
      R.Data.ValuePtr = Msg;
    else
      memcpy(R.Data.Value, Msg, R.Size);
    return R;
  }

  /// Create an out-of-band error from a string literal.
  static WrapperFunctionResult
  createOutOfBandErrorFromStringLiteral(const char *Msg) {
    LLVMOrcSharedCWrapperFunctionResult R;
    R.Size = 0;
    R.Data.ValuePtr = Msg;
    R.Destroy = nullptr;
    return R;
  }

  /// Create an out-of-band error by copying the given string.
  static WrapperFunctionResult createOutOfBandErrorFromString(const char *Msg) {
    size_t BufferLen = strlen(Msg) + 1;
    auto Data = std::make_unique<char[]>(BufferLen);
    memcpy(Data.get(), Msg, BufferLen);

    LLVMOrcSharedCWrapperFunctionResult R;
    R.Size = 0;
    R.Data.ValuePtr = Data.release();
    R.Destroy = WrapperFunctionResult::destroyWithArrayDelete;
    return R;
  }

  /// If this value is an out-of-band error then this returns the error message,
  /// otherwise returns nullptr.
  const char *getOutOfBandError() const {
    return R.Size == 0 ? R.Data.ValuePtr : nullptr;
  }

  /// Always free Data.ValuePtr by calling delete[] on it.
  static void
  destroyWithArrayDelete(LLVMOrcSharedCWrapperFunctionResultData Data,
                         uint64_t Size) {
    delete[] Data.ValuePtr;
  }

private:
  static void zeroInit(LLVMOrcSharedCWrapperFunctionResult &R) {
    R.Size = 0;
    R.Data.ValuePtr = nullptr;
    R.Destroy = nullptr;
  }

  LLVMOrcSharedCWrapperFunctionResult R;
};

/// Output char buffer with overflow check.
class BlobOutputBuffer {
public:
  BlobOutputBuffer(char *Buffer, size_t Remaining)
      : Buffer(Buffer), Remaining(Remaining) {}
  bool write(const char *Data, size_t Size) {
    if (Size > Remaining)
      return false;
    memcpy(Buffer, Data, Size);
    Buffer += Size;
    Remaining -= Size;
    return true;
  }

private:
  char *Buffer = nullptr;
  size_t Remaining = 0;
};

/// Input char buffer with underflow check.
class BlobInputBuffer {
public:
  BlobInputBuffer() = default;
  BlobInputBuffer(const char *Buffer, size_t Remaining)
      : Buffer(Buffer), Remaining(Remaining) {}
  bool read(char *Data, size_t Size) {
    if (Size > Remaining)
      return false;
    memcpy(Data, Buffer, Size);
    Buffer += Size;
    Remaining -= Size;
    return true;
  }

  const char *data() const { return Buffer; }
  bool skip(size_t Size) {
    if (Size > Remaining)
      return false;
    Remaining -= Size;
    return true;
  }

private:
  const char *Buffer = nullptr;
  size_t Remaining = 0;
};

/// Specialize to describe how to serialize/deserialize to/from the given
/// concrete type.
template <typename BlobT, typename ConcreteT, typename _ = void>
class BlobSerializationTraits;

/// A utility class for serializing to a blob from a variadic list.
template <typename... ArgTs> class BlobArgList;

template <> class BlobArgList<> {
public:
  static size_t size() { return 0; }

  static bool serialize(BlobOutputBuffer &BOB) { return true; }
  static bool deserialize(BlobInputBuffer &BIB) { return true; }

  static bool toBlob(WrapperFunctionResult &R) {
    R = WrapperFunctionResult();
    return true;
  }
};

template <typename BlobT, typename... BlobTs>
class BlobArgList<BlobT, BlobTs...> {
public:
  template <typename ArgT, typename... ArgTs>
  static size_t size(const ArgT &Arg, const ArgTs &...Args) {
    return BlobSerializationTraits<BlobT, ArgT>::size(Arg) +
           BlobArgList<BlobTs...>::size(Args...);
  }

  template <typename ArgT, typename... ArgTs>
  static bool serialize(BlobOutputBuffer &BOB, const ArgT &Arg,
                        const ArgTs &...Args) {
    return BlobSerializationTraits<BlobT, ArgT>::serialize(BOB, Arg) &&
           BlobArgList<BlobTs...>::serialize(BOB, Args...);
  }

  template <typename ArgT, typename... ArgTs>
  static bool deserialize(BlobInputBuffer &BIB, ArgT &Arg, ArgTs &...Args) {
    return BlobSerializationTraits<BlobT, ArgT>::deserialize(BIB, Arg) &&
           BlobArgList<BlobTs...>::deserialize(BIB, Args...);
  }

  template <typename... ArgTs>
  static bool toBlob(WrapperFunctionResult &R, const ArgTs &...Args) {
    LLVMOrcSharedCWrapperFunctionResult CR;

    CR.Size = size(Args...);
    char *DataPtr = nullptr;
    if (CR.Size > sizeof(LLVMOrcSharedCWrapperFunctionResultData)) {
      DataPtr = new char[CR.Size];
      CR.Data.ValuePtr = DataPtr;
      CR.Destroy = WrapperFunctionResult::destroyWithArrayDelete;
    } else {
      DataPtr = &CR.Data.Value[0];
      CR.Destroy = nullptr;
    }

    BlobOutputBuffer BOB(DataPtr, CR.Size);
    if (!serialize(BOB, Args...)) {
      LLVMOrcSharedDisposeCWrapperFunctionResult(&CR);
      return false;
    }

    R = WrapperFunctionResult(CR);
    return true;
  }

  template <typename... ArgTs>
  bool fromBlob(const char *Data, size_t Size, ArgTs &...Args) {
    BlobInputBuffer BIB(Data, Size);
    return deserialize(BIB, Args...);
  }
};

template <typename BlobT>
class BlobSerializationTraits<
    BlobT, BlobT,
    std::enable_if_t<std::is_same<BlobT, bool>::value ||
                     std::is_same<BlobT, char>::value ||
                     std::is_same<BlobT, int8_t>::value ||
                     std::is_same<BlobT, int16_t>::value ||
                     std::is_same<BlobT, int32_t>::value ||
                     std::is_same<BlobT, int64_t>::value ||
                     std::is_same<BlobT, uint8_t>::value ||
                     std::is_same<BlobT, uint16_t>::value ||
                     std::is_same<BlobT, uint32_t>::value ||
                     std::is_same<BlobT, uint64_t>::value>> {
public:
  static size_t size(const BlobT &Value) { return sizeof(BlobT); }

  static bool serialize(BlobOutputBuffer &BOB, const BlobT &Value) {
    BlobT Tmp = Value;
    if (!sys::IsBigEndianHost)
      sys::swapByteOrder(Tmp);
    return BOB.write(reinterpret_cast<const char *>(&Tmp), sizeof(Tmp));
  }

  static bool deserialize(BlobInputBuffer &BIB, BlobT &Value) {
    BlobT Tmp;
    if (!BIB.read(reinterpret_cast<char *>(&Tmp), sizeof(Tmp)))
      return false;
    if (!sys::IsBigEndianHost)
      sys::swapByteOrder(Tmp);
    Value = Tmp;
    return true;
  }
};

/// Blob tag type for target addresses.
///
/// BlobTargetAddresses should be serialized as a uint64_t value.
class BlobTargetAddress;

template <>
class BlobSerializationTraits<BlobTargetAddress, uint64_t>
    : public BlobSerializationTraits<uint64_t, uint64_t> {};

/// Blob tag type for tuples.
///
/// A blob tuple should be serialized by serializing each of the elements in
/// sequence.
template <typename... BlobElementTs> class BlobTuple {
public:
  /// Convenience typedef of the corresponding arg list.
  typedef BlobArgList<BlobElementTs...> AsArgList;
};

/// Blob tag type for sequences.
///
/// BlobSequences should be serialized as a uint64_t sequence length,
/// followed by the serialization of each of the elements.
template <typename BlobElementT> class BlobSequence;

/// Blob tag type for strings, which are equivalent to sequences of chars.
using BlobString = BlobSequence<char>;

/// Blob tag type for maps.
///
/// Blob maps are just sequences of (Key, Value) tuples.
template <typename BlobT1, typename BlobT2>
using BlobMap = BlobSequence<BlobTuple<BlobT1, BlobT2>>;

/// Specialize this to implement 'trivial' sequence serialization for
/// a concrete sequence type.
///
/// Trivial sequence serialization uses the sequence's 'size' member to get the
/// length of the sequence, and uses a range-based for loop to iterate over the
/// elements.
///
/// Specializing this template class means that you do not need to provide a
/// specialization of BlobSerializationTraits for your type.
template <typename BlobElementT, typename ConcreteSequenceT>
class TrivialBlobSequenceSerialization {
public:
  static constexpr bool available = false;
};

/// Specialize this to implement 'trivial' sequence deserialization for
/// a concrete sequence type.
///
/// Trivial deserialization calls a static 'reserve(SequenceT&)' method on your
/// specialization (you must implement this) to reserve space, and then calls
/// a static 'append(SequenceT&, ElementT&) method to append each of the
/// deserialized elements.
///
/// Specializing this template class means that you do not need to provide a
/// specialization of BlobSerializationTraits for your type.
template <typename BlobElementT, typename ConcreteSequenceT>
class TrivialBlobSequenceDeserialization {
public:
  static constexpr bool available = false;
};

/// 'Trivial' sequence serialization: Sequence is serialized as a uint64_t size
/// followed by a for-earch loop over the elements of the sequence to serialize
/// each of them.
template <typename BlobElementT, typename SequenceT>
class BlobSerializationTraits<BlobSequence<BlobElementT>, SequenceT,
                              std::enable_if_t<TrivialBlobSequenceSerialization<
                                  BlobElementT, SequenceT>::available>> {
public:
  static size_t size(const SequenceT &S) {
    size_t Size = BlobArgList<uint64_t>::size(static_cast<uint64_t>(S.size()));
    for (const auto &E : S)
      Size += BlobArgList<BlobElementT>::size(E);
    return Size;
  }

  static bool serialize(BlobOutputBuffer &BOB, const SequenceT &S) {
    if (!BlobArgList<uint64_t>::serialize(BOB, static_cast<uint64_t>(S.size())))
      return false;
    for (const auto &E : S)
      if (!BlobArgList<BlobElementT>::serialize(BOB, E))
        return false;
    return true;
  }

  static bool deserialize(BlobInputBuffer &BIB, SequenceT &S) {
    using TBSD = TrivialBlobSequenceDeserialization<BlobElementT, SequenceT>;
    uint64_t Size;
    if (!BlobArgList<uint64_t>::deserialize(BIB, Size))
      return false;
    TBSD::reserve(S, Size);
    for (size_t I = 0; I != Size; ++I) {
      typename TBSD::element_type E;
      if (!BlobArgList<BlobElementT>::deserialize(BIB, E))
        return false;
      if (!TBSD::append(S, std::move(E)))
        return false;
    }
    return true;
  }
};

/// BlobTuple serialization for std::pair.
template <typename BlobT1, typename BlobT2, typename T1, typename T2>
class BlobSerializationTraits<BlobTuple<BlobT1, BlobT2>, std::pair<T1, T2>> {
public:
  static size_t size(const std::pair<T1, T2> &P) {
    return BlobArgList<BlobT1>::size(P.first) +
           BlobArgList<BlobT2>::size(P.second);
  }

  static bool serialize(BlobOutputBuffer &BOB, const std::pair<T1, T2> &P) {
    return BlobArgList<BlobT1>::serialize(BOB, P.first) &&
           BlobArgList<BlobT2>::serialize(BOB, P.second);
  }

  static bool deserialize(BlobInputBuffer &BIB, std::pair<T1, T2> &P) {
    return BlobArgList<BlobT1>::deserialize(BIB, P.first) &&
           BlobArgList<BlobT2>::deserialize(BIB, P.second);
  }
};

// Any empty placeholder suitable as a substitute for void when deserializing
class BlobEmpty {};

template <> class BlobSerializationTraits<BlobEmpty, BlobEmpty> {
public:
  static size_t size(const BlobEmpty &EP) { return 0; }
  static bool serialize(BlobOutputBuffer &BOB, const BlobEmpty &BE) {
    return true;
  }
  static bool deserialize(BlobInputBuffer &BIB, BlobEmpty &BE) { return true; }
};

template <typename WrapperFunctionImplT,
          template <typename> class ResultSerializer, typename... BlobTs>
class WrapperFunctionHandlerHelper
    : public WrapperFunctionHandlerHelper<
          decltype(&std::remove_reference_t<WrapperFunctionImplT>::operator()),
          ResultSerializer, BlobTs...> {};

template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... BlobTs>
class WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                   BlobTs...> {
public:
  using ArgTuple = std::tuple<std::decay_t<ArgTs>...>;
  using ArgIndices = std::make_index_sequence<std::tuple_size<ArgTuple>::value>;

  template <typename HandlerT>
  static WrapperFunctionResult apply(HandlerT &&H, const char *ArgData,
                                     size_t ArgSize) {
    ArgTuple Args;
    if (!deserialize(ArgData, ArgSize, Args, ArgIndices{}))
      return WrapperFunctionResult::createOutOfBandErrorFromStringLiteral(
          "Could not deserialize arguments for wrapper function call");

    return ResultSerializer<RetT>::serialize(
        call(std::forward<HandlerT>(H), Args, ArgIndices{}));
  }

private:
  template <std::size_t... I>
  static bool deserialize(const char *ArgData, size_t ArgSize, ArgTuple &Args,
                          std::index_sequence<I...>) {
    BlobInputBuffer BIB(ArgData, ArgSize);
    return BlobArgList<BlobTs...>::deserialize(BIB, std::get<I>(Args)...);
  }

  template <typename HandlerT, std::size_t... I>
  static decltype(auto) call(HandlerT &&H, ArgTuple &Args,
                             std::index_sequence<I...>) {
    return std::forward<HandlerT>(H)(std::get<I>(Args)...);
  }
};

// Map function references to function types.
template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... BlobTs>
class WrapperFunctionHandlerHelper<RetT (&)(ArgTs...), ResultSerializer,
                                   BlobTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          BlobTs...> {};

// Map non-const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... BlobTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...), ResultSerializer,
                                   BlobTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          BlobTs...> {};

// Map const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... BlobTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...) const,
                                   ResultSerializer, BlobTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          BlobTs...> {};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILS_H
