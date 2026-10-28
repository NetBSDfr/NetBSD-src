/*	$NetBSD: virtgpu_trace.h,v 1.1 2026-10-26 smolBSD Exp $	*/

/*
 * No-op tracepoint shims for the virtio-gpu DRM driver.  The Linux
 * virtgpu_trace.h defines TRACE_EVENT(virtio_gpu_cmd_*) machinery; the
 * NetBSD kernel build has no tracepoint consumer for this driver, so
 * elide the events.
 */

#ifndef	_VIRTGPU_TRACE_H_
#define	_VIRTGPU_TRACE_H_

#define	trace_virtio_gpu_cmd_queue(...)		/* nothing */
#define	trace_virtio_gpu_cmd_response(...)	/* nothing */

#endif	/* _VIRTGPU_TRACE_H_ */
