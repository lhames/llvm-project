//===- WrapperFunctionUtilsStdVector.h - serialize std::vectors -*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDVECTOR_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDVECTOR_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include <vector>

namespace llvm {
namespace orc {
namespace shared {

template <typename BlobElementT, typename T>
class TrivialBlobSequenceSerialization<BlobElementT, std::vector<T>> {
public:
  static constexpr bool available = true;
};

template <typename BlobElementT, typename T>
class TrivialBlobSequenceDeserialization<BlobElementT, std::vector<T>> {
public:
  static constexpr bool available = true;

  using element_type = typename std::vector<T>::value_type;

  static void reserve(std::vector<T> &V, uint64_t Size) { V.reserve(Size); }
  static bool append(std::vector<T> &V, T E) {
    V.push_back(std::move(E));
    return true;
  }
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDVECTOR_H
