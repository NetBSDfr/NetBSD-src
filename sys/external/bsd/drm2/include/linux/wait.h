/*	$NetBSD: wait.h,v 1.2 2014/03/18 18:20:43 riastradh Exp $	*/

/*-
 * Copyright (c) 2013 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Taylor R. Campbell.
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

#ifndef _LINUX_WAIT_H_
#define _LINUX_WAIT_H_

/*
 * Minimal wait queues emulated with a mutex/condvar pair.  wake_up()
 * holds the wait-queue mutex so that checks of the condition and entry
 * into the sleep are serialized against the wakeup (no lost wakeups).
 * Waits use a bounded poll slice so they also recover from any residual
 * wakeup races.
 */

#include <sys/types.h>
#include <sys/condvar.h>
#include <sys/mutex.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/types.h>

typedef struct wait_queue_head {
	kmutex_t	wqh_mtx;
	kcondvar_t	wqh_cv;
} wait_queue_head_t;

typedef wait_queue_head_t	wait_queue_t;

static inline void
init_waitqueue_head(wait_queue_head_t *wq)
{

	mutex_init(&wq->wqh_mtx, MUTEX_DEFAULT, IPL_SOFTNET);
	cv_init(&wq->wqh_cv, "linuxwq");
}

static inline void
destroy_waitqueue_head(wait_queue_head_t *wq)
{

	cv_destroy(&wq->wqh_cv);
	mutex_destroy(&wq->wqh_mtx);
}

static inline void
wake_up(wait_queue_head_t *wq)
{

	mutex_enter(&wq->wqh_mtx);
	cv_broadcast(&wq->wqh_cv);
	mutex_exit(&wq->wqh_mtx);
}

#define	wake_up_all(wq)	wake_up(wq)
#define	wake_up_interruptible(wq)	wake_up(wq)

static inline void
__linux_wait_event(wait_queue_head_t *wq, long *const timeout)
{
	int error;

	mutex_enter(&wq->wqh_mtx);
	/*
	 * Sleep in slices of at most 100ms so that the poll loop also
	 * recovers from any lost wakeup; count elapsed slices down in
	 * caller units (jiffies).
	 */
	error = cv_timedwait(&wq->wqh_cv, &wq->wqh_mtx, hz / 10 + 1);
	if (timeout != NULL && error == EWOULDBLOCK)
		*timeout -= 1;
	mutex_exit(&wq->wqh_mtx);
}

#define	wait_event(wq, condition)					      \
	do {								      \
		while (!(condition))					      \
			__linux_wait_event(&(wq), NULL);		      \
	} while (/*CONSTCOND*/0)

#define	wait_event_interruptible(wq, condition)				      \
	(wait_event((wq), (condition)), 0)

/*
 * Returns remaining timeout in the caller's units, or 0 on timeout.
 */
#define	wait_event_timeout(wq, condition, timeout)			      \
	({								      \
		long __remain = (timeout);				      \
		bool __done = false;					      \
		while (__remain > 0) {					      \
			__done = (condition);				      \
			if (__done)					      \
				break;					      \
			__linux_wait_event(&(wq), &__remain);		      \
			if (__remain > 0)				      \
				__remain--;				      \
		}							      \
		__done ? __remain : 0;					      \
	})

#endif  /* _LINUX_WAIT_H_ */
