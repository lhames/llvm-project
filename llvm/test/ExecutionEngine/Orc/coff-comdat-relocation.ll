; Test that COMDAT section symbols are registered in the graph symbol table
; so that relocations in .pdata$<suffix> can reference them by index.
;
; COMDAT metadata sections (e.g. .xdata$*) may only have a section-definition
; symbol with no export following. Relocations from .pdata$<suffix> targeting
; these indices must still resolve.
;
; The test uses a COMDAT function with uwtable to force .pdata$<suffix>
; emission, then links with llvm-jitlink -noexec to verify all relocations
; resolve without error.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: llc -mtriple=x86_64-w64-windows-gnu -filetype=obj -o %t.obj %s
; RUN: llvm-jitlink -noexec -entry comdat_caller %t.obj

; A simple helper that the COMDAT function calls (making it non-leaf).
define i32 @helper() #0 {
  ret i32 7
}

; A COMDAT function — simulates a template instantiation or inline function.
; It calls helper, making it non-leaf so the compiler emits .pdata$comdat_fn
; and .xdata$comdat_fn with relocations referencing the COMDAT section symbol.
$comdat_fn = comdat any

define weak i32 @comdat_fn() #0 comdat {
  %val = call i32 @helper()
  ret i32 %val
}

; Entry point that calls the COMDAT function.
define i32 @comdat_caller() #0 {
  %val = call i32 @comdat_fn()
  ret i32 %val
}

attributes #0 = { nounwind uwtable }
