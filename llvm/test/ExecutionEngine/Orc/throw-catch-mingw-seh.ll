; Integration test: C++ throw/catch through JIT'd frames using Windows SEH
; on MinGW x86_64.
;
; This exercises the full COFF SEH pipeline: JITLink linker selection,
; __ImageBase resolution, PLT/ADDR32NB stubs, SEH registration plugin,
; DLLImport filtering, and .pdata keep-alive for COMDAT sections.
;
; The test throws an integer exception through two JIT'd frames (main →
; call_thrower → do_throw) and catches it in main. If any part of the
; pipeline is broken, the unwinder crashes or the link fails.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: lli -jit-kind=orc -dlopen libc++.dll -dlopen libunwind.dll %s

@_ZTIi = external constant ptr

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @__gxx_personality_seh0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

; Innermost frame: allocates and throws an integer exception.
define void @do_throw() #0 personality ptr @__gxx_personality_seh0 {
  %ex = call ptr @__cxa_allocate_exception(i64 4)
  store i32 42, ptr %ex
  call void @__cxa_throw(ptr %ex, ptr @_ZTIi, ptr null)
  unreachable
}

; Middle frame: calls do_throw. The unwinder must traverse this frame
; using its .pdata entry to reach main's catch handler.
define void @call_thrower() #0 personality ptr @__gxx_personality_seh0 {
  call void @do_throw()
  ret void
}

; Entry point: invokes call_thrower inside a try/catch. Verifies the
; exception propagates through two JIT'd frames and is caught correctly.
define i32 @main() #0 personality ptr @__gxx_personality_seh0 {
  invoke void @call_thrower()
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
  ; Return 0 on success (exception caught with correct value)
  %is42 = icmp eq i32 %val, 42
  %ret = select i1 %is42, i32 0, i32 2
  ret i32 %ret
}

attributes #0 = { uwtable }
