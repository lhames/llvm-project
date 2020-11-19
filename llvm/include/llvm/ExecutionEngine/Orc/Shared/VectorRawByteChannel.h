//===-- VectorRawByteChannel.h - std::vector based byte-channel -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// std::vector based RawByteChannel.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_SHARED_VECTORRAWBYTECHANNEL_H
#define LLVM_EXECUTIONENGINE_ORC_SHARED_VECTORRAWBYTECHANNEL_H

#include "llvm/ExecutionEngine/Orc/Shared/RawByteChannel.h"
#include "llvm/Support/FormatVariadic.h"

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#else
#include <io.h>
#endif

namespace llvm {
namespace orc {
namespace shared {

/// Serialization channel that reads from and writes from a vector.
///
/// Note: This channel holds the whole history of the channel in the vector. It
/// is intended for serialization of types to/from byte vectors and for
/// debugging.
class VectorRawByteChannel final : public RawByteChannel {
public:
  VectorRawByteChannel() = default;

  VectorRawByteChannel(std::vector<uint8_t> InitialContent)
      : Content(std::move(InitialContent)) {}

  const std::vector<uint8_t> &getVector() const { return Content; }
  std::vector<uint8_t> takeVector() { return std::move(Content); }

  llvm::Error readBytes(char *Dst, unsigned Size) override {
    if (Size > Content.size() - ReadPosition)
      return make_error<StringError>(
          formatv("Invalid read of {0} bytes at position {1}: "
                  "only {2} bytes remain",
                  Size, ReadPosition, Content.size() - ReadPosition),
          inconvertibleErrorCode());
    while (Size--)
      *Dst++ = Content[ReadPosition++];
    return Error::success();
  }

  llvm::Error appendBytes(const char *Src, unsigned Size) override {
    while (Size--)
      Content.push_back(*Src++);
    return llvm::Error::success();
  }

  llvm::Error send() override { return llvm::Error::success(); }

private:
  std::vector<uint8_t> Content;
  std::vector<uint8_t>::size_type ReadPosition = 0;
};

} // namespace shared
} // namespace orc
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_SHARED_VECTORAWBYTECHANNEL_H
