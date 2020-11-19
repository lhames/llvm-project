//===---- WrapperFunctionUtilsErrors.h - Serializable errors ----*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSERRORS_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSERRORS_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtilsStdString.h"

#include <cassert>

namespace llvm {
namespace orc {
namespace shared {

/// Blob tag type for errors.
class BlobError;

/// Blob tag type for expecteds, which are either a T or a string representing
/// an error.
template <typename BlobT> class BlobExpected;

/// Helper type for serializing Errors.
///
/// llvm::Errors are move-only, and not inspectable except by consuming them.
/// This makes them unsuitable for direct serialization via
/// BlobSerializationTraits, which needs to inspect values twice (once to
/// determine the amount of space to reserve, and then again to serialize).
///
/// The WrapperFunctionSerializableError type is a helper that can be
/// constructed from an llvm::Error, but inspected more than once.
struct BlobErrorHelper {
  bool HasError = false;
  std::string ErrMsg;
};

/// Helper type for serializing Expected<T>s.
///
/// See BlobErrorHelper for more details.
///
// FIXME: Use std::variant for storage once we have c++17.
template <typename T> struct BlobExpectedHelper {
  bool HasValue = false;
  T Value{};
  std::string ErrMsg;
};

/// Serialize to a BlobError from a BlobErrorHelper.
template <> class BlobSerializationTraits<BlobError, BlobErrorHelper> {
public:
  static size_t size(const BlobErrorHelper &BEH) {
    size_t Size = BlobArgList<bool>::size(BEH.HasError);
    if (BEH.HasError)
      Size += BlobArgList<BlobString>::size(BEH.ErrMsg);
    return Size;
  }

  static bool serialize(BlobOutputBuffer &BOB, const BlobErrorHelper &BEH) {
    if (!BlobArgList<bool>::serialize(BOB, BEH.HasError))
      return false;
    if (BEH.HasError)
      if (!BlobArgList<BlobString>::serialize(BOB, BEH.ErrMsg))
        return false;
    return true;
  }

  static bool deserialize(BlobInputBuffer &BIB, BlobErrorHelper &BEH) {
    if (!BlobArgList<bool>::deserialize(BIB, BEH.HasError))
      return false;

    if (!BEH.HasError)
      return true;

    return BlobArgList<BlobString>::deserialize(BIB, BEH.ErrMsg);
  }
};

/// Serialize to a BlobExpected<BlobT> from an BlobExpectedHelper<T>.
template <typename BlobT, typename T>
class BlobSerializationTraits<BlobExpected<BlobT>, BlobExpectedHelper<T>> {
public:
  static size_t size(const BlobExpectedHelper<T> &BEH) {
    size_t Size = BlobArgList<bool>::size(BEH.HasValue);
    if (BEH.HasValue)
      Size += BlobArgList<BlobT>::size(BEH.Value);
    else
      Size += BlobArgList<BlobString>::size(BEH.ErrMsg);
    return Size;
  }

  static bool serialize(BlobOutputBuffer &BOB,
                        const BlobExpectedHelper<T> &BEH) {
    if (!BlobArgList<bool>::serialize(BOB, BEH.HasValue))
      return false;

    if (BEH.HasValue)
      return BlobArgList<BlobT>::serialize(BOB, BEH.Value);

    return BlobArgList<BlobString>::serialize(BOB, BEH.ErrMsg);
  }

  static bool deserialize(BlobInputBuffer &BIB, BlobExpectedHelper<T> &BEH) {
    if (!BlobArgList<bool>::deserialize(BIB, BEH.HasValue))
      return false;

    if (BEH.HasValue)
      return BlobArgList<BlobT>::deserialize(BIB, BEH.Value);

    return BlobArgList<BlobString>::deserialize(BIB, BEH.ErrMsg);
  }
};

/// Serialize to a BlobExpected<BlobT> from a BlobErrorHelper.
template <typename BlobT>
class BlobSerializationTraits<BlobExpected<BlobT>, BlobErrorHelper> {
public:
  static size_t size(const BlobErrorHelper &BEH) {
    assert(BEH.HasError && "Cannot serialize expected from a success value");
    return BlobArgList<bool>::size(false) +
           BlobArgList<BlobString>::size(BEH.ErrMsg);
  }

  static bool serialize(BlobOutputBuffer &BOB, const BlobErrorHelper &BEH) {
    assert(BEH.HasError && "Cannot serialize expected from a success value");
    if (!BlobArgList<bool>::serialize(BOB, false))
      return false;
    return BlobArgList<BlobString>::serialize(BOB, BEH.ErrMsg);
  }
};

/// Serialize to a BlobExpected<BlobT> from a T.
template <typename BlobT, typename T>
class BlobSerializationTraits<BlobExpected<BlobT>, T> {
public:
  static size_t size(const T &Value) {
    return BlobArgList<bool>::size(true) + BlobArgList<BlobT>::size(Value);
  }

  static bool serialize(BlobOutputBuffer &BOB, const T &Value) {
    if (!BlobArgList<bool>::serialize(BOB, true))
      return false;
    return BlobArgList<BlobT>::serialize(Value);
  }
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSERRORS_H
