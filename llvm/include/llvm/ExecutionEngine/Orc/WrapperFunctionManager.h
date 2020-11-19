//===--- WrapperFunctionManager.h - Manage wrapper functions ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Associates tags with wrapper functions and allows execution of wrapper
// functions by tag.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_WRAPPERFUNCTIONMANAGER_H
#define LLVM_EXECUTIONENGINE_ORC_WRAPPERFUNCTIONMANAGER_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h"
#include <memory>
#include <mutex>
#include <vector>

namespace llvm {
namespace orc {

/// Allows mappings to be created between functions inside the JIT and
/// function tags (represented by JITTargetAddresses) that can be used to call
/// them via __orc_rt_call_jit_function in the executor.
class WrapperFunctionManager {
public:
  using WrapperFunction =
      unique_function<shared::WrapperFunctionResult(ArrayRef<char> ArgBuffer)>;

  using WrapperFunctionMap = DenseMap<JITTargetAddress, WrapperFunction>;

  /// Associate the given address with the given wrapper function in the JIT.
  Error associate(WrapperFunctionMap M);

  /// Run a registered function.
  Expected<shared::WrapperFunctionResult>
  runWrapper(JITTargetAddress FunctionTag, ArrayRef<char> ArgBuffer);

private:
  std::mutex MapMutex;
  DenseMap<JITTargetAddress, std::shared_ptr<WrapperFunction>> TagToFunc;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_WRAPPERFUNCTIONMANAGER_H
