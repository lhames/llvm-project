; Integration test: C++ throw/catch through a no-frame-pointer function
; on MinGW x86_64.
;
; This tests that the Windows unwinder correctly processes UNWIND_INFO with
; ALLOC_SMALL unwind codes (used when there's no frame pointer and the
; function only needs a small stack allocation for spills/locals).
;
; Without a frame pointer, the unwinder relies entirely on .xdata unwind
; codes to restore RSP. If .pdata/.xdata are wrong or missing, the
; unwinder computes a garbage return address and crashes.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: lli -jit-kind=orc -dlopen libc++.dll -dlopen libunwind.dll %s

@_ZTIi = external constant ptr

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @__gxx_personality_seh0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

; A non-leaf function WITHOUT a frame pointer that throws.
; The compiler will emit ALLOC_SMALL unwind codes in .xdata to describe
; the stack adjustment. The unwinder must use these codes to restore RSP.
define void @no_fp_thrower() #0 personality ptr @__gxx_personality_seh0 {
  ; Use some local state to force a stack frame without frame pointer
  %buf = alloca [16 x i8], align 8
  store i8 1, ptr %buf
  %ex = call ptr @__cxa_allocate_exception(i64 4)
  store i32 77, ptr %ex
  call void @__cxa_throw(ptr %ex, ptr @_ZTIi, ptr null)
  unreachable
}

; Middle frame: also no frame pointer. Tests unwinding through multiple
; no-FP frames in sequence.
define void @no_fp_caller() #0 personality ptr @__gxx_personality_seh0 {
  %local = alloca i64, align 8
  store i64 123, ptr %local
  call void @no_fp_thrower()
  ret void
}

; Entry point: catches the exception after unwinding through two
; no-frame-pointer frames.
define i32 @main() #0 personality ptr @__gxx_personality_seh0 {
  invoke void @no_fp_caller()
          to label %unreachable unwind label %catch

unreachable:
  ret i32 1

catch:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTIi
  %exn = extractvalue { ptr, i32 } %lp, 0
  %caught = call ptr @__cxa_begin_catch(ptr %exn)
  %val = load i32, ptr %caught
  call void @__cxa_end_catch()
  ; Return 0 on success (exception value is 77)
  %is77 = icmp eq i32 %val, 77
  %ret = select i1 %is77, i32 0, i32 2
  ret i32 %ret
}

; uwtable but NO frame pointer — forces ALLOC_SMALL unwind codes
attributes #0 = { uwtable "frame-pointer"="none" }
