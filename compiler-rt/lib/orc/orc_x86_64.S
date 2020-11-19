//===-- orc_x86_64.s --------------------------------------------*- ASM -*-===//
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

#include "../builtins/assembly.h"

	.text
#if !defined(__APPLE__)
	.section .text
	.file "orc_x86_64.S"
#else
	.section __TEXT,__text
#endif

//===----------------------------------------------------------------------===//

DEFINE_COMPILERRT_FUNCTION(__orc_helloWorldEntry)
Lentry:
  jmp ___orc_helloWorld
END_COMPILERRT_FUNCTION(__orc_helloWorldEntry)

//===----------------------------------------------------------------------===//


NO_EXEC_STACK_DIRECTIVE
