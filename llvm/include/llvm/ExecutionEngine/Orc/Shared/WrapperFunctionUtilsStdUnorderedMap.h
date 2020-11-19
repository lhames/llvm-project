//===------------ WrapperFunctionUtilsStdUnorderedMap.h ---------*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDUNORDEREDMAP_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDUNORDEREDMAP_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include <unordered_map>

namespace llvm {
namespace orc {
namespace shared {

template <typename BlobT1, typename BlobT2, typename T1, typename T2>
class TrivialBlobSequenceSerialization<BlobTuple<BlobT1, BlobT2>,
                                       std::unordered_map<T1, T2>> {
public:
  static constexpr bool available = true;
};

template <typename BlobT1, typename BlobT2, typename T1, typename T2>
class TrivialBlobSequenceDeserialization<BlobTuple<BlobT1, BlobT2>,
                                         std::unordered_map<T1, T2>> {
public:
  static constexpr bool available = true;

  using element_type = std::pair<T1, T2>;

  static void reserve(std::unordered_map<T1, T2> &M, uint64_t Size) {
    M.reserve(Size);
  }
  static bool append(std::unordered_map<T1, T2> &M,
                     typename std::unordered_map<T1, T2>::value_type P) {
    return M.insert(std::move(P)).second;
  }
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDUNORDEREDMAP_H
