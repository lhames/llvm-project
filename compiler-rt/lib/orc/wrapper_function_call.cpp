//===-- wrapper_function_call.cpp -----------------------------------------===//
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

#include "wrapper_function_call.h"

using namespace orc_rt;

namespace {

class ErrorWFCSequenceRunner {
public:
  using OnCompleteFn = unique_function<void(Error)>;

  static void run(OnCompleteFn &&OnComplete,
                  std::vector<WrapperFunctionCall> &&Fns) {
    auto *R = new ErrorWFCSequenceRunner(std::move(Fns), std::move(OnComplete));
    runNextCall(static_cast<void *>(R), 0, Error::success());
  }

  static void runNextCall(void *SessionCtx, uintptr_t MsgCtx,
                          orc_rt_WrapperFunctionResult R) {
    runNextCall(SessionCtx, MsgCtx, flattenErrorResult(R));
  }

  static void runNextCall(void *SessionCtx, uintptr_t MsgCtx, Error Err) {
    auto *This = static_cast<ErrorWFCSequenceRunner *>(SessionCtx);

    if (Err || MsgCtx == This->DAs.size()) {
      auto OnComplete = std::move(This->OnComplete);
      delete This;
      return OnComplete(std::move(Err));
    }

    This->DAs[MsgCtx].run(SessionCtx, MsgCtx + 1, runNextCall);
  }

private:
  ErrorWFCSequenceRunner(std::vector<WrapperFunctionCall> &&DAs,
                         OnCompleteFn OnComplete)
      : DAs(std::move(DAs)), OnComplete(std::move(OnComplete)) {}

  static Error flattenErrorResult(WrapperFunctionResult R) {
    // TODO: Move this out into generic utility?
    if (const char *ErrMsg = R.getOutOfBandError())
      return make_error<StringError>(ErrMsg);
    detail::SPSSerializableError RErr;
    SPSInputBuffer IB(R.data(), R.size());
    if (!SPSArgList<SPSError>::deserialize(IB, RErr))
      return make_error<StringError>(
          "Could not deserialize error wrapper function call result");
    if (RErr.HasError)
      return make_error<StringError>(std::move(RErr.ErrMsg));
    return Error::success();
  }

  std::vector<WrapperFunctionCall> DAs;
  OnCompleteFn OnComplete;
};

} // anonymous namespace

namespace orc_rt {

void runErrorWFCSequence(unique_function<void(Error)> OnComplete,
                         std::vector<WrapperFunctionCall> WFCs) {
  ErrorWFCSequenceRunner::run(std::move(OnComplete), std::move(WFCs));
}

} // namespace orc_rt
