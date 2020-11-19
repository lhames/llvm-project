//===-------- stl_extras.h - Useful STL related functions-------*- C++ -*-===//
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

#ifndef ORC_RT_STL_EXTRAS_H
#define ORC_RT_STL_EXTRAS_H

#include <utility>

namespace __orc_rt {

namespace detail {

template <typename F, typename Tuple, std::size_t... I>
decltype(auto) apply_tuple_impl(F &&f, Tuple &&t, std::index_sequence<I...>) {
  return std::forward<F>(f)(std::get<I>(std::forward<Tuple>(t))...);
}

} // end namespace detail

/// Given an input tuple (a1, a2, ..., an), pass the arguments of the
/// tuple variadically to f as if by calling f(a1, a2, ..., an) and
/// return the result.
template <typename F, typename Tuple>
decltype(auto) apply_tuple(F &&f, Tuple &&t) {
  using Indices = std::make_index_sequence<
      std::tuple_size<typename std::decay<Tuple>::type>::value>;

  return detail::apply_tuple_impl(std::forward<F>(f), std::forward<Tuple>(t),
                                  Indices{});
}

/// A suitably aligned and sized character array member which can hold elements
/// of any type.
///
/// This template is equivalent to std::aligned_union_t<1, ...>, but we cannot
/// use it due to a bug in the MSVC x86 compiler:
/// https://github.com/microsoft/STL/issues/1533
/// Using `alignas` here works around the bug.
template <typename T, typename... Ts> struct AlignedCharArrayUnion {
  using AlignedUnion = std::aligned_union_t<1, T, Ts...>;
  alignas(alignof(AlignedUnion)) char buffer[sizeof(AlignedUnion)];
};

} // namespace __orc_rt

#endif // ORC_RT_STL_EXTRAS
