#	$NetBSD$

# Call a user module's _start through the kernel's exported function
# table.  The host instantiates the module and places its _start in
# the given table slot (see wasm_loader.c); calling it from here --
# rather than through a host import -- keeps the whole call chain in
# wasm frames, which JSPI suspension requires (a JS frame between the
# promising export and a suspending import traps).

	.text
	.globl	wasm_user_start
	.type	wasm_user_start,@function
wasm_user_start:				# (i32 slot, i32 argp) -> i32
	.functype	wasm_user_start (i32, i32) -> (i32)
	local.get	1				# argp (the i32 argument)
	local.get	0				# table slot
	call_indirect	 __indirect_function_table, (i32) -> (i32)
	end_function
