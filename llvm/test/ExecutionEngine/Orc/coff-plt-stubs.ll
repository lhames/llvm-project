; Test that COFF x86_64 JITLink creates PLT stubs for calls to external symbols.
;
; When JIT'd code calls an external DLL function (e.g., puts), the call uses a
; 32-bit PC-relative offset which cannot reach targets beyond ±2GB. JITLink
; generates a PLT stub (jmp [rip+0] + GOT entry) near the call site to bridge
; the gap via a 64-bit indirect jump.
;
; This test calls puts through a PLT stub and verifies it executes correctly.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: llc -mtriple=x86_64-w64-windows-gnu -filetype=obj -o %t.obj %s
; RUN: llvm-jitlink -entry entry %t.obj

@.str = private unnamed_addr constant [12 x i8] c"plt works!\0A\00"

declare i32 @puts(ptr)

define i32 @entry() #0 {
  %call = call i32 @puts(ptr @.str)
  ret i32 0
}

attributes #0 = { nounwind uwtable }
