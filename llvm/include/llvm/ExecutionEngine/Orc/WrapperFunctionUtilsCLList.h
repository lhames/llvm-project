//===- WrapperFunctionUtilsCLList.h - serialize llvm::cl::lists -*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_WRAPPERFUNCTIONUTILSCLLIST_H
#define LLVM_EXECUTIONENGINE_ORC_WRAPPERFUNCTIONUTILSCLLIST_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include "llvm/Support/CommandLine.h"

namespace llvm {
namespace orc {
namespace shared {

template <typename BlobElementT, typename T>
class TrivialBlobSequenceSerialization<BlobElementT, cl::list<T>> {
public:
  static constexpr bool available = true;
};

template <typename BlobElementT, typename T>
class TrivialBlobSequenceDeserialization<BlobElementT, cl::list<T>> {
public:
  static constexpr bool available = true;

  using element_type = typename cl::list<T>::value_type;

  static void reserve(cl::list<T> &V, uint64_t Size) {}
  static void append(cl::list<T> &V, T E) { V.push_back(std::move(E)); }
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSCLLIST_H
