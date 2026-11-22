/* SPDX-License-Identifier: MIT */
/*
 * iotrace - LD_PRELOAD ioctl timeline for the gpu31 latency work.
 *
 * Logs every DRM ioctl (type 'd', 0x64) as
 *
 *   <monotonic_ns_start> <fd> <request_hex> <return> <duration_ns>
 *
 * so a per-frame timeline (SUBMIT / WAIT_BO / SYNCOBJ_WAIT / ...) can
 * be reconstructed without kernel tracing (this kernel has only the nop
 * tracer and no kprobes). Overhead is one clock_gettime pair per DRM
 * ioctl; the file is flushed each line so a wedged run still leaves a
 * usable trace.
 *
 * Build on the target (musl/Alpine) or any aarch64 toolchain:
 *   gcc -O2 -shared -fPIC -o iotrace.so iotrace.c -ldl
 * Use:
 *   IOTRACE=/tmp/iotrace.log LD_PRELOAD=/tmp/iotrace.so <app>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

static int (*real_ioctl)(int, unsigned long, void *);
static FILE *logf;
static __thread int in_hook;

static long long ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

__attribute__((constructor)) static void init(void)
{
	const char *p;

	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	p = getenv("IOTRACE");
	logf = fopen(p ? p : "/tmp/iotrace.log", "w");
}

__attribute__((destructor)) static void fini(void)
{
	if (logf)
		fclose(logf);
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	long long t0, t1;
	int r;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (!real_ioctl)
		init();
	if (in_hook || !logf)
		return real_ioctl(fd, req, arg);
	in_hook = 1;
	t0 = ns();
	r = real_ioctl(fd, req, arg);
	t1 = ns();
	/* DRM ioctl type byte: _IOC type 'd' (0x64) in bits 8..15. */
	if (((req >> 8) & 0xff) == 0x64 || fd == 5)
		fprintf(logf, "%lld %d %lx %d %lld\n", t0, fd, req, r,
			t1 - t0);
	fflush(logf);
	in_hook = 0;
	return r;
}
