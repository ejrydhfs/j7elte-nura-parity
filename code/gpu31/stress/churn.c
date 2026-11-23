/* gpu31-fpssplit: CREATE_BO + MMAP_BO + mmap + GEM_CLOSE churn.
 *
 * Originally the gpu31-mmlock stress (4 threads x 250 iters x <=6 BOs),
 * plus a sequential phase that proves a freed BO's fake offset stops
 * resolving. The concurrent phase deliberately probes a stale offset
 * (a reused offset may legitimately resolve to another thread's live BO;
 * the sequential phase is the deterministic check).
 *
 * The two bugs it caught:
 *   - kernel: the 3.10 core mmap-offset manager was corrupted by
 *     concurrent create/free (gpu31-mmlock, fixed by the driver-owned
 *     offset manager under mmap_offset_lock);
 *   - client: the first cleanup loop unmapped a BO without clearing
 *     bo[i].map, so the second loop munmapped the same range again and
 *     could destroy a VMA another thread had just obtained (crash
 *     address was a hole in /proc/self/maps, i.e. SEGV_MAPERR at a
 *     successful mmap() return). Fixed here.
 *
 * Build (phone): aarch64-linux-gnu-gcc -O2 -static -pthread \
 *   -I../client -I../gem churn.c -o churn
 * Run: churn [threads] [iters] [max_bos]   e.g. churn 4 250 6
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include "panfrost_drm.h"

#ifndef DRM_IOCTL_GEM_CLOSE
struct drm_gem_close_local { __u32 handle; __u32 pad; };
#define DRM_IOCTL_GEM_CLOSE DRM_IOW(0x09, struct drm_gem_close_local)
#else
struct drm_gem_close_local { __u32 handle; __u32 pad; };
#endif

static const char *node = "/dev/dri/renderD128";

static void puthex(unsigned long long v, char *o)
{
	int i;
	const char *h = "0123456789abcdef";

	for (i = 15; i >= 0; i--)
		o[i] = h[v & 0xf], v >>= 4;
}

/* Crash discriminator: faulting address + the process maps, so an
 * address inside a fake-offset VMA can be told apart from a wild
 * client pointer. Only write(2)/open/read are used. */
static void on_segv(int sig, siginfo_t *si, void *uc)
{
	char buf[4096];
	char hex[16];
	char *p = buf;
	int fd, n;
	unsigned long long a = (unsigned long long)(unsigned long)si->si_addr;

	(void)sig; (void)uc;
	puthex(a, hex);
	memcpy(p, "CHURN SEGV addr=0x", 18); p += 18;
	memcpy(p, hex, 16); p += 16;
	memcpy(p, " maps follow:\n", 14); p += 14;
	write(2, buf, (size_t)(p - buf));
	fd = open("/proc/self/maps", O_RDONLY);
	if (fd >= 0) {
		while ((n = read(fd, buf, sizeof(buf))) > 0)
			write(2, buf, (size_t)n);
		close(fd);
	}
	_exit(3);
}
static int g_iters = 50;
static int g_nbo = 6;
static int g_errs;
static int g_stale_reused;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void fail(const char *what, int e)
{
	pthread_mutex_lock(&g_lock);
	g_errs++;
	pthread_mutex_unlock(&g_lock);
	fprintf(stderr, "CHURN FAIL %s errno=%d (%s)\n", what, e, strerror(e));
}

static int open_dev(void)
{
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "CHURN cannot open %s: %s\n", node, strerror(errno));
		exit(2);
	}
	return fd;
}

static void *worker(void *arg)
{
	long id = (long)arg;
	unsigned long seed = 0x9e3779b9u * (id + 1);
	int fd = open_dev();
	int it;

	for (it = 0; it < g_iters; it++) {
		struct {
			__u32 handle;
			__u32 size;
			void *map;
			__u64 off;
		} bo[16];
		int n = 1 + (int)(rand_r((unsigned int *)&seed) % g_nbo);
		int i;

		if (n > 16)
			n = 16;
		memset(bo, 0, sizeof(bo));
		for (i = 0; i < n; i++) {
			struct drm_panfrost_create_bo c;
			struct drm_panfrost_mmap_bo m;
			int sz = 4096 << (rand_r((unsigned int *)&seed) % 9); /* 4K..1M */

			memset(&c, 0, sizeof(c));
			c.size = sz;
			if (ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c)) {
				fail("create", errno);
				continue;
			}
			memset(&m, 0, sizeof(m));
			m.handle = c.handle;
			if (ioctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m)) {
				fail("mmap_bo", errno);
				bo[i].handle = c.handle;
				bo[i].map = NULL;
				continue;
			}
			bo[i].handle = c.handle;
			bo[i].off = m.offset;
			bo[i].size = sz;
			bo[i].map = mmap(NULL, sz, PROT_READ | PROT_WRITE,
					 MAP_SHARED, fd, m.offset);
			if (bo[i].map == MAP_FAILED) {
				fail("mmap", errno);
				bo[i].map = NULL;
			} else {
				/* fault in a page at each end */
				((volatile char *)bo[i].map)[0] = (char)it;
				((volatile char *)bo[i].map)[sz - 1] = (char)it;
			}
		}
		/* Last BO of the batch: unmap, close the handle (last ref ->
		 * gem_free_object) and then probe the now-stale offset. The
		 * offset must stop resolving once the object is gone.
		 * Other threads are creating/closing/mapping their own BOs at
		 * the same time, which is the real race window. */
		for (i = 0; i < n; i++) {
			if (!bo[i].handle)
				continue;
			if (bo[i].map && ((it + i) & 1)) {
				munmap(bo[i].map, bo[i].size);
				/* gpu31-fpssplit: drop the pointer with the
				 * mapping. Without this the second cleanup
				 * loop munmaps the SAME range again; any
				 * thread that mmapped into that range in
				 * between keeps a valid mmap() return whose
				 * VMA the stale munmap destroys -> the
				 * client (not the kernel) takes SIGSEGV
				 * MAPERR on its first store. */
				bo[i].map = NULL;
			}
			if (i == n - 1) {
				struct drm_gem_close_local cl;

				if (bo[i].map) {
					munmap(bo[i].map, bo[i].size);
					bo[i].map = NULL;
				}
				cl.handle = bo[i].handle;
				cl.pad = 0;
				if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl))
					fail("gem_close", errno);
				bo[i].handle = 0;
				/* stale mmap: expect failure (EINVAL/ENODEV/ENOENT) */
				if (getenv("CHURN_NOSTALE")) continue;
				{
					void *p = mmap(NULL, bo[i].size,
						       PROT_READ | PROT_WRITE,
						       MAP_SHARED, fd, bo[i].off);
					/* Under concurrency the range may already
					 * belong to another thread's new BO: that
					 * is legitimate offset reuse, not a bug.
					 * The deterministic check is the sequential
					 * phase in main(). */
					if (p != MAP_FAILED) {
						pthread_mutex_lock(&g_lock);
						g_stale_reused++;
						pthread_mutex_unlock(&g_lock);
						munmap(p, bo[i].size);
					}
				}
			}
		}
		for (i = 0; i < n; i++) {
			struct drm_gem_close_local cl;

			if (!bo[i].handle)
				continue;
			if (bo[i].map)
				munmap(bo[i].map, bo[i].size);
			cl.handle = bo[i].handle;
			cl.pad = 0;
			if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl))
				fail("gem_close2", errno);
		}
		/* client-exit churn: drop the whole fd (context teardown) */
		if ((it % 7) == 0) {
			close(fd);
			fd = open_dev();
		}
	}
	close(fd);
	return NULL;
}

/* Sequential phase: no reuse can race, so the offset MUST stop
 * resolving once the mapping and handle are gone. */
static void sequential_check(void)
{
	static const int sizes[] = { 4096, 65536, 1048576 };
	int fd = open_dev();
	unsigned int k;

	for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		struct drm_panfrost_create_bo c;
		struct drm_panfrost_mmap_bo m;
		struct drm_gem_close_local cl;
		void *p;

		memset(&c, 0, sizeof(c));
		c.size = sizes[k];
		if (ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c)) {
			fail("seq create", errno);
			break;
		}
		memset(&m, 0, sizeof(m));
		m.handle = c.handle;
		if (ioctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m)) {
			fail("seq mmap_bo", errno);
			break;
		}
		p = mmap(NULL, sizes[k], PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, m.offset);
		if (p == MAP_FAILED) {
			fail("seq mmap", errno);
			break;
		}
		((volatile char *)p)[0] = 1;
		munmap(p, sizes[k]);
		cl.handle = c.handle;
		cl.pad = 0;
		if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl)) {
			fail("seq gem_close", errno);
			break;
		}
		p = mmap(NULL, sizes[k], PROT_READ | PROT_WRITE, MAP_SHARED,
			 fd, m.offset);
		if (p != MAP_FAILED) {
			fail("seq stale-mmap-succeeded", 0);
			munmap(p, sizes[k]);
		}
	}
	close(fd);
}

int main(int argc, char **argv)
{
	int nthreads = argc > 1 ? atoi(argv[1]) : 4;
	int iters = argc > 2 ? atoi(argv[2]) : 50;
	int nbo = argc > 3 ? atoi(argv[3]) : 6;
	pthread_t th[64];
	int i;

	{
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = on_segv;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
	}
	sequential_check();

	if (nthreads > 64)
		nthreads = 64;
	g_iters = iters;
	g_nbo = nbo;
	for (i = 0; i < nthreads; i++)
		if (pthread_create(&th[i], NULL, worker, (void *)(long)i)) {
			fprintf(stderr, "CHURN pthread_create failed\n");
			return 2;
		}
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	printf("CHURN %s iters=%d threads=%d errs=%d stale_reused=%d\n",
	       g_errs ? "FAIL" : "ok", nthreads * iters, nthreads, g_errs,
	       g_stale_reused);
	return g_errs ? 1 : 0;
}
