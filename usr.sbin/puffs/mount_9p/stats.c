/*	$NetBSD$	*/

/*
 * SIGINFO diagnostics for mount_9p: count every 9P wire message by type
 * (see ninebuf.c hooks) and dump them on SIGINFO, together with the
 * negotiated msize.  The dump goes to stderr and to /tmp/mount_9p.stats;
 * the daemon's pid is written to /tmp/mount_9p.pid so scripts can
 * "kill -INFO $(cat /tmp/mount_9p.pid)".
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ninepuffs.h"
#include "nineproto.h"

static struct puffs_usermount *stats_pu;

#define STATS_PIDFILE	"/tmp/mount_9p.pid"
#define STATS_DUMPFILE	"/tmp/mount_9p.stats"

static const char *
msgbase(uint8_t type)
{

	switch (type & ~1) {
	case P9PROTO_T_VERSION:	return "VERSION";
	case P9PROTO_T_AUTH:	return "AUTH";
	case P9PROTO_T_ATTACH:	return "ATTACH";
	case P9PROTO_T_ERROR:	return "ERROR";
	case P9PROTO_T_FLUSH:	return "FLUSH";
	case P9PROTO_T_WALK:	return "WALK";
	case P9PROTO_T_OPEN:	return "OPEN";
	case P9PROTO_T_CREATE:	return "CREATE";
	case P9PROTO_T_READ:	return "READ";
	case P9PROTO_T_WRITE:	return "WRITE";
	case P9PROTO_T_CLUNK:	return "CLUNK";
	case P9PROTO_T_REMOVE:	return "REMOVE";
	case P9PROTO_T_STAT:	return "STAT";
	case P9PROTO_T_WSTAT:	return "WSTAT";
	case P9PROTO_T_L_READLINK: return "LREADLINK";
	default:		return NULL;
	}
}

static void
stats_vadd(char *buf, size_t bufsz, size_t *off, const char *fmt, ...)
{
	char line[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (*off + (size_t)n < bufsz) {
		memcpy(buf + *off, line, (size_t)n);
		*off += (size_t)n;
		buf[*off] = '\0';
	}
}

static void
stats_dump(void)
{
	struct puffs9p *p9p = puffs_getspecific(stats_pu);
	char buf[4096];
	size_t off = 0;
	unsigned t;
	const char *nm;
	int fd;

	off = (size_t)snprintf(buf, sizeof(buf),
	    "msize=%zu bytes_tx=%" PRIu64 " bytes_rx=%" PRIu64 "\n",
	    p9p->maxreq, p9p->bytes_tx, p9p->bytes_rx);
	buf[off] = '\0';

	for (t = 0; t < 256; t++) {
		uint64_t tx = p9p->stats_tx[t];
		uint64_t rx = p9p->stats_rx[t];

		if (tx == 0 && rx == 0)
			continue;
		nm = msgbase((uint8_t)t);
		if (nm != NULL)
			stats_vadd(buf, sizeof(buf), &off, "%c_%s tx=%" PRIu64
			    " rx=%" PRIu64 "\n", (t & 1) ? 'R' : 'T', nm, tx, rx);
		else
			stats_vadd(buf, sizeof(buf), &off,
			    "type=%u tx=%" PRIu64 " rx=%" PRIu64 "\n", t, tx, rx);
	}

	fd = open(STATS_DUMPFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd != -1) {
		if (write(fd, buf, off) == -1) {}
		close(fd);
	}
	if (write(STDERR_FILENO, buf, off) == -1) {}
}

static void
stats_siginfo(int sig)
{
	int error = errno;

	stats_dump();
	errno = error;
}

void
p9p_stats_start(struct puffs_usermount *pu)
{
	char pidbuf[32];
	int fd, n;

	stats_pu = pu;
	signal(SIGINFO, stats_siginfo);

	fd = open(STATS_PIDFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd != -1) {
		n = snprintf(pidbuf, sizeof(pidbuf), "%d\n", getpid());
		if (write(fd, pidbuf, (size_t)n) == -1) {}
		close(fd);
	}
	unlink(STATS_DUMPFILE);
}

void
p9p_stats_count(struct puffs_usermount *pu, uint8_t type, size_t len, int out)
{
	struct puffs9p *p9p = puffs_getspecific(pu);

	if (out) {
		p9p->stats_tx[type]++;
		p9p->bytes_tx += len;
	} else {
		p9p->stats_rx[type]++;
		p9p->bytes_rx += len;
	}
}
