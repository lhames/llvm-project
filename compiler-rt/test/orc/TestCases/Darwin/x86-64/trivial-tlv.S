// Test that basic MachO TLVs work.
//
// RUN: %clang -c -o %t %s
// RUN: %llvm_jitlink %t

	.section	__TEXT,__text,regular,pure_instructions
	.build_version macos, 11, 0

	.globl	_main
	.p2align	4, 0x90
_main:
	pushq	%rbp
	movq	%rsp, %rbp
	movq	_x@TLVP(%rip), %rdi
	callq	*(%rdi)
	movl	(%rax), %eax
	popq	%rbp
	retq

	.section	__DATA,__thread_data,thread_local_regular
	.p2align	2
_x$tlv$init:
	.long	0

	.section	__DATA,__thread_vars,thread_local_variables
	.globl	_x
_x:
	.quad	__tlv_bootstrap
	.quad	0
	.quad	_x$tlv$init

.subsections_via_symbols
