/*	$NetBSD$ */

/*-
 * Copyright (c) 2026 The smolBSD Project
 * All rights reserved.
 *
 * This code is derived from software contributed to The smolBSD Project
 * by the smolBSD project.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE smolBSD PROJECT AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * wasm_loader: host-mediated loader for wasm user modules.
 *
 * The kernel cannot compile or instantiate wasm by itself: that is a
 * host (embedder) privilege.  The loop is:
 *
 *   kernel: wasm_host_load_module(bytes) -> host compiles + instantiates
 *           the module, wiring its env.svc import to our exported
 *           wasm_user_svc() directly (a wasm function import, no JS
 *           trampoline), wiring env.memory to the shared linear
 *           memory, and storing the module's _start in the next slot
 *           of our exported function table; returns that slot.
 *   kernel: wasm_user_start(slot, argp) (wasm_callin.s) -> call_indirect
 *           through the table straight into _start(argp); the return
 *           value is the program's exit code.  Keeping this chain in
 *           wasm frames is what lets a blocking svc suspend the epoch
 *           via JSPI (a JS frame in between would trap).
 *   user:   svc(nr, argp) -> wasm_user_svc() here, directly.
 *
 * Shared memory (B1): user modules import the kernel's own linear
 * memory, so pointers pass unmodified across every boundary.  Data
 * isolation (per-instance memory) is a later phase.  Modules are
 * linked with --global-base/--stack-first inside the user window
 * (see rootimg.h) so their data and stack avoid the kernel's own
 * image, stack, and the exec'd argv area; uvm page allocation does
 * not reach that deep in practice yet, but the real carve-out comes
 * with per-instance memory.
 *
 * The syscall ABI (v1) is the svc import below: NetBSD SYS_* numbers,
 * args as the matching sys_*_args struct in shared memory, straight
 * into the kernel's real sys_* implementations.  _start receives a
 * wasm_start_info block (argc/argv/envc/envp).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/atomic.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/kmem.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/syscall.h>
#include <sys/syscallargs.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include <sys/file.h>
#include <sys/filedesc.h>

#include <uvm/uvm_extern.h>

#include <arch/wasm/wasm/emul_i486.h>

#include <lib/libkern/libkern.h>

/*
 * Host loader service (imports).  The host keeps instance handles
 * until unload; see wasmrun/run.mjs.
 */
int32_t	wasm_host_load_module(uint32_t, uint32_t)
	    __attribute__((import_module("host"), import_name("load_module")));
void	wasm_host_unload_module(int32_t)
	    __attribute__((import_module("host"), import_name("unload_module")));

/*
 * JSPI blocking support.  The host calls wasm_set_jspi(1) before
 * boot when it can suspend wasm stacks (WebAssembly.promising +
 * Suspending).  A syscall that would block then calls wasm_host_wait
 * with a reason code (the Suspending callback receives it: the host
 * must not short-circuit a child wait just because console input is
 * queued) and retries.  Without the flag, EWOULDBLOCK is returned to
 * the guest as before.
 */
#define	WASM_WAIT_INPUT	0	/* retry once console input exists */
#define	WASM_WAIT_CHILD	1	/* retry once a child changed state */

void	wasm_host_wait(int32_t)
	    __attribute__((import_module("host"), import_name("wait")));

static int wasm_jspi;

int	wasm_set_jspi(int);
int
wasm_set_jspi(int v)
{
	wasm_jspi = v;
	return 0;
}

/*
 * E5 JIT tier: host-assisted code generation.  The kernel emits wasm
 * module bytes into ordinary memory and asks the host to compile and
 * instantiate them with the shared linear memory imported, so compiled
 * guest code reads and writes guest memory directly.  jit_compile
 * returns a module handle (-1 on failure); jit_call invokes export
 * number fnidx with one i32 argument.  See wasmrun/run.mjs.
 */
int32_t	wasm_host_jit_compile(uint32_t, uint32_t)
	    __attribute__((import_module("host"), import_name("jit_compile")));
int32_t	wasm_host_jit_call(int32_t, int32_t, int32_t)
	    __attribute__((import_module("host"), import_name("jit_call")));

/*
 * E5a self-test: hand-assembled module exporting f(p) = *(int32 *)p + 4.
 * Proves the emit -> compile -> call round trip over shared memory.
 * Run once from the first exec_wasm32 (console is up by then).
 */
static const uint8_t jit_spike_mod[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
	0x02, 0x0f, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x06,
	0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00,
	0x00, 0x03, 0x02, 0x01, 0x00, 0x07, 0x05, 0x01,
	0x01, 0x66, 0x00, 0x00, 0x0a, 0x0c, 0x01, 0x0a,
	0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x41, 0x04,
	0x6a, 0x0b
};

int	wasm_jit_selftest(void);
int
wasm_jit_selftest(void)
{
	static uint32_t jit_spike_buf;
	int32_t h, r;

	jit_spike_buf = 38;
	h = wasm_host_jit_compile((uint32_t)(uintptr_t)jit_spike_mod,
	    sizeof(jit_spike_mod));
	if (h < 0) {
		printf("[jit] selftest: compile unavailable\n");
		return 0;
	}
	r = wasm_host_jit_call(h, 0, (uint32_t)(uintptr_t)&jit_spike_buf);
	printf("[jit] selftest: f(38) = %d %s\n", r, r == 42 ? "OK" : "FAIL");
	return 0;
}

/* wasm_callin.s: call_indirect into the user module's _start */
int32_t	wasm_user_start(int32_t, uint32_t);

/* wasm_callin.s: stack-pointer save/restore around a throw unwind */
void	wasm_sp_save(int *);
void	wasm_sp_restore(int *);

int	wasm_user_svc(int, int);
int	wasm_loader_load(struct vnode *, size_t, const char *);
void	wasm_loader_run(void);
int	wasm_child_spawn(const char *, char *const *);

/*
 * Startup info handed to _start as its single argument (wasm exports
 * take one i32): the classic argc/argv/envc/envp, with pointers as
 * u32 words into the shared linear memory.
 */
struct wasm_start_info {
	int32_t		argc;
	uint32_t	argv;	/* char *[argc + 1], NULL-terminated */
	int32_t		envc;
	uint32_t	envp;	/* char *[envc + 1], NULL-terminated */
};

static struct wasm_start_info wasm_si;
static char *wasm_argv[2];
static char wasm_e0[] = "PATH=/rescue:/bin:/sbin:/usr/bin:/usr/sbin";
static char wasm_e1[] = "HOME=/root";
static char wasm_e2[] = "SHELL=/rescue/sh";
static char wasm_e3[] = "TERM=dumb";
static char *wasm_envp[] = { wasm_e0, wasm_e1, wasm_e2, wasm_e3, NULL };

int	wasm_read_file(const char *, void **, size_t *);
int	wasm_i486_fork(struct i486_vm *);
static char wasm_arg0[128];

/*
 * Real-module support (used by exec_wasm32 for files that carry the
 * wasm magic rather than a WASMUSER stub).
 *
 * Loading happens at makecmds time while the vnode is still open;
 * the resulting handle is parked until the epoch enters the program.
 * One pending module at a time: this system execs init once and
 * never again, so a global is honest v1 scope.
 */
#define	WASM_MODULE_MAXSIZE	(16 * 1024 * 1024)

static int32_t wasm_pending_slot = -1;

int
wasm_loader_load(struct vnode *vp, size_t size, const char *kname)
{
	uint8_t *buf;
	int32_t slot;
	int error;

	if (size == 0 || size > WASM_MODULE_MAXSIZE)
		return EFTYPE;

	buf = kmem_alloc(size, KM_SLEEP);
	error = vn_rdwr(UIO_READ, vp, buf, size, (off_t)0, UIO_SYSSPACE,
	    IO_UNIT, kauth_cred_get(), NULL, NULL);
	if (error != 0) {
		kmem_free(buf, size);
		return error;
	}

	slot = wasm_host_load_module((uint32_t)(uintptr_t)buf,
	    (uint32_t)size);
	kmem_free(buf, size);
	if (slot < 0)
		return EFTYPE;

	strlcpy(wasm_arg0, kname, sizeof(wasm_arg0));
	wasm_argv[0] = wasm_arg0;
	wasm_argv[1] = NULL;
	wasm_si.argc = 1;
	wasm_si.argv = (uint32_t)(uintptr_t)wasm_argv;
	wasm_si.envc = 0;
	wasm_si.envp = (uint32_t)(uintptr_t)wasm_envp;

	wasm_pending_slot = slot;
	return 0;
}

/*
 * Epoch entry for a real module (ep_entry round-trip; see
 * wasm_user_entry).  _start receives the startup info block.
 */
void
wasm_loader_run(void)
{
	int32_t slot, rv;

	if (wasm_pending_slot < 0) {
		printf("wasm: no module pending\n");
		return;
	}
	slot = wasm_pending_slot;
	wasm_pending_slot = -1;

	rv = wasm_user_start(slot, (uint32_t)(uintptr_t)&wasm_si);
	printf("wasm: module exited with %d\n", rv);
	wasm_host_unload_module(slot);
}

/*
 * Multi-process support (M1): host-instantiated child processes.
 *
 * init spawns children with the WASM_SYS_spawn svc call: the kernel
 * reads the module from the root FFS, has the host instantiate it
 * (shared memory, shared svc import, _start in our function table),
 * and wraps it in a real struct proc with its own fd table (the
 * fork1 recipe minus lwp/signal machinery: children run on init's
 * lwp with l_proc and l_fd swapped for the duration of a slice).
 *
 * Scheduling is host-driven and cooperative: a child runs only when
 * the epoch is already suspended -- wait6 suspends via JSPI, and the
 * host then calls wasm_run_child (promising), so a child that blocks
 * on input suspends in turn and resumes when input arrives.  A child
 * runs to completion: return from _start or SYS_exit (the latter
 * unwinds through wasm_engine_guarded, the same mechanism reboot
 * uses).  No preemption.
 *
 * The child's frames live on the kernel's own wasm stack below the
 * suspended init frames: safe while at most one child runs at a
 * time, which the host scheduler guarantees.  Per-process stacks
 * (sp_run) come with multi-suspend scheduling.
 */
#define	WASM_CHILD_MAX	8
#define	WASM_SYS_spawn	512	/* svc extension: { path, argv } */
#define	WASM_SPAWN_ARGC_MAX	16

struct wasm_spawn_args {
	uint32_t	path;
	uint32_t	argv;	/* NULL-terminated char *[], may be 0 */
};

struct wasm_child {
	struct proc	*c_proc;	/* NULL: slot free */
	pid_t		c_parent;	/* p_pid of the spawning process */
	int32_t		c_slot;		/* host function-table slot */
	int	c_started;
	int	c_forked;	/* fd table inherited, no console seed */
	int		c_running;
	int		c_exited;
	int		c_exitcode;
	int		c_rv;		/* _start return value */
	void		*c_i486;	/* i486 guest task, NULL for wasm */
	struct wasm_start_info c_si;
	char		c_arg0[128];
	char		*c_argv[WASM_SPAWN_ARGC_MAX + 1];
};

static struct wasm_child wasm_children[WASM_CHILD_MAX];
static int wasm_current_child = -1;	/* running child index, -1 = init */
static int wasm_slice_sp;		/* SP across a throw unwind */

extern int wasm_engine_guarded(void (*)(void *), void *);
extern void wasm_engine_throw(void);
int	sys_getpid_with_ppid(struct lwp *, const void *, register_t *);
int	sys_getuid(struct lwp *, const void *, register_t *);
int	sys_getgid(struct lwp *, const void *, register_t *);
void	*wasm_i486_load(const void *, size_t, char *const *, char *const *);
int	wasm_i486_exec(void *);
void	wasm_i486_free(void *);
void	wasm_i486_snapshot(struct i486_vm *, struct i486_vm *);
void	*wasm_i486_task_alloc(void);

/*
 * wasm_proc_fork: the fork1 recipe without lwp, signals, or address
 * space (B1 shares init's vmspace outright); used for both wasm and
 * i486 children.  Returns the new proc, ready for fd/cwd defaults,
 * or NULL.
 */
static struct proc *
wasm_proc_fork(struct proc *p1, int copyfds)
{
	struct proc *p2 = proc_alloc();

	if (p2 == NULL)
		return NULL;
	memset(&p2->p_startzero, 0,
	    (unsigned)((char *)&p2->p_endzero - (char *)&p2->p_startzero));
	memcpy(&p2->p_startcopy, &p1->p_startcopy,
	    (unsigned)((char *)&p2->p_endcopy - (char *)&p2->p_startcopy));
	p2->p_stackbase = p1->p_stackbase;
	TAILQ_INIT(&p2->p_sigpend.sp_info);
	LIST_INIT(&p2->p_lwps);
	LIST_INIT(&p2->p_sigwaiters);
	p2->p_flag = p1->p_flag & PK_32;
	p2->p_emul = p1->p_emul;
	p2->p_execsw = p1->p_execsw;
	mutex_init(&p2->p_stmutex, MUTEX_DEFAULT, IPL_HIGH);
	mutex_init(&p2->p_auxlock, MUTEX_DEFAULT, IPL_NONE);
	rw_init(&p2->p_reflock);
	cv_init(&p2->p_waitcv, "wasmwait");
	cv_init(&p2->p_lwpcv, "wasmlwp");
	p2->p_lock = mutex_obj_alloc(MUTEX_DEFAULT, IPL_NONE);
	kauth_proc_fork(p1, p2);
	p2->p_raslist = NULL;
	p2->p_textvp = NULL;
	p2->p_path = NULL;
	p2->p_fd = copyfds ? fd_copy() : fd_init(NULL);
	p2->p_cwdi = cwdinit();
	lim_addref(p1->p_limit);
	p2->p_limit = p1->p_limit;
	p2->p_sflag = 0;
	p2->p_slflag = 0;
	p2->p_lflag = 0;
	p2->p_pptr = p1;
	p2->p_ppid = p1->p_pid;
	LIST_INIT(&p2->p_children);
	p2->p_aio = NULL;
	p2->p_mqueue_cnt = 0;
	p2->p_sigacts = sigactsinit(p1, 0);
	p2->p_stats = pstatscopy(p1->p_stats);
	p2->p_vmspace = p1->p_vmspace;
	p2->p_vmspace->vm_refcnt++;
	p2->p_md = p1->p_md;
	p2->p_stat = SACTIVE;
	mutex_enter(&proc_lock);
	LIST_INSERT_HEAD(&allproc, p2, p_list);
	atomic_inc_uint(&nprocs);
	mutex_exit(&proc_lock);
	return p2;
}

/*
 * wasm_child_spawn: read a wasm module by path and instantiate it as
 * a new process.  Returns the child's pid or a negative errno.
 */
int
wasm_child_spawn(const char *path, char *const *uargv)
{
	struct wasm_child *c;
	struct proc *p1 = curlwp->l_proc, *p2;
	struct nameidata nd;
	struct pathbuf *pb;
	struct vattr va;
	struct vnode *vp;
	uint8_t *buf;
	int32_t slot;
	size_t size;
	int i, error;

	c = NULL;
	for (i = 0; i < WASM_CHILD_MAX; i++)
		if (wasm_children[i].c_proc == NULL) {
			c = &wasm_children[i];
			break;
		}
	if (c == NULL)
		return -EAGAIN;

	/*
	 * Read the whole module into kernel memory (same bound as
	 * exec); the host copies it out of linear memory itself.
	 */
	pb = pathbuf_create(path);
	if (pb == NULL)
		return -ENOMEM;
	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, pb);
	error = namei(&nd);
	pathbuf_destroy(pb);
	if (error != 0)
		return -error;
	vp = nd.ni_vp;

	error = VOP_GETATTR(vp, &va, kauth_cred_get());
	if (error != 0 || va.va_size == 0 ||
	    va.va_size > WASM_MODULE_MAXSIZE) {
		error = error != 0 ? error : EFTYPE;
		goto out;
	}
	size = (size_t)va.va_size;
	printf("wasm: spawn %s size=%zu\n", path, size);

	error = VOP_OPEN(vp, FREAD, kauth_cred_get());
	if (error != 0)
		goto out;
	printf("wasm: spawn opened\n");
	buf = kmem_alloc(size, KM_SLEEP);
	printf("wasm: spawn buf at %p\n", (void *)buf);
	printf("wasm: spawn free=%lu wire=%lu\n", (u_long)uvmexp.free,
	    (u_long)uvmexp.wired);
	{
		extern u_long bufmem_hiwater, bufmem_lowater, bufmem;
		printf("wasm: bufmem hi=%lu low=%lu used=%lu\n",
		    (u_long)bufmem_hiwater, (u_long)bufmem_lowater,
		    (u_long)bufmem);
	}
	{
		size_t done = 0;
		while (done < size) {
			size_t n = size - done > (1u << 20) ? (1u << 20)
			    : size - done;
			size_t resid = 0;
			error = vn_rdwr(UIO_READ, vp, buf + done, n,
			    (off_t)done, UIO_SYSSPACE, IO_NODELOCKED,
			    kauth_cred_get(), &resid, NULL);
			if (error != 0)
				break;
			if (resid != 0)
				printf("wasm: spawn SHORT read at %zu "
				    "n=%zu resid=%zu\n", done, n, resid);
			done += n - resid;
		}
	}
	VOP_CLOSE(vp, FREAD, kauth_cred_get());
	if (error != 0) {
		kmem_free(buf, size);
		goto out;
	}
	printf("wasm: spawn read done\n");

	/*
	 * Startup info: the caller's argv array lives in shared memory
	 * and the parent is frozen while the child runs, but copy the
	 * pointers (bounded) so the child never dereferences a stale
	 * user array; the strings themselves pass by reference.
	 */
	if (uargv != NULL) {
		int n;
		for (n = 0; n < WASM_SPAWN_ARGC_MAX && uargv[n] != NULL; n++)
			c->c_argv[n] = uargv[n];
		c->c_argv[n] = NULL;
		c->c_si.argc = n;
	} else {
		strlcpy(c->c_arg0, path, sizeof(c->c_arg0));
		c->c_argv[0] = c->c_arg0;
		c->c_argv[1] = NULL;
		c->c_si.argc = 1;
	}
	c->c_si.argv = (uint32_t)(uintptr_t)c->c_argv;
	c->c_si.envc = 0;
	c->c_si.envp = (uint32_t)(uintptr_t)wasm_envp;

	/*
	 * ELF32 EM_386: run under the in-kernel i486 interpreter;
	 * anything else goes to the host wasm module loader.
	 */
	if (size >= 20 && memcmp(buf, "\177ELF", 4) == 0 && buf[4] == 1 &&
	    (buf[18] | (buf[19] << 8)) == 3) {
		printf("wasm: spawn i486 load begin\n");
		c->c_i486 = wasm_i486_load(buf, size, c->c_argv, wasm_envp);
		printf("wasm: spawn i486 load %s\n",
		    c->c_i486 != NULL ? "ok" : "FAILED");
		kmem_free(buf, size);
		if (c->c_i486 == NULL) {
			error = EFTYPE;
			goto out;
		}
		slot = -1;
	} else {
		slot = wasm_host_load_module((uint32_t)(uintptr_t)buf,
		    (uint32_t)size);
		kmem_free(buf, size);
		if (slot < 0) {
			error = EFTYPE;
			goto out;
		}
		c->c_i486 = NULL;
	}

	/*
	 * Child proc: the fork1 sequence without lwp, signals, or
	 * address space (B1 shares init's vmspace outright).
	 */
	p2 = wasm_proc_fork(p1, 0);
	if (p2 == NULL) {
		if (slot >= 0)
			wasm_host_unload_module(slot);
		else
			wasm_i486_free(c->c_i486);
		c->c_i486 = NULL;
		error = EAGAIN;
		goto out;
	}
	p2->p_path = kmem_strdupsize(path, NULL, KM_SLEEP);

	c->c_proc = p2;
	c->c_parent = p1->p_pid;
	c->c_slot = slot;
	c->c_started = 0;
	c->c_running = 0;
	c->c_exited = 0;
	c->c_exitcode = 0;
	c->c_rv = 0;

	vput(vp);
	printf("wasm: spawned %s as pid %d (slot %d)\n", path,
	    (int)p2->p_pid, slot);
	return (int)p2->p_pid;

out:
	vput(vp);
	return -error;
}

/*
 * wasm_read_file: read a regular file by path into kernel memory.
 * Used by the i486 execve bridge.  Returns 0 with bufp and sizep set.
 */
int
wasm_read_file(const char *path, void **bufp, size_t *sizep)
{
	struct nameidata nd;
	struct pathbuf *pb;
	struct vattr va;
	struct vnode *vp;
	uint8_t *buf;
	size_t size;
	int error;

	pb = pathbuf_create(path);
	if (pb == NULL)
		return ENOMEM;
	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, pb);
	error = namei(&nd);
	pathbuf_destroy(pb);
	if (error != 0)
		return error;
	vp = nd.ni_vp;

	error = VOP_GETATTR(vp, &va, kauth_cred_get());
	if (error != 0 || va.va_size == 0 ||
	    va.va_size > WASM_MODULE_MAXSIZE) {
		if (error == 0)
			error = EFTYPE;
		goto out;
	}
	size = (size_t)va.va_size;

	error = VOP_OPEN(vp, FREAD, kauth_cred_get());
	if (error != 0)
		goto out;
	buf = kmem_alloc(size, KM_SLEEP);
	{
		size_t done = 0;
		while (done < size) {
			size_t n = size - done > (1u << 20) ? (1u << 20)
			    : size - done;
			size_t resid = 0;
			error = vn_rdwr(UIO_READ, vp, buf + done, n,
			    (off_t)done, UIO_SYSSPACE, IO_NODELOCKED,
			    kauth_cred_get(), &resid, NULL);
			if (error != 0)
				break;
			if (resid != 0) {
				printf("wasm: rfile SHORT at %zu resid=%zu\n",
				    done, resid);
				break;
			}
			done += n;
		}
	}
	VOP_CLOSE(vp, FREAD, kauth_cred_get());
	if (error != 0) {
		kmem_free(buf, size);
		goto out;
	}
	*bufp = buf;
	*sizep = size;
out:
	vput(vp);
	return error;
}

/*
 * wasm_i486_fork: i486 guest fork(2).  Snapshots the calling task
 * into a new child (region copy, EAX=0/EDX=1 per fork.S), gives it a
 * copy of the parent's fd table, and queues it as pending: the host
 * scheduler runs it when the epoch next suspends (typically the
 * parent's wait4).  Returns the child pid to the parent.
 */
int
wasm_i486_fork(struct i486_vm *vm)
{
	struct wasm_child *c;
	struct proc *p1 = curlwp->l_proc, *p2;
	void *t2;
	int i;

	c = NULL;
	for (i = 0; i < WASM_CHILD_MAX; i++)
		if (wasm_children[i].c_proc == NULL) {
			c = &wasm_children[i];
			break;
		}
	if (c == NULL)
		return -EAGAIN;

	t2 = wasm_i486_task_alloc();
	if (t2 == NULL)
		return -ENOMEM;
	p2 = wasm_proc_fork(p1, 1);
	if (p2 == NULL) {
		wasm_i486_free(t2);
		return -EAGAIN;
	}
	if (p1->p_path != NULL)
		p2->p_path = kmem_strdupsize(p1->p_path, NULL, KM_SLEEP);

	wasm_i486_snapshot(vm, (struct i486_vm *)t2);
	c->c_proc = p2;
	c->c_parent = p1->p_pid;
	c->c_slot = -1;
	c->c_i486 = t2;
	c->c_forked = 1;
	c->c_started = 0;
	c->c_running = 0;
	c->c_exited = 0;
	c->c_exitcode = 0;
	c->c_rv = 0;
	return (int)p2->p_pid;
}

/*
 * wasm_child_reap: undo wasm_child_spawn's allocations once the fd
 * table has already been freed at the end of the child's last slice
 * (fd_free runs in child context, like exit1).  Called from init's
 * wait6 with the child exited.
 */
static void
wasm_child_reap(struct wasm_child *c)
{
	struct proc *p = c->c_proc;

	if (c->c_i486 != NULL)
		wasm_i486_free(c->c_i486);
	else
		wasm_host_unload_module(c->c_slot);
	cwdfree(p->p_cwdi);
	lim_free(p->p_limit);
	pstatsfree(p->p_stats);
	sigactsfree(p->p_sigacts);
	kauth_cred_free(p->p_cred);
	kmem_strfree(p->p_path);
	uvmspace_free(p->p_vmspace);
	mutex_enter(&proc_lock);
	LIST_REMOVE(p, p_list);
	atomic_dec_uint(&nprocs);
	mutex_exit(&proc_lock);
	mutex_destroy(&p->p_stmutex);
	mutex_destroy(&p->p_auxlock);
	rw_destroy(&p->p_reflock);
	cv_destroy(&p->p_waitcv);
	cv_destroy(&p->p_lwpcv);
	mutex_obj_free(p->p_lock);
	proc_free_pid(p->p_pid);
	proc_free_mem(p);
	memset(c, 0, sizeof(*c));
}

static void
wasm_child_trampoline(void *arg)
{
	struct wasm_child *c = arg;

	if (c->c_i486 != NULL) {
		register_t rv;
		struct sys_open_args oa;
		int i;

		SCARG(&oa, path) = (long)(uintptr_t)"/dev/console";
		SCARG(&oa, flags) = O_RDWR;
		SCARG(&oa, mode) = 0;

		/* init-style console seeding: real i386 binaries
		 * expect fd 0/1/2 open; child fd table starts empty
		 * (forked children inherit the parent's table) */
		if (!c->c_forked) {
			for (i = 0; i < 3; i++)
				(void)sys_open(curlwp, &oa, &rv);
		}
		c->c_rv = wasm_i486_exec(c->c_i486);
		return;
	}
	c->c_rv = wasm_user_start(c->c_slot,
	    (uint32_t)(uintptr_t)&c->c_si);
}

/*
 * wasm_run_child: run one child slice (exported; the host calls it,
 * promising-wrapped, only while the epoch is suspended).  Swaps
 * curlwp's proc/fd for the duration, runs the module under a
 * protected region, then frees the fd table in child context.  A
 * wasm_engine_throw (SYS_exit) unwinds back to the guarded frame.
 */
int	wasm_run_child(int32_t);
int
wasm_run_child(int32_t idx)
{
	struct wasm_child *c;
	struct proc *oldp;
	filedesc_t *oldfd;
	int g, oldchild, oldsp;

	if (idx < 0 || idx >= WASM_CHILD_MAX)
		return EINVAL;
	c = &wasm_children[idx];
	if (c->c_proc == NULL || c->c_exited || c->c_running)
		return EINVAL;

	oldp = curlwp->l_proc;
	oldfd = curlwp->l_fd;
	/* nested slices (a forked child of a running child) must
	 * restore the outer slice's context, not clear it */
	oldchild = wasm_current_child;
	oldsp = wasm_slice_sp;
	c->c_started = 1;
	c->c_running = 1;
	wasm_current_child = idx;
	curlwp->l_proc = c->c_proc;
	curlwp->l_fd = c->c_proc->p_fd;

	wasm_sp_save(&wasm_slice_sp);
	g = wasm_engine_guarded(wasm_child_trampoline, c);
	if (g != 0)
		wasm_sp_restore(&wasm_slice_sp);
	wasm_slice_sp = oldsp;

	/* exit1-style fd teardown, still in child context */
	fd_free();
	c->c_proc->p_fd = NULL;
	curlwp->l_fd = oldfd;
	curlwp->l_proc = oldp;
	wasm_current_child = oldchild;
	c->c_running = 0;
	c->c_exited = 1;
	if (g == 0)
		c->c_exitcode = c->c_rv;
	c->c_proc->p_stat = SZOMB;
	printf("wasm: pid %d exited with %d%s\n", (int)c->c_proc->p_pid,
	    c->c_exitcode, g ? " (exit)" : "");
	return 0;
}

/*
 * wasm_take_pending: index of a spawned child that has never run,
 * or -1.  The host calls this when an epoch suspends to decide
 * whether to run a child instead of waiting for console input.
 */
int	wasm_take_pending(void);
int
wasm_take_pending(void)
{
	int i;

	for (i = 0; i < WASM_CHILD_MAX; i++)
		if (wasm_children[i].c_proc != NULL && !wasm_children[i].c_started)
			return i;
	return -1;
}

/*
 * wasm_child_wait: shared wait4/wait6 service over the child table.
 */
static int
wasm_child_wait(idtype_t idtype, id_t id, int *status, register_t *rvp)
{
	struct wasm_child *c;

	if (idtype != P_ALL && idtype != P_PID)
		return EOPNOTSUPP;
	for (;;) {
		int live = 0, got = -1;
		for (int i = 0; i < WASM_CHILD_MAX; i++) {
			c = &wasm_children[i];
			if (c->c_proc == NULL)
				continue;
			/* only this caller's own children are waitable */
			if (c->c_parent != curlwp->l_proc->p_pid)
				continue;
			if (idtype == P_PID &&
			    (pid_t)id != c->c_proc->p_pid)
				continue;
			if (!c->c_exited) {
				live++;
				continue;
			}
			got = i;
			break;
		}
		if (got >= 0) {
			c = &wasm_children[got];
			if (status != NULL)
				*status = (int)W_EXITCODE(
				    c->c_exitcode & 0377, 0);
			*rvp = c->c_proc->p_pid;
			wasm_child_reap(c);
			return 0;
		}
		if (live == 0)
			return ECHILD;
		/*
		 * A child of ours that has never run: execute it as a
		 * nested slice right here.  The caller may itself be a
		 * running child slice (a shell waiting on pipeline
		 * children), and the host scheduler only starts
		 * top-level slices; nesting wasm_run_child keeps the
		 * whole family on one wasm stack, where a JSPI suspend
		 * inside the child unwinds (and resumes) through the
		 * parent's frames intact.
		 */
		for (int i = 0; i < WASM_CHILD_MAX; i++) {
			c = &wasm_children[i];
			if (c->c_proc == NULL || c->c_started)
				continue;
			if (c->c_parent != curlwp->l_proc->p_pid)
				continue;
			wasm_run_child(i);
			live = 0;	/* re-scan: it exited or ran */
			break;
		}
		if (live == 0)
			continue;
		if (wasm_jspi == 0)
			return EWOULDBLOCK;
		wasm_host_wait(WASM_WAIT_CHILD);
	}
}

/*
 * svc: syscall entry for user modules (ABI v1).  The host wires this
 * as every user module's env.svc import.  Numbers are the real NetBSD
 * SYS_* numbers; argp points to the matching sys_*_args struct in the
 * shared linear memory (wasm32 words match syscallarg_t exactly).
 * The call goes straight into the kernel's sys_* implementation --
 * the same function a trapframe dispatch would reach -- so user
 * programs share the real VFS, fd table, and credentials.
 *
 * Deviations: SYS_exit from a child unwinds its slice (the exit code
 * is picked up by wasm_run_child); SYS_exit from init reboots into
 * the host, matching what returning from _start already does.
 * WASM_SYS_spawn and SYS_wait6 are served from the child table
 * above rather than the MI exit paths, which assume real LWPs.
 *
 * Exported for the host (--export-all).
 */
void	wasm_user_reboot(void);		/* exec_wasm32.c */

int
wasm_user_svc(int nr, int argp)
{
	const void *uap = (const void *)(uintptr_t)argp;
	register_t rv = 0;
	struct lwp *l = curlwp;
	int error;

	/*
	 * Each case reinterprets argp as the specific args struct;
	 * the user side declares the identical layout.
	 */
	switch (nr) {
	case SYS_read:
		/*
		 * Blocking read: the console driver returns EWOULDBLOCK
		 * when its input ring is empty.  With JSPI, suspend the
		 * epoch in the host (which resolves when input arrives)
		 * and retry; the whole chain from the host's promising
		 * entry to wasm_host_wait is wasm frames, as JSPI
		 * requires.
		 *
		 * Pipes take the same route with a different wait
		 * reason: a pipe reader that cv_sleeps inside sys_read
		 * strands the slice (no runnable LWP can wake it in the
		 * epoch model) while the child that would feed the pipe
		 * waits for the host to start it.  Read pipes with
		 * FNONBLOCK forced on and let the host scheduler run
		 * pending children before the retry.
		 */
		for (;;) {
			const struct sys_read_args *ra = uap;
			file_t *fp;
			int ispipe = 0, wasnb = 0;

			fp = fd_getfile(SCARG(ra, fd));
			if (fp != NULL) {
				ispipe = fp->f_type == DTYPE_PIPE;
				if (ispipe) {
					wasnb = (fp->f_flag & FNONBLOCK) != 0;
					fp->f_flag |= FNONBLOCK;
				}
				fd_putfile(SCARG(ra, fd));
			}
			error = sys_read(l, ra, &rv);
			if (ispipe && !wasnb) {
				fp = fd_getfile(SCARG(ra, fd));
				if (fp != NULL) {
					fp->f_flag &= ~FNONBLOCK;
					fd_putfile(SCARG(ra, fd));
				}
			}
			if (error != EWOULDBLOCK || wasm_jspi == 0)
				break;
			wasm_host_wait(ispipe ? WASM_WAIT_CHILD :
			    WASM_WAIT_INPUT);
		}
		break;
	case SYS_write:
		error = sys_write(l, (const struct sys_write_args *)uap, &rv);
		break;
	case SYS_open:
		error = sys_open(l, (const struct sys_open_args *)uap, &rv);
		break;
	case SYS_close:
		error = sys_close(l, (const struct sys_close_args *)uap, &rv);
		break;
	case SYS_getpid: {
		register_t rvs[2] = { 0, 0 };
		error = sys_getpid_with_ppid(l, uap, rvs);
		rv = rvs[0];
		break;
	}
	case SYS_chdir:
		error = sys_chdir(l, (const struct sys_chdir_args *)uap, &rv);
		break;
	case SYS___getcwd:
		error = sys___getcwd(l, (const struct sys___getcwd_args *)uap,
		    &rv);
		break;
	case SYS_unlink:
		error = sys_unlink(l, (const struct sys_unlink_args *)uap, &rv);
		break;
	case SYS_mkdir:
		error = sys_mkdir(l, (const struct sys_mkdir_args *)uap, &rv);
		break;
	case SYS_rmdir:
		error = sys_rmdir(l, (const struct sys_rmdir_args *)uap, &rv);
		break;
	case SYS_access:
		error = sys_access(l, (const struct sys_access_args *)uap, &rv);
		break;
	case SYS_dup2:
		error = sys_dup2(l, (const struct sys_dup2_args *)uap, &rv);
		break;
	case SYS_readlink:
		error = sys_readlink(l, (const struct sys_readlink_args *)uap,
		    &rv);
		break;
	case SYS_fchdir:
		error = sys_fchdir(l, (const struct sys_fchdir_args *)uap, &rv);
		break;
	case SYS_rename:
		error = sys_rename(l, (const struct sys_rename_args *)uap, &rv);
		break;
	case SYS_link:
		error = sys_link(l, (const struct sys_link_args *)uap, &rv);
		break;
	case SYS_symlink:
		error = sys_symlink(l, (const struct sys_symlink_args *)uap, &rv);
		break;
	case SYS_fchmod:
		error = sys_fchmod(l, (const struct sys_fchmod_args *)uap, &rv);
		break;
	case SYS_fchown:
		error = sys_fchown(l, (const struct sys_fchown_args *)uap, &rv);
		break;
	case SYS_fpathconf:
		error = sys_fpathconf(l,
		    (const struct sys_fpathconf_args *)uap, &rv);
		break;
	case SYS___getdents30:
		error = sys___getdents30(l,
		    (const struct sys___getdents30_args *)uap, &rv);
		break;
	case SYS___stat50:
		error = sys___stat50(l, (const struct sys___stat50_args *)uap,
		    &rv);
		break;
	case WASM_SYS_spawn: {
		const struct wasm_spawn_args *sa = uap;
		int pid = wasm_child_spawn(
		    (const char *)(uintptr_t)sa->path,
		    (char *const *)(uintptr_t)sa->argv);
		return pid;
	}
	case SYS___wait450: {
		/* wait4(pid, status, options, rusage): pid -1 = any */
		const struct sys___wait450_args *wa = uap;
		pid_t pid = SCARG(wa, pid);

		error = wasm_child_wait(pid == -1 ? P_ALL : P_PID,
		    (id_t)pid, SCARG(wa, status), &rv);
		break;
	}
	case SYS_wait6: {
		/*
		 * Served from the child table: the MI wait path
		 * assumes real LWPs and signal delivery, neither of
		 * which exists for host-instantiated children.  Only
		 * P_ALL and P_PID blocking waits are supported; the
		 * loop suspends the epoch (JSPI) until the host has
		 * run a pending child to completion.
		 */
		const struct sys_wait6_args *wa = uap;

		error = wasm_child_wait(SCARG(wa, idtype), SCARG(wa, id),
		    SCARG(wa, status), &rv);
		break;
	}
	case SYS_lseek:
		error = sys_lseek(l, uap, &rv);
		break;
	case SYS_fcntl:
		error = sys_fcntl(l, uap, &rv);
		break;
	case SYS_getppid: {
		register_t rvs[2] = { 0, 0 };
		error = sys_getpid_with_ppid(l, uap, rvs);
		rv = rvs[1];
		break;
	}
	case SYS_getuid:
		error = sys_getuid(l, uap, &rv);
		break;
	case SYS_geteuid:
		error = sys_geteuid(l, uap, &rv);
		break;
	case SYS_getgid:
		error = sys_getgid(l, uap, &rv);
		break;
	case SYS_getegid:
		error = sys_getegid(l, uap, &rv);
		break;
	case SYS___sysctl:
		error = sys___sysctl(l, uap, &rv);
		break;
	case SYS_getrlimit:
		error = sys_getrlimit(l, uap, &rv);
		break;
	case SYS_setrlimit:
		error = sys_setrlimit(l, uap, &rv);
		break;
	case SYS___clock_gettime50:
		error = sys___clock_gettime50(l, uap, &rv);
		break;
	case SYS___gettimeofday50:
		error = sys___gettimeofday50(l, uap, &rv);
		break;
	case SYS___nanosleep50:
		error = sys___nanosleep50(l, uap, &rv);
		break;
	case SYS___fstat50:
		error = sys___fstat50(l, uap, &rv);
		break;
	case SYS___lstat50:
		error = sys___lstat50(l, uap, &rv);
		break;
	case SYS_lpathconf:
		error = sys_lpathconf(l, (const struct sys_lpathconf_args *)uap,
		    &rv);
		break;
	case SYS_pread:
		error = sys_pread(l, (const struct sys_pread_args *)uap, &rv);
		break;
	case SYS___acl_get_link:
		error = sys___acl_get_link(l,
		    (const struct sys___acl_get_link_args *)uap, &rv);
		break;
	case SYS___statvfs190:
		error = sys___statvfs190(l, uap, &rv);
		break;
	case SYS___fstatvfs190:
		error = sys___fstatvfs190(l, uap, &rv);
		break;
	case SYS_kill:
		error = 0;	/* no guest signal delivery yet */
		break;
	case SYS_dup:
		error = sys_dup(l, uap, &rv);
		break;
	case SYS_umask:
		error = sys_umask(l, uap, &rv);
		break;
	case SYS_getpgrp:
		error = sys_getpgrp(l, uap, &rv);
		break;
	case SYS_setpgid:
		error = sys_setpgid(l, uap, &rv);
		break;
	case SYS_fsync:
		error = sys_fsync(l, uap, &rv);
		break;
	case SYS_sched_yield:
		error = 0;
		break;
	case SYS_exit:
		if (wasm_current_child >= 0) {
			struct wasm_child *c = &wasm_children[wasm_current_child];
			const struct sys_exit_args *ea = uap;

			c->c_exitcode = SCARG(ea, rval);
			wasm_engine_throw();	/* to wasm_run_child */
			/* NOTREACHED */
		}
		printf("wasm: init exited via SYS_exit\n");
		wasm_user_reboot();	/* not reached */
		/* FALLTHROUGH */
	default:
		printf("wasm: svc %d unimplemented\n", nr);
		return -EINVAL;
	}

	return error != 0 ? -error : (int)rv;
}
