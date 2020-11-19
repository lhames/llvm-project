//===- WrapperFunctionUtilsStdString.h - serialize std::strings -*- C++ -*-===//
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

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDSTRING_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDSTRING_H

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include <string>

namespace llvm {
namespace orc {
namespace shared {

template <> class TrivialBlobSequenceSerialization<char, std::string> {
public:
  static constexpr bool available = true;
};

template <> class TrivialBlobSequenceDeserialization<char, std::string> {
public:
  static constexpr bool available = true;

  using element_type = char;

  static void reserve(std::string &S, uint64_t Size) { S.reserve(Size); }
  static bool append(std::string &S, char C) {
    S.push_back(C);
    return true;
  }
};

} // end namespace shared
} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_WRAPPERFUNCTIONUTILSSTDSTRING_H
