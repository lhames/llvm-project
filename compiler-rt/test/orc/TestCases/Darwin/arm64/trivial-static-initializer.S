// Test that basic MachO static initializers work. The main function in this
// test returns the value of 'x', which is initially 1 in the data section,
// and reset to 0 if the _static_init function is run. If the static initializer
// does not run then main will return 1, causing the test to be treated as a
// failure.
//
// RUN: %clang -c -o %t %s
// RUN: %llvm_jitlink %t

	.section	__TEXT,__text,regular,pure_instructions
	.build_version macos, 11, 0

	.globl	_main
	.p2align	2
_main:
	stp	x29, x30, [sp, #-16]!
	adrp	x8, _x@PAGE
	ldr	w0, [x8, _x@PAGEOFF]
	ldp	x29, x30, [sp], #16
	ret

	.section	__TEXT,__StaticInit,regular,pure_instructions
	.p2align	2
_static_init:
	adrp	x8, _x@GOTPAGE
	ldr	x8, [x8, _x@GOTPAGEOFF]
	str	wzr, [x8]
	ret

	.section	__DATA,__data
	.globl	_x
	.p2align	2
_x:
	.long	1

 	.section	__DATA,__mod_init_func,mod_init_funcs
	.p2align	3
	.quad	_static_init

.subsections_via_symbols
