	# extern int __stack_pointer;
	.globaltype __stack_pointer, i32

	# NOTE: Implement this in raw assembly to avoid stack pointer
	#       operations in C-prologue and epilogue.

	# void *asyncjmp_get_stack_pointer(void);
	.section	.text.asyncjmp_get_stack_pointer,"",@
	.globl	asyncjmp_get_stack_pointer
	.type	asyncjmp_get_stack_pointer,@function
asyncjmp_get_stack_pointer:
	.functype	asyncjmp_get_stack_pointer () -> (i32)
	global.get	__stack_pointer
	end_function

	# void asyncjmp_set_stack_pointer(void *sp);
	.section	.text.asyncjmp_set_stack_pointer,"",@
	.globl	asyncjmp_set_stack_pointer
	.type	asyncjmp_set_stack_pointer,@function
asyncjmp_set_stack_pointer:
	.functype	asyncjmp_set_stack_pointer (i32) -> ()
	local.get	0
	global.set	__stack_pointer
	end_function
