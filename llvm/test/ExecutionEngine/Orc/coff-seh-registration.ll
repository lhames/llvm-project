; Test that C++ exceptions work in JIT'd code via lli (LLJIT).
; This verifies that LLJIT's platform setup registers .pdata with the
; Windows unwinder, enabling SEH-based stack unwinding through JIT'd frames.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: lli -jit-kind=orc -dlopen libc++.dll -dlopen libunwind.dll %s

@_ZTIi = external constant ptr

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @__gxx_personality_seh0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

; A non-leaf function that throws. The unwinder must find its .pdata entry
; to unwind past this frame.
define void @do_throw() #0 personality ptr @__gxx_personality_seh0 {
  %ex = call ptr @__cxa_allocate_exception(i64 4)
  store i32 99, ptr %ex
  call void @__cxa_throw(ptr %ex, ptr @_ZTIi, ptr null)
  unreachable
}

; Entry point: calls do_throw inside a try/catch. If the unwinder can find
; .pdata for both frames, the exception is caught and we return 0.
; If .pdata is not registered, the process crashes (non-zero exit).
define i32 @main() #0 personality ptr @__gxx_personality_seh0 {
  invoke void @do_throw()
          to label %unreachable unwind label %catch

unreachable:
  ret i32 1

catch:
  %lp = landingpad { ptr, i32 }
          catch ptr @_ZTIi
  %exn = extractvalue { ptr, i32 } %lp, 0
  %val = call ptr @__cxa_begin_catch(ptr %exn)
  call void @__cxa_end_catch()
  ret i32 0
}

attributes #0 = { uwtable }
