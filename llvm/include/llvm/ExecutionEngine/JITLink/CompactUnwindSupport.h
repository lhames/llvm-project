//===-- CompactUnwindSupport.h - Compact Unwind format support --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Compact Unwind format support.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORT_H
#define LLVM_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORT_H

#include "llvm/ExecutionEngine/JITLink/JITLink.h"

namespace llvm {
namespace jitlink {

/// Split blocks in an __LD,__compact_unwind section on record boundaries.
/// When this function returns edges within each record are guaranteed to be
/// sorted by offset.
Error splitCompactUnwindBlocks(LinkGraph &G, Section &CompactUnwindSection,
                               size_t RecordSize);

} // end namespace jitlink
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_JITLINK_COMPACTUNWINDSUPPORT_H
