# Verify that ADDR32NB (IMAGE_REL_AMD64_ADDR32NB) relocations in .pdata produce
# correct 32-bit image-relative offsets. The test hand-crafts a .pdata entry
# with @IMGREL relocations targeting .text and .xdata symbols. After linking,
# jitlink-check verifies that each .pdata field equals target - __ImageBase.
#
# RUN: llvm-mc -filetype=obj -triple=x86_64-windows-msvc %s -o %t.o
# RUN: llvm-jitlink -noexec -abs __ImageBase=0xfff00000 \
# RUN:   -slab-allocate 100Kb -slab-address 0xfff00000 -slab-page-size 4096 \
# RUN:   -check %s %t.o
#
# jitlink-check: *{4}(my_func_pdata) = my_func - __ImageBase
# jitlink-check: *{4}(my_func_pdata + 4) = my_func_end - __ImageBase
# jitlink-check: *{4}(my_func_pdata + 8) = my_func_xdata - __ImageBase

	.text

	.def my_func;
	.scl 2;
	.type 32;
	.endef
	.globl my_func
	.p2align 4, 0x90
my_func:
	pushq %rbp
	movq %rsp, %rbp
	popq %rbp
	retq
	.globl my_func_end
my_func_end:

	.def main;
	.scl 2;
	.type 32;
	.endef
	.globl main
	.p2align 4, 0x90
main:
	retq

# .pdata entry: three 4-byte image-relative fields.
	.section .pdata,"dr"
	.p2align 2
	.globl my_func_pdata
my_func_pdata:
	.long my_func@IMGREL
	.long my_func_end@IMGREL
	.long my_func_xdata@IMGREL

# Minimal .xdata (UNWIND_INFO): version 1, no frame register, 1 unwind code.
	.section .xdata,"dr"
	.p2align 2
	.globl my_func_xdata
my_func_xdata:
	.byte 0x01           # Version=1, Flags=0
	.byte 0x04           # SizeOfProlog=4
	.byte 0x01           # CountOfUnwindCodes=1
	.byte 0x00           # FrameRegister=0, FrameOffset=0
	# Unwind code: offset 1, PUSH_NONVOL rbp
	.byte 0x01           # CodeOffset=1
	.byte 0x50           # UnwindOp=PUSH_NONVOL(0), OpInfo=RBP(5) -> 0x50
