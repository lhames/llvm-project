//===------------ Dispatch.h - Blob function dispatch -----------*- C++ -*-===//
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

#ifndef ORC_RT_DISPATCH_H
#define ORC_RT_DISPATCH_H

#include "common.h"
#include "error.h"

/// Context object for dispatching calls to the JIT object.
extern "C" Opaque __orc_rt_jit_dispatch_ctx __attribute__((weak_import));

/// For dispatching calls to the JIT object.
extern "C" LLVMOrcSharedCWrapperFunctionResult
__orc_rt_jit_dispatch(Opaque *DispatchCtx, const void *FnTag, const char *Data,
                      size_t Size);

#endif // ORC_RT_DISPATCH_H
