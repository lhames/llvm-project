//===- extensible_rtti.cpp ------------------------------------------------===//
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

#include "extensible_rtti.h"

namespace __orc_rt {

char RTTIRoot::ID = 0;
void RTTIRoot::anchor() {}

} // end namespace __orc_rt
