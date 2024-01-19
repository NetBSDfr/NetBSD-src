/* $NetBSD$ */

/*-
 * Copyright (c) 2024 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Emile 'iMil' Heitor.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*-
 * Copyright (c) 2017 Colin Percival
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/tslog.h>

#include <machine/atomic.h>
#include <machine/cpu.h>

#ifndef TSLOGSIZE
#define TSLOGSIZE 262144
#endif

#define nitems(x) __arraycount(x)

static volatile int nrecs = 0;
static struct timestamp {
	lwpid_t lid;
	int type;
	const char *f;
	const char *s;
	uint64_t tsc;
} timestamps[TSLOGSIZE];

void tslog(const lwp_t *, int, const char *, const char *);
static int sysctl_debug_tslog(SYSCTLFN_PROTO);

void
tslog(const lwp_t *l, int type, const char *f, const char *s)
{
	uint64_t tsc = rdtsc();

	/* A NULL thread is lwp0 before curthread is set. */
	if (l == NULL)
		l = &lwp0;

	/* Store record. */
	if (nrecs < nitems(timestamps)) {
		timestamps[nrecs].lid = l->l_lid;
		timestamps[nrecs].type = type;
		timestamps[nrecs].f = f;
		timestamps[nrecs].s = s;
		timestamps[nrecs].tsc = tsc;

		/* Grab a slot. */
		atomic_add_int(&nrecs, 1);
	}
}

static int
sysctl_debug_tslog(SYSCTLFN_ARGS)
{
	char buf[LINE_MAX] = "";
	char *where = oldp;
	size_t buflen, slen, i, limit, needed = 0;
	int error = 0;
	static bool first = true;
	static size_t max = 0;

	buflen = *oldlenp;
	/* Add data logged within the kernel. */
	limit = MIN(nrecs, nitems(timestamps));
	for (i = 0; i < limit; i++) {
		snprintf(buf, LINE_MAX, "0x%x %llu",
			timestamps[i].lid,
			(unsigned long long)timestamps[i].tsc);
		switch (timestamps[i].type) {
		case TS_ENTER:
			strcat(buf, " ENTER");
			break;
		case TS_EXIT:
			strcat(buf, " EXIT");
			break;
		case TS_THREAD:
			strcat(buf, " THREAD");
			break;
		case TS_EVENT:
			strcat(buf, " EVENT");
			break;
		}
		snprintf(buf, LINE_MAX, "%s %s", buf,
			timestamps[i].f ? timestamps[i].f : "(null)");
		if (timestamps[i].s)
			snprintf(buf, LINE_MAX, "%s %s\n", buf,
				timestamps[i].s);
		else
			strcat(buf, "\n");

		slen = strlen(buf) + 1;

		if (!first) {
			if (buflen < slen) {
				/* still not enough space */
				first = true;
				continue;
			}
			if (i > 0)
				where--; /* overwrite last \0 */
			if ((error = copyout(buf, where, slen)))
				break;
			where += slen;
			buflen -= slen;
		}
		needed += slen;
	}
	first = false;

	if (needed > max) {
		max = needed;
	}
	*oldlenp = max;

	return error;
}

MALLOC_DEFINE(M_TSLOGUSER, "tsloguser", "Strings used by userland tslog");
static struct procdata {
	pid_t ppid;
	uint64_t tsc_forked;
	uint64_t tsc_exited;
	char *execname;
	char *namei;
	int reused;
} procs[PID_MAX + 1];

void
tslog_user(pid_t pid, pid_t ppid, const char *execname, const char *namei)
{
	uint64_t tsc = rdtsc();
	size_t len;

	/* If we wrapped, do nothing. */
	if (procs[pid].reused)
		return;

	/* If we have a ppid, we're recording a fork. */
	if (ppid != (pid_t)(-1)) {
		/* If we have a ppid already, we wrapped. */
		if (procs[pid].ppid) {
			procs[pid].reused = 1;
			return;
		}

		/* Fill in some fields. */
		procs[pid].ppid = ppid;
		procs[pid].tsc_forked = tsc;
		return;
	}

	/* If we have an execname, record it. */
	if (execname != NULL) {
		if (procs[pid].execname != NULL)
			free(procs[pid].execname, M_TSLOGUSER);
		len = strlen(execname) + 1;
		procs[pid].execname = malloc(len,
			M_TSLOGUSER, M_WAITOK | M_ZERO);
		strlcpy(procs[pid].execname, execname, len);
		return;
	}

	/* Record the first namei for the process. */
	if (namei != NULL) {
		len = strlen(namei) + 1;
		if (procs[pid].namei == NULL) {
			procs[pid].namei = malloc(len,
				M_TSLOGUSER, M_WAITOK | M_ZERO);
			strlcpy(procs[pid].namei, namei, len);
		}
		return;
	}

	/* Otherwise we're recording an exit. */
	procs[pid].tsc_exited = tsc;
}

static int
sysctl_debug_tslog_user(SYSCTLFN_ARGS)
{
	char buf[LINE_MAX];
	char *where = oldp;
	size_t buflen, slen, needed = 0;
	int error = 0;
	static bool first = true;
	static size_t max = 0;
	pid_t pid;

	buflen = *oldlenp;
	/* Export the data we logged. */
	for (pid = 0; pid <= PID_MAX; pid++) {
		if (procs[pid].tsc_forked == 0 &&
			procs[pid].execname == NULL &&
			procs[pid].namei == NULL &&
			procs[pid].tsc_exited == 0)
			continue;
		snprintf(buf, LINE_MAX, "%zu", (size_t)pid);
		snprintf(buf, LINE_MAX, "%s  %zu", buf, (size_t)procs[pid].ppid);
		snprintf(buf, LINE_MAX, "%s  %llu", buf,
		    (unsigned long long)procs[pid].tsc_forked);
		snprintf(buf, LINE_MAX, "%s  %llu", buf,
		    (unsigned long long)procs[pid].tsc_exited);
		snprintf(buf, LINE_MAX, "%s  \"%s\"", buf, procs[pid].execname ?
		    procs[pid].execname : "");
		snprintf(buf, LINE_MAX, "%s  \"%s\"", buf, procs[pid].namei ?
		    procs[pid].namei : "");
		strcat(buf, "\n");

		slen = strlen(buf) + 1;

		if (!first) {
			if (buflen < slen) {
				/* still not enough space */
				first = true;
				continue;
			}
			if (pid > 0)
				where--; /* overwrite last \0 */
			if ((error = copyout(buf, where, slen)))
				break;
			where += slen;
			buflen -= slen;
		}
		needed += slen;
	}
	first = false;

	if (needed > max) {
		max = needed;
	}
	*oldlenp = max;

	return (error);
}

SYSCTL_SETUP(sysctl_tslog_setup, "tslog sysctl")
{
	sysctl_createv(NULL, 0, NULL, NULL,
		CTLFLAG_PERMANENT|CTLFLAG_READONLY,
		CTLTYPE_STRING, "tslog",
		SYSCTL_DESCR("Dump recorded event timestamps"),
		sysctl_debug_tslog, 0, NULL, 0,
		CTL_CREATE, CTL_EOL);
	sysctl_createv(NULL, 0, NULL, NULL,
		CTLFLAG_PERMANENT|CTLFLAG_READONLY,
		CTLTYPE_STRING, "tslog_user",
		SYSCTL_DESCR("Dump recorded userland event timestamps"),
		sysctl_debug_tslog_user, 0, NULL, 0,
		CTL_CREATE, CTL_EOL);
}

