//===- wrapper_function_utils.cpp -----------------------------------------===//
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

#include "wrapper_function_utils.h"

#include <cstdio>

namespace __orc_rt {

void dumpBuffer(const char *Buffer, size_t Size) {
  fprintf(stderr, "Buffer of size %li at %p:\n", Size, (void *)Buffer);
  for (size_t I = 0; I != Size; ++I) {
    if (I % 16 == 0)
      fprintf(stderr, " ");
    fprintf(stderr, " 0x%02x", Buffer[I]);
    if (I % 16 == 15)
      fprintf(stderr, "\n");
  }
  if (Size % 16)
    fprintf(stderr, "\n");
}

void dumpBuffer(WrapperFunctionResult &R) { dumpBuffer(R.data(), R.size()); }

} // end namespace __orc_rt
