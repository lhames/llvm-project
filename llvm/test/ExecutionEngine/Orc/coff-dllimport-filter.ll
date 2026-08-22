; Test that COFF x86_64 JIT correctly handles both __imp_ symbol resolution
; and C++ exception handling in the same module.
;
; The test uses __imp_GetCurrentProcessId (an IAT-style DLL import) alongside
; a C++ throw/catch that requires the personality function
; (__gxx_personality_seh0) to be callable via an image-relative reference in
; .xdata. Both mechanisms must coexist without interfering.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: lli -jit-kind=orc -dlopen libc++.dll -dlopen libunwind.dll %s

@_ZTIi = external constant ptr
@__imp_GetCurrentProcessId = external global ptr

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @__gxx_personality_seh0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

; Non-leaf function that throws. The unwinder must find its .pdata entry
; to unwind past this frame.
define void @do_throw() #0 personality ptr @__gxx_personality_seh0 {
  %ex = call ptr @__cxa_allocate_exception(i64 4)
  store i32 77, ptr %ex
  call void @__cxa_throw(ptr %ex, ptr @_ZTIi, ptr null)
  unreachable
}

; Entry point: loads a DLL function pointer through an __imp_ symbol AND
; catches an exception. Both paths must work simultaneously.
define i32 @main() #0 personality ptr @__gxx_personality_seh0 {
  %fp = load ptr, ptr @__imp_GetCurrentProcessId
  %pid = call i32 %fp()
  %cmp = icmp eq i32 %pid, 0

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
