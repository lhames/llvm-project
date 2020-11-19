//===-- cxa.h ---------------------------------------------------*- C++ -*-===//
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

#ifndef ORC_RT_CXA_H
#define ORC_RT_CXA_H

#include "common.h"

ORC_RT_INTERFACE int __orc_rt_cxa_atexit(void (*func)(void *), void *arg,
                                         void *dso_handle);

ORC_RT_INTERFACE void __orc_rt_cxa_finalize(void *dso_handle);

#endif // ORC_RT_CXA_H
