/*===-------- llvm-c/OrcShared.h - Orc Shared C bindings --------*- C++ -*-===*\
|*                                                                            *|
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM          *|
|* Exceptions.                                                                *|
|* See https://llvm.org/LICENSE.txt for license information.                  *|
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    *|
|*                                                                            *|
|*===----------------------------------------------------------------------===*|
|*                                                                            *|
|* This header declares the C interface to libLLVMOrcShared.a, which          *|
|* implements routines shared by both C ORC JITs and target processes.        *|
|*                                                                            *|
|* Many exotic languages can interoperate with C code but have a harder time  *|
|* with C++ due to name mangling. So in addition to C, this interface enables *|
|* tools written in such languages.                                           *|
|*                                                                            *|
|* Note: This interface is experimental. It is *NOT* stable, and may be       *|
|*       changed without warning. Only C API usage documentation is           *|
|*       provided. See the C++ documentation for all higher level ORC API     *|
|*       details.                                                             *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef LLVM_C_ORCSHARED_H
#define LLVM_C_ORCSHARED_H

#include "llvm-c/Error.h"
#include <stddef.h>
#include <stdint.h>

LLVM_C_EXTERN_C_BEGIN

typedef union {
  char Value[sizeof(const char *)];
  const char *ValuePtr;
} LLVMOrcSharedCWrapperFunctionResultData;

typedef void (*LLVMOrcSharedCWrapperFunctionResultDataDestructor)(
    LLVMOrcSharedCWrapperFunctionResultData, uint64_t Size);

typedef struct {
  uint64_t Size;
  LLVMOrcSharedCWrapperFunctionResultData Data;
  LLVMOrcSharedCWrapperFunctionResultDataDestructor Destroy;
} LLVMOrcSharedCWrapperFunctionResult;

typedef struct LLVMOrcSharedOpaqueJITProcessControl
    *LLVMOrcSharedJITProcessControlRef;

/**
 * Zero-initialize an LLVMOrcSharedCWrapperFunctionResult.
 */
inline void
LLVMOrcSharedCWraperFunctionResultInit(LLVMOrcSharedCWrapperFunctionResult *R) {
  R->Size = 0;
  R->Data.ValuePtr = 0;
  R->Destroy = 0;
}

/**
 * Create an LLVMOrcSharedCWrapperFunctionResult representing an out-of-band
 * error.
 */
inline LLVMOrcSharedCWrapperFunctionResult
LLVMOrcSharedCreateCWrapperFunctionResultFromOutOfBandError(
    const char *ErrMsg,
    LLVMOrcSharedCWrapperFunctionResultDataDestructor Destroy) {
  LLVMOrcSharedCWrapperFunctionResult R;
  R.Size = 0;
  R.Data.ValuePtr = ErrMsg;
  R.Destroy = Destroy;
  return R;
}

/**
 * This should be called to destroy LLVMOrcSharedCWrapperFunctionResult values
 * regardless of their state.
 */
inline void LLVMOrcSharedDisposeCWrapperFunctionResult(
    LLVMOrcSharedCWrapperFunctionResult *R) {
  if (R->Destroy)
    R->Destroy(R->Data, R->Size);
}

/**
 * Returns a pointer to the out-of-band error string for this
 * LLVMOrcSharedCWrapperFunctionResult, or null if there is no error.
 *
 * The LLVMOrcSharedCWrapperFunctionResult retains ownership of the error
 * string, so it should be copied if the caller wishes to preserve it.
 */
inline const char *LLVMOrcSharedCWrapperFunctionResultGetOutOfBandError(
    LLVMOrcSharedCWrapperFunctionResult *R) {
  return R->Size == 0 ? R->Data.ValuePtr : 0;
}

/**
 * Get a pointer to the data contained in the given
 * LLVMOrcSharedCWrapperFunctionResult.
 */
inline const char *LLVMOrcSharedCWrapperFunctionResultGetData(
    LLVMOrcSharedCWrapperFunctionResult *R) {
  return R->Size > sizeof(R->Data.Value) ? R->Data.Value : R->Data.ValuePtr;
}

LLVM_C_EXTERN_C_END

#endif /* LLVM_C_ORCSHARED_H */
