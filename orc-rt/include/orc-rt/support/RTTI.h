//===------------- RTTI.h - RTTI support for ORC RT -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// \file
//
// Provides an extensible RTTI mechanism, that can be used regardless of whether
// the runtime is built with -frtti or not. This is predominantly used to
// support error handling.
//
// The RTTIRoot class defines methods for comparing type ids. Implementations
// of these methods can be injected into new classes using the RTTIExtends
// class template.
//
// E.g.
//
//   @code{.cpp}
//   class MyBaseClass : public RTTIExtends<MyBaseClass, RTTIRoot> {
//   public:
//     virtual void foo() = 0;
//   };
//
//   class MyDerivedClass1 : public RTTIExtends<MyDerivedClass1, MyBaseClass> {
//   public:
//     void foo() override {}
//   };
//
//   class MyDerivedClass2 : public RTTIExtends<MyDerivedClass2, MyBaseClass> {
//   public:
//     void foo() override {}
//   };
//
//   void fn() {
//     std::unique_ptr<MyBaseClass> B = std::make_unique<MyDerivedClass1>();
//     outs() << isa<MyBaseClass>(B) << "\n"; // Outputs "1".
//     outs() << isa<MyDerivedClass1>(B) << "\n"; // Outputs "1".
//     outs() << isa<MyDerivedClass2>(B) << "\n"; // Outputs "0'.
//   }
//
//   @endcode
//
// Note:
//   This header was adapted from llvm/Support/ExtensibleRTTI.h, however the
// data structures are not shared and the code need not be kept in sync.
//
//===----------------------------------------------------------------------===//

#ifndef ORC_RT_SUPPORT_RTTI_H
#define ORC_RT_SUPPORT_RTTI_H

#include <cstring>
#include <string_view>
#include <type_traits>

namespace orc_rt {

class ErrorInfoBase;

template <typename ThisT, typename ParentT> class RTTIExtends;

/// Base class for the extensible RTTI hierarchy.
///
/// This class defines virtual methods, dynamicClassID and isA, that enable
/// type comparisons.
class RTTIRoot {
public:
  virtual ~RTTIRoot() noexcept = default;

  /// Returns the class ID for this type.
  static constexpr const char *RTTIName = "orc_rt::RTTIRoot";

  /// Return the libray ID for this value.
  ///
  /// This identifies which dylib produced the value, allowing us to fast-path
  /// type equality checks within the same library.
  const void *libraryID() const noexcept { return LibraryID; }

  /// Returns the class ID for the dynamic type of this RTTIRoot instance.
  virtual const char *dynamicRTTIName() const noexcept = 0;

  /// Check whether this instance is a subclass of QueryT.
  template <typename QueryT> bool isA() const noexcept {
    return libraryID() == &ThisLibraryID ? sameDylibIsA(QueryT::RTTIName)
                                         : differentDylibIsA(QueryT::RTTIName);
  }

  static bool classof(const RTTIRoot *R) noexcept { return R->isA<RTTIRoot>(); }

protected:
  /// Fast-path isA for values produced by this dylib.
  virtual bool sameDylibIsA(const char *const ClassName) const noexcept {
    return ClassName == RTTIName;
  }

  /// Slow-path isA for values produced by different dylibs.
  virtual bool differentDylibIsA(const char *const ClassName) const noexcept {
    return strcmp(ClassName, RTTIName) == 0;
  }

private:
  static char ThisLibraryID;
  const char *const LibraryID = &ThisLibraryID;
  virtual void anchor() noexcept;
};

/// Inheritance utility for extensible RTTI.
///
/// Supports single inheritance only: A class can only have one
/// ExtensibleRTTI-parent (i.e. a parent for which the isa<> test will work),
/// though it can have many non-ExtensibleRTTI parents.
///
/// RTTIExtents uses CRTP so the first template argument to RTTIExtends is the
/// newly introduced type, and the *second* argument is the parent class.
///
/// class MyType : public RTTIExtends<MyType, RTTIRoot> {
/// public:
///   constexpr const char *RTTIName = "MyType";
///   ...
/// };
///
/// class MyDerivedType : public RTTIExtends<MyDerivedType, MyType> {
/// public:
///   constexpr const char *RTTIName = "MyDerivedType";
///   ...
/// };
///
template <typename ThisT, typename ParentT> class RTTIExtends : public ParentT {
public:
  static_assert(!std::is_base_of_v<ErrorInfoBase, ParentT>,
                "RTTIExtends should not be used to define orc_rt custom error "
                "types, use ErrorExtends instead");

  // Inherit constructors from ParentT.
  using ParentT::ParentT;

  const char *dynamicRTTIName() const noexcept override {
    static_assert(std::string_view(ThisT::RTTIName) !=
                      std::string_view(ParentT::RTTIName),
                  "ThisT must define its own RTTIName, distinct from "
                  "ParentT::RTTIName (did you forget to shadow it, or copy "
                  "the parent's string literal instead of writing a new "
                  "one?)");
    return ThisT::RTTIName;
  }

  static bool classof(const RTTIRoot *R) noexcept { return R->isA<ThisT>(); }

protected:
  bool sameDylibIsA(const char *const ClassName) const noexcept override {
    return ClassName == ThisT::RTTIName || ParentT::sameDylibIsA(ClassName);
  }

  bool differentDylibIsA(const char *const ClassName) const noexcept override {
    return strcmp(ClassName, ThisT::RTTIName) == 0 ||
           ParentT::differentDylibIsA(ClassName);
  }
};

/// Returns true if the given value is an instance of the template type
/// parameter.
template <typename To, typename From> bool isa(const From &Value) noexcept {
  return To::classof(&Value);
}

} // namespace orc_rt

#endif // ORC_RT_SUPPORT_RTTI_H
