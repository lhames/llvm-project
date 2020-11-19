//===- common.h - Common utilities for the ORC runtime ----------*- C++ -*-===//
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

#ifndef ORC_RT_COMMON_H
#define ORC_RT_COMMON_H

#include "adt.h"
#include "compiler.h"
#include "error.h"

#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <type_traits>

/// Opaque struct for external symbols.
struct Opaque {};

/// Error reporting function.
extern "C" void __orc_rt_log_error(const char *ErrMsg);

namespace __orc_rt {

/// Must be kept in sync with JITSymbol.h
using JITTargetAddress = uint64_t;

/// Cast from JITTargetAddress to pointer.
template <typename T> T jitTargetAddressToPointer(JITTargetAddress Addr) {
  static_assert(std::is_pointer<T>::value, "T must be a pointer type");
  return reinterpret_cast<T>(static_cast<uintptr_t>(Addr));
}

/// Convert a JITTargetAddress to a callable function pointer.
template <typename T> T jitTargetAddressToFunction(JITTargetAddress Addr) {
  static_assert(std::is_pointer<T>::value &&
                    std::is_function<std::remove_pointer_t<T>>::value,
                "T must be a function pointer type");
  return jitTargetAddressToPointer<T>(Addr);
}

/// Cast from pointer to JITTargetAddress.
template <typename T> JITTargetAddress pointerToJITTargetAddress(T *Ptr) {
  return static_cast<JITTargetAddress>(reinterpret_cast<uintptr_t>(Ptr));
}

} // end namespace __orc_rt

#endif // ORC_RT_COMMON_H
