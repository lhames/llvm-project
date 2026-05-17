; Test that COFF x86_64 JITLink creates stubs for Pointer32NB edges targeting
; external symbols whose image-relative offset exceeds 32 bits.
;
; The .xdata section references the personality function (__gxx_personality_seh0)
; via an IMAGE_REL_AMD64_ADDR32NB relocation. When the personality function is
; in a DLL far from JIT'd code, its offset from __ImageBase doesn't fit in 32
; bits. JITLink creates an executable stub near the JIT'd code and redirects
; the .xdata reference to the stub, whose image-relative offset does fit.
;
; This test throws and catches a C++ exception, exercising the full path:
; .xdata personality reference → stub → indirect jump → real personality function.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: llc -mtriple=x86_64-w64-windows-gnu -filetype=obj -o %t.obj %s
; RUN: llvm-jitlink -entry entry -preload libc++.dll -preload libunwind.dll %t.obj

@_ZTIi = external constant ptr

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @__gxx_personality_seh0(...)
declare i32 @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

define i32 @thrower() #0 personality ptr @__gxx_personality_seh0 {
  %ex = call ptr @__cxa_allocate_exception(i64 4)
  store i32 42, ptr %ex
  call void @__cxa_throw(ptr %ex, ptr @_ZTIi, ptr null)
  unreachable
}

define i32 @entry() #0 personality ptr @__gxx_personality_seh0 {
  %val = invoke i32 @thrower()
          to label %normal unwind label %catch

normal:
  ret i32 1

catch:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTIi
  %exn = extractvalue { ptr, i32 } %lp, 0
  %sel = call i32 @__cxa_begin_catch(ptr %exn)
  call void @__cxa_end_catch()
  ret i32 0
}

attributes #0 = { nounwind uwtable }
