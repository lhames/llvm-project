; Test that __ImageBase resolves to the correct base address so that
; image-relative (ADDR32NB) relocations in .pdata/.xdata produce valid
; 32-bit offsets. Without the fix, __ImageBase is zero and the offsets
; overflow, causing a link failure.
;
; The test compiles a non-leaf function (one that calls another) with the
; uwtable attribute. Non-leaf functions require unwind info, so the compiler
; emits .pdata/.xdata with ADDR32NB relocations that reference __ImageBase.
;
; We use llvm-jitlink -noexec to test linking in isolation — verifying that
; __ImageBase resolves correctly and all ADDR32NB edges fit in 32 bits.
;
; REQUIRES: system-windows && host-unwind-supports-jit
; RUN: llc -mtriple=x86_64-w64-windows-gnu -filetype=obj -o %t.obj %s
; RUN: llvm-jitlink -noexec -entry entry %t.obj

define i32 @helper() #0 {
  ret i32 42
}

define i32 @entry() #0 {
  %val = call i32 @helper()
  ret i32 0
}

attributes #0 = { nounwind uwtable }
