//===-------- WrapperFunctionManager.cpp - Manage wrapper functions -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/WrapperFunctionManager.h"
#include "llvm/Support/FormatVariadic.h"

namespace llvm {
namespace orc {

Error WrapperFunctionManager::associate(WrapperFunctionMap M) {
  std::lock_guard<std::mutex> Lock(MapMutex);
  for (auto &KV : M) {
    if (TagToFunc.count(KV.first))
      return make_error<StringError>("Duplicate wrapper function tag " +
                                         formatv("{0:x16}", KV.first),
                                     inconvertibleErrorCode());
    TagToFunc[KV.first] =
        std::make_shared<WrapperFunction>(std::move(KV.second));
  }
  return Error::success();
}

Expected<shared::WrapperFunctionResult>
WrapperFunctionManager::runWrapper(JITTargetAddress FunctionTag,
                                   ArrayRef<char> ArgBuffer) {
  std::shared_ptr<WrapperFunction> F;
  {
    std::lock_guard<std::mutex> Lock(MapMutex);
    auto I = TagToFunc.find(FunctionTag);
    if (I == TagToFunc.end())
      return make_error<StringError>("Unrecognized function tag " +
                                         Twine(FunctionTag),
                                     inconvertibleErrorCode());
    F = I->second;
  }

  return (*F)(ArgBuffer);
}

} // end namespace orc
} // end namespace llvm
