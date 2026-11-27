#	$NetBSD$

# Call a user module's _start through the kernel's exported function
# table.  The host instantiates the module and places its _start in
# the given table slot (see wasm_loader.c); calling it from here --
# rather than through a host import -- keeps the whole call chain in
# wasm frames, which JSPI suspension requires (a JS frame between the
# promising export and a suspending import traps).

	.text
	.globaltype	__stack_pointer, i32
	.globl	wasm_user_start
	.type	wasm_user_start,@function
wasm_user_start:				# (i32 slot, i32 argp) -> i32
	.functype	wasm_user_start (i32, i32) -> (i32)
	local.get	1				# argp (the i32 argument)
	local.get	0				# table slot
	call_indirect	 __indirect_function_table, (i32) -> (i32)
	end_function

# Save/restore the stack pointer global through a caller-owned slot.
# A wasm_engine_throw() unwind does NOT run the epilogues of the
# unwound frames, so __stack_pointer is left wherever the deepest
# frame abandoned it; the code catching the throw must restore it
# before touching its own stack slots or calling anything.  The slot
# lives in kernel .data (address-absolute), never on the stack that
# the unwind invalidates.

	.globl	wasm_sp_save
	.type	wasm_sp_save,@function
wasm_sp_save:					# (i32 *slot) -> ()
	.functype	wasm_sp_save (i32) -> ()
	local.get	0
	global.get	__stack_pointer
	i32.store	0
	end_function

	.globl	wasm_sp_restore
	.type	wasm_sp_restore,@function
wasm_sp_restore:				# (i32 *slot) -> ()
	.functype	wasm_sp_restore (i32) -> ()
	local.get	0
	i32.load	0
	global.set	__stack_pointer
	end_function
