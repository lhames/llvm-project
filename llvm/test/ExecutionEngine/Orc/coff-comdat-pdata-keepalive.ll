; Test that COMDAT .pdata$<suffix> sections are kept alive during
; dead-stripping, ensuring the Windows unwinder can find unwind info
; for COMDAT functions (e.g., template instantiations).
;
; A COMDAT function produces .pdata$<suffix> and .xdata$<suffix> sections.
; SEHFrameKeepAlivePass must process all sections starting with ".pdata"
; (not just the exact ".pdata" section) so the pruner preserves them.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: lli -jit-kind=orc -dlopen libc++.dll -dlopen libunwind.dll %s

@_ZTIi = external constant ptr

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @__gxx_personality_seh0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()

; COMDAT function that throws — its .pdata$comdat_thrower must survive
; dead-stripping for the unwinder to traverse this frame.
$comdat_thrower = comdat any

define weak void @comdat_thrower() #0 comdat personality ptr @__gxx_personality_seh0 {
  %ex = call ptr @__cxa_allocate_exception(i64 4)
  store i32 55, ptr %ex
  call void @__cxa_throw(ptr %ex, ptr @_ZTIi, ptr null)
  unreachable
}

; Calls comdat_thrower inside a try/catch. Verifies the unwinder can
; unwind through the COMDAT frame using its preserved .pdata entry.
define i32 @main() #0 personality ptr @__gxx_personality_seh0 {
  invoke void @comdat_thrower()
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
