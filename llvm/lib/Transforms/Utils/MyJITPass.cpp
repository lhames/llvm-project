//===---------------------------- MyJITPass.cpp ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/MyJITPass.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"

using namespace llvm;
using namespace llvm::orc;

PreservedAnalyses MyJITPass::run(Function &F,
				 FunctionAnalysisManager &AM) {
  auto J = LLJITBuilder().create();
  if (!J) {
    errs() << "MyJITPass could not create JIT instance for "
	   << F.getName() << ": " << toString(J.takeError()) << "\n";
    return PreservedAnalyses::all();
  }

  auto I32I32FnTy = FunctionType::get(Type::getInt32Ty(F.getContext()),
				      {Type::getInt32Ty(F.getContext())}, false);
  if (F.getFunctionType() != I32I32FnTy) {
    errs() << "MyJITPass: Function " << F.getName()
	   << " does not have required type. Skipping.\n";
    return PreservedAnalyses::all();
  } 
  
  auto FM = cloneToNewContext(*F.getParent(),
			      [&](const GlobalValue &GV) { return &GV == &F; });
  if (auto Err = (*J)->addIRModule(std::move(FM))) {
    errs() << "MyJITPass could not add extracted module for "
	   << F.getName() << ": " << toString(std::move(Err)) << "\n";
    return PreservedAnalyses::all();
  }    

  auto FSym = (*J)->lookup(F.getName());
  if (!FSym) {
    errs() << "MyJITPass could not get JIT'd symbol for "
	   << F.getName() << ": " << toString(FSym.takeError()) << "\n";
    return PreservedAnalyses::all();
  }

  auto *JittedF = FSym->toPtr<int32_t(int32_t)>();
  errs() << "Result: " << JittedF(42) << "\n";
  
  return PreservedAnalyses::all();
}
