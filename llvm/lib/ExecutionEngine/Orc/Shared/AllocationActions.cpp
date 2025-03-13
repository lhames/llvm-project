//===----- AllocationActions.gpp -- JITLink allocation support calls  -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/Shared/AllocationActions.h"

using namespace llvm;
using namespace llvm::orc;
using namespace llvm::orc::shared;

namespace {

Error flattenErrorResult(WrapperFunctionResult R) {
  using namespace shared::detail;

  if (const char *ErrMsg = R.getOutOfBandError())
    return make_error<StringError>(ErrMsg, inconvertibleErrorCode());

  SPSSerializableError RErr;
  SPSInputBuffer IB(R.data(), R.size());
  if (!SPSArgList<SPSError>::deserialize(IB, RErr))
    return make_error<StringError>(
        "Could not deserialize allocation action result",
        inconvertibleErrorCode());

  if (RErr.HasError)
    return make_error<StringError>(std::move(RErr.ErrMsg),
                                   inconvertibleErrorCode());

  return Error::success();
}

class FinalizeActionsRunner {
public:
  static void run(AllocActions &&AAs,
                  OnRunFinalizeActionsCompleteFn &&OnComplete) {
    auto *Runner =
        new FinalizeActionsRunner(std::move(AAs), std::move(OnComplete));
    runNextFinalizeAction(static_cast<void *>(Runner), 0, Error::success());
  }

private:
  FinalizeActionsRunner(AllocActions &&AAs,
                        OnRunFinalizeActionsCompleteFn OnComplete)
      : AAs(std::move(AAs)), OnComplete(std::move(OnComplete)) {}

  static void runNextFinalizeAction(void *SessionCtx, uintptr_t MsgCtx,
                                    CWrapperFunctionResult R) {
    runNextFinalizeAction(SessionCtx, MsgCtx, flattenErrorResult(R));
  }

  static void runNextFinalizeAction(void *SessionCtx, uintptr_t MsgCtx,
                                    Error Err) {
    // If there's an error then run dealloc actions corresponding to previously
    // run actions.
    if (Err) {
      assert(MsgCtx != 0 && "Can't error before running the first action");
      return runNextDeallocAction(SessionCtx, MsgCtx, std::move(Err));
    }

    auto *This = static_cast<FinalizeActionsRunner *>(SessionCtx);

    // Skip any null finalize actions.
    while (LLVM_UNLIKELY(MsgCtx != This->AAs.size() &&
                         !This->AAs[MsgCtx].Finalize))
      ++MsgCtx;

    if (MsgCtx == This->AAs.size()) {
      // If we got here then there must not have been any error running the
      // finalize Actions
      cantFail(std::move(This->Err));

      auto OnComplete = std::move(This->OnComplete);
      std::vector<WrapperFunctionCall> DeallocActions;
      DeallocActions.reserve(This->AAs.size());
      for (auto &AA : reverse(This->AAs))
        if (AA.Dealloc) // Skip any null dealloc actions.
          DeallocActions.push_back(AA.Dealloc);
      delete This;
      return OnComplete(std::move(DeallocActions));
    }

    This->AAs[MsgCtx].Finalize.run(SessionCtx, MsgCtx + 1,
                                   runNextFinalizeAction);
  }

  static void runNextDeallocAction(void *SessionCtx, uintptr_t MsgCtx,
                                   CWrapperFunctionResult R) {
    runNextDeallocAction(SessionCtx, MsgCtx, flattenErrorResult(R));
  }

  static void runNextDeallocAction(void *SessionCtx, uintptr_t MsgCtx,
                                   Error Err) {
    auto *This = static_cast<FinalizeActionsRunner *>(SessionCtx);
    Err = joinErrors(std::move(This->Err), std::move(Err));

    // Skip any null dealloc actions.
    while (MsgCtx != 0 && !This->AAs[MsgCtx - 1].Dealloc)
      --MsgCtx;

    if (MsgCtx == 0) {
      auto OnComplete = std::move(This->OnComplete);
      delete This;
      return OnComplete(std::move(Err));
    }

    This->AAs[MsgCtx - 1].Dealloc.run(SessionCtx, MsgCtx - 1,
                                      runNextDeallocAction);
  }

  AllocActions AAs;
  OnRunFinalizeActionsCompleteFn OnComplete;
  Error Err = Error::success();
};

class DeallocActionsRunner {
public:
  static void run(std::vector<WrapperFunctionCall> &&DeallocActions,
                  OnRunDeallocActionsCompleteFn &&OnComplete) {
    auto *Runner = new DeallocActionsRunner(std::move(DeallocActions),
                                            std::move(OnComplete));
    runNextDeallocAction(static_cast<void *>(Runner), 0, Error::success());
  }

  static void runNextDeallocAction(void *SessionCtx, uintptr_t MsgCtx,
                                   CWrapperFunctionResult R) {
    runNextDeallocAction(SessionCtx, MsgCtx, flattenErrorResult(R));
  }

  static void runNextDeallocAction(void *SessionCtx, uintptr_t MsgCtx,
                                   Error Err) {
    auto *This = static_cast<DeallocActionsRunner *>(SessionCtx);
    Err = joinErrors(std::move(This->Err), std::move(Err));

    if (MsgCtx == This->DAs.size()) {
      auto OnComplete = std::move(This->OnComplete);
      delete This;
      return OnComplete(std::move(Err));
    }
    This->Err = std::move(Err);
    This->DAs[MsgCtx].run(SessionCtx, MsgCtx + 1, runNextDeallocAction);
  }

private:
  DeallocActionsRunner(std::vector<WrapperFunctionCall> &&DAs,
                       OnRunDeallocActionsCompleteFn OnComplete)
      : DAs(std::move(DAs)), OnComplete(std::move(OnComplete)) {}

  std::vector<WrapperFunctionCall> DAs;
  OnRunDeallocActionsCompleteFn OnComplete;
  Error Err = Error::success();
};

} // anonymous namespace

namespace llvm::orc::shared {

void runFinalizeActions(AllocActions &&AAs,
                        OnRunFinalizeActionsCompleteFn &&OnComplete) {
  FinalizeActionsRunner::run(std::move(AAs), std::move(OnComplete));
}

void runDeallocActions(std::vector<WrapperFunctionCall> &&DAs,
                       OnRunDeallocActionsCompleteFn &&OnComplete) {
  DeallocActionsRunner::run(std::move(DAs), std::move(OnComplete));
}

} // namespace llvm::orc::shared
