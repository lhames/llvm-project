//===- dlfcn_wrapper.cpp --------------------------------------------------===//
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

#include "adt.h"
#include "common.h"
#include "wrapper_function_utils.h"

#include <vector>

using namespace orc_rt;

extern "C" const char *__orc_rt_jit_dlerror();
extern "C" void *__orc_rt_jit_dlopen(const char *path, int mode);
extern "C" int __orc_rt_jit_dlupdate(void *dso_handle);
extern "C" int __orc_rt_jit_dlclose(void *dso_handle);

ORC_RT_INTERFACE void __orc_rt_jit_dlerror_wrapper(const char *ArgData,
                                                   size_t ArgSize,
                                                   void *SessionCtx,
                                                   uintptr_t MsgCtx,
                                                   orc_rt_YieldFn Yield) {
  WrapperFunction<SPSString()>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      []() { return std::string(__orc_rt_jit_dlerror()); });
}

ORC_RT_INTERFACE void __orc_rt_jit_dlopen_wrapper(const char *ArgData,
                                                  size_t ArgSize,
                                                  void *SessionCtx,
                                                  uintptr_t MsgCtx,
                                                  orc_rt_YieldFn Yield) {
  WrapperFunction<SPSExecutorAddr(SPSString, int32_t)>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](const std::string &Path, int32_t mode) {
        return ExecutorAddr::fromPtr(__orc_rt_jit_dlopen(Path.c_str(), mode));
      });
}

#ifndef _WIN32
ORC_RT_INTERFACE void __orc_rt_jit_dlupdate_wrapper(const char *ArgData,
                                                    size_t ArgSize,
                                                    void *SessionCtx,
                                                    uintptr_t MsgCtx,
                                                    orc_rt_YieldFn Yield) {
  WrapperFunction<int32_t(SPSExecutorAddr)>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](ExecutorAddr &DSOHandle) {
        return __orc_rt_jit_dlupdate(DSOHandle.toPtr<void *>());
      });
}
#endif

ORC_RT_INTERFACE void __orc_rt_jit_dlclose_wrapper(const char *ArgData,
                                                   size_t ArgSize,
                                                   void *SessionCtx,
                                                   uintptr_t MsgCtx,
                                                   orc_rt_YieldFn Yield) {
  WrapperFunction<int32_t(SPSExecutorAddr)>::handleAsyncWithSync(
      ArgData, ArgSize, CYield(SessionCtx, MsgCtx, Yield),
      [](ExecutorAddr &DSOHandle) {
        return __orc_rt_jit_dlclose(DSOHandle.toPtr<void *>());
      });
}
