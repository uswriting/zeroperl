	# extern int _asyncjmp_setjmp_internal(asyncjmp_wasm_jmp_buf *env);
	.functype	_asyncjmp_setjmp_internal (i32) -> (i32)
	# extern int __stack_pointer;
	.globaltype 	__stack_pointer, i32

	# A wrapper of _asyncjmp_setjmp_internal to save and restore stack pointer
	# This cannot be implemented in C because there is no way to manipulate stack pointer
	# without C-epilogue.

	# extern int _asyncjmp_setjmp(asyncjmp_wasm_jmp_buf *env);
	.section	.text._asyncjmp_setjmp,"",@
	.globl		_asyncjmp_setjmp
	.type		_asyncjmp_setjmp,@function
_asyncjmp_setjmp:
	.functype	_asyncjmp_setjmp (i32) -> (i32)
	.local		i32, i32
	# save sp (this local is stored in asyncify stack and restored when rewinding)
	global.get	__stack_pointer
	local.set	1

	local.get	0
	call		_asyncjmp_setjmp_internal

	# restore sp
	local.get	1
	global.set	__stack_pointer
	end_function