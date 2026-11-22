/* SPDX-License-Identifier: MIT */
/*
 * polycensus.c - LD_PRELOAD diagnostic for the gpu31 polygon list.
 *
 * Runs inside Mesa's process, tracks CREATE_BO (handle -> GPU IOVA),
 * and on every SUBMIT that carries an out-sync (the fragment submit,
 * per panfrost_batch_submit_jobs) waits on that syncobj fence itself,
 * then reads everything through the DEBUG-ONLY kernel route
 * (DRM_IOCTL at 0xCF, p7580_gem_debug_read_word: clean-then-invalidate
 * + kmap of the BO page) instead of a userspace alias:
 *
 *   - the fragment job header/payload (type 9), the SFBD it points at,
 *   - the FBD tiler section: polygon_list_size/hierarchy/polygon_list/
 *     polygon_list_body/heap_start/heap_end,
 *   - the polygon-list prologue + 1024 8-byte header entries (census:
 *     nonzero, in-range, first/last, first entries), body tiles,
 *     tiler heap,
 *   - the preceding chain submit's job descriptors (walk `next`),
 *     Primitive word, tiler position pointer, tiler/vertex Draw.State,
 *   - the tiler position buffer, the VS/FS Renderer State and ISA.
 *
 * Output lines start with "PC2 " on stderr. Env:
 *   PC2_MAX   number of fragment submits to census (default 2)
 *   PC2_RAW   if set, dump raw words of every job descriptor
 *
 * Device-only diagnostic; not part of the shipped driver/Mesa.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h> /* _IOC_TYPE/_IOC_NR/_IOWR; not <sys/ioctl.h> (musl and glibc disagree on the prototype) */

#define DRM_IOCTL_BASE		'd'
#define DRM_COMMAND_BASE	0x40

struct pf_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};
struct pf_submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct pf_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};
struct pf_debug_read_bo {
	uint32_t handle, offset, value, pad;
};
struct drm_syncobj_wait_arg {
	uint64_t handles;
	int64_t timeout_ns;
	uint32_t count_handles, flags, first_signaled, pad;
};

#define NR_SUBMIT	(DRM_COMMAND_BASE + 0x00)
#define NR_CREATE_BO	(DRM_COMMAND_BASE + 0x02)
#define NR_MMAP_BO	(DRM_COMMAND_BASE + 0x03)
#define NR_DEBUG_READ	0xCF
#define NR_SYNC_WAIT	0xC3
/* Request encodings must match _IOC_SIZE exactly: the 3.10 core keys
 * copy_from/to_user on the driver's _IOC_SIZE. */
#define REQ_SUBMIT	_IOWR(DRM_IOCTL_BASE, NR_SUBMIT, char[40])
#define REQ_CREATE	_IOWR(DRM_IOCTL_BASE, NR_CREATE_BO, char[24])
#define REQ_MMAPBO	_IOWR(DRM_IOCTL_BASE, NR_MMAP_BO, char[16])
#define REQ_DEBUG	_IOWR(DRM_IOCTL_BASE, NR_DEBUG_READ, char[16])
#define REQ_WAIT	_IOWR(DRM_IOCTL_BASE, NR_SYNC_WAIT, char[32])

#define MAXBO		4096
#define MAXMAP		1024
#define MAXCHAIN	8

struct boent {
	uint32_t handle;
	uint32_t size;
	uint64_t gpu;
};

static int (*real_ioctl)(int, unsigned long, ...);
static int drv_fd = -1;
static struct boent bos[MAXBO];
static int nbos;
static int nr_fd_seen;

struct mape {
	uint64_t gpu;
	uint64_t size;
	void *cpu;
};
static struct mape maps[MAXMAP];
static int nmaps;

struct fakeent {
	uint32_t handle;
	uint64_t fake;
};
static struct fakeent fakes[256];
static int nfakes;

static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

static struct {
	uint64_t jc;
	uint32_t out_sync;
	uint32_t nbos;
	uint32_t h[64];
} last_chain;
static int have_chain;
static unsigned census_done;
static uint64_t g_tiler_jc;
static uint32_t g_tiler_prim, g_tiler_dep;
static int g_tiler_valid;
static uint64_t g_pl;
static unsigned census_max = 2;
static int raw_mode;

static void pc2(const char *fmt, ...)
{
	va_list ap;

	fputs("PC2 ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}

static void resolve(void)
{
	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	if (!real_mmap)
		real_mmap = dlsym(RTLD_NEXT, "mmap");
}

static void note_mmap(char *addr, size_t len, int fd, uint64_t off)
{
	int i;

	if (fd != drv_fd || fd < 0 || !addr)
		return;
	for (i = nfakes - 1; i >= 0; i--) {
		if (fakes[i].fake != off)
			continue;
		if (nmaps < MAXMAP) {
			int j;

			for (j = 0; j < nbos; j++) {
				if (bos[j].handle != fakes[i].handle)
					continue;
				maps[nmaps].gpu = bos[j].gpu;
				maps[nmaps].size = len;
				maps[nmaps].cpu = addr;
				nmaps++;
				return;
			}
		}
		return;
	}
}

static int rd32(uint32_t handle, uint32_t off, uint32_t *out)
{
	struct pf_debug_read_bo a;

	memset(&a, 0, sizeof(a));
	a.handle = handle;
	a.offset = off;
	if (real_ioctl(drv_fd, REQ_DEBUG, &a))
		return -errno;
	*out = a.value;
	return 0;
}

static int rd64(uint32_t handle, uint32_t off, uint64_t *out)
{
	uint32_t lo, hi;

	if (rd32(handle, off, &lo) || rd32(handle, off + 4, &hi))
		return -1;
	*out = (uint64_t)lo | ((uint64_t)hi << 32);
	return 0;
}

static int bo_find(uint64_t addr, uint32_t *handle, uint32_t *off)
{
	int i;

	for (i = nbos - 1; i >= 0; i--) {
		if (addr >= bos[i].gpu &&
		    addr < bos[i].gpu + (uint64_t)bos[i].size &&
		    bos[i].size) {
			*handle = bos[i].handle;
			*off = (uint32_t)(addr - bos[i].gpu);
			return 0;
		}
	}
	return -1;
}

/* Read `n` consecutive 32-bit words at a GPU address (may cross pages). */
static int rd_words(uint64_t addr, uint32_t *w, unsigned n)
{
	uint32_t h, off;
	unsigned i;

	if (bo_find(addr, &h, &off))
		return -1;
	if (n == 0)
		return 0;
	/* Debug read is per-word; offsets are byte offsets into the BO. */
	for (i = 0; i < n; i++) {
		uint32_t a = off + i * 4;

		if (rd32(h, a, &w[i]))
			return -1;
	}
	return 0;
}

static void hexline(const char *tag, uint64_t addr, const uint32_t *w,
		    unsigned n)
{
	unsigned i;

	fprintf(stderr, "PC2 %s at=0x%llx:", tag, (unsigned long long)addr);
	for (i = 0; i < n; i++)
		fprintf(stderr, " %08x", w[i]);
	fputc('\n', stderr);
}

static int syncobj_wait(uint32_t handle, int64_t timeout_ns)
{
	struct drm_syncobj_wait_arg w;

	memset(&w, 0, sizeof(w));
	w.handles = (uint64_t)(uintptr_t)&handle;
	w.timeout_ns = timeout_ns;
	w.count_handles = 1;
	return real_ioctl(drv_fd, REQ_WAIT, &w) ? -errno : 0;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void dump_state(uint64_t st, const char *which)
{
	uint32_t w[32];
	uint64_t sh;
	uint32_t isa[24];
	uint64_t ih;

	if (!st)
		return;
	if (rd_words(st, w, 32)) {
		pc2("%s state at=0x%llx: unreadable", which,
		    (unsigned long long)st);
		return;
	}
	hexline("state", st, w, 16);
	sh = (uint64_t)w[0] | ((uint64_t)w[1] << 32);
	pc2("%s meta shader=0x%llx tag=%u", which,
	    (unsigned long long)sh, (unsigned)(sh & 0xf));
	ih = sh & ~0xfull;
	if (rd_words(ih, isa, 24) == 0)
		hexline("isa", ih, isa, 12);
	else
		pc2("%s isa at=0x%llx: unreadable", which,
		    (unsigned long long)ih);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	void *r;

	resolve();
	r = real_mmap(addr, len, prot, flags, fd, off);
	note_mmap(r, len, fd, (uint64_t)off);
	return r;
}

/* PFN of a userspace address via /proc/self/pagemap (root). 3.10
 * layout: bits 0..54 pfn, bit 63 present. */
static uint64_t page_phys(void *addr)
{
	uint64_t e = 0;
	int fd = open("/proc/self/pagemap", O_RDONLY);

	if (fd < 0)
		return 0;
	if (pread(fd, &e, 8, ((uintptr_t)addr >> 12) * 8) != 8)
		e = 0;
	close(fd);
	if (!(e & (1ULL << 63)))
		return 0;
	return (e & ((1ULL << 55) - 1)) << 12;
}

/* Map a BO read-only through the driver's MMAP_BO fake offset. */
static void *bo_map_ro(uint32_t handle, size_t size)
{
	struct pf_mmap_bo m;
	void *p;

	memset(&m, 0, sizeof(m));
	m.handle = handle;
	if (real_ioctl(drv_fd, REQ_MMAPBO, &m))
		return NULL;
	p = real_mmap(NULL, size, PROT_READ, MAP_SHARED, drv_fd, m.offset);
	return (p == MAP_FAILED) ? NULL : p;
}

static void *bo_map_rw(uint32_t handle, size_t size)
{
	struct pf_mmap_bo m;
	void *p;

	memset(&m, 0, sizeof(m));
	m.handle = handle;
	if (real_ioctl(drv_fd, REQ_MMAPBO, &m))
		return NULL;
	p = real_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, drv_fd,
		      m.offset);
	return (p == MAP_FAILED) ? NULL : p;
}

/* Diagnostic: the T720 tiler's Draw.Varyings is fs_vary. Rewrite it with
 * the vertex job's vs_vary list (entry 0 = gl_Position SNAP_4 / buffer 1)
 * before the submit ioctl, so the driver's submit clean covers the edit. */
static void patch_tiler_varyings(struct pf_submit *s)
{
	uint32_t h, off, size = 0;
	uint64_t base, job;
	uint64_t vs_vary = 0;
	void *map;
	int k, j;

	if ((!getenv("PC2_SWAPVARY") && !getenv("PC2_BADPOS") && !getenv("PC2_POINTS") && !getenv("PC2_ZEROPRIM") && !getenv("PC2_CPUPOS") && !getenv("PC2_CPUPOS2") && !getenv("PC2_TILERAFTER") && !getenv("PC2_TILERAFTER2")) || !s->jc)
		return;
	if (bo_find(s->jc, &h, &off))
		return;
	for (j = 0; j < nbos; j++)
		if (bos[j].handle == h)
			size = bos[j].size;
	if (!size)
		return;
	map = bo_map_rw(h, size);
	if (!map)
		return;

	base = s->jc - off;
	job = s->jc;
	for (k = 0; k < 4; k++) {
		uint32_t *jw;
		uint32_t type;
		uint64_t next, v;

		if (job < base || job - base + 0xc0 > size)
			break;
		jw = (uint32_t *)map + ((job - base) >> 2);
		type = (jw[4] >> 1) & 0xf;
		next = (uint64_t)jw[6] | ((uint64_t)jw[7] << 32);
		v = (uint64_t)jw[38] | ((uint64_t)jw[39] << 32);
		if (type == 2) {
			g_pl = (uint64_t)jw[8] | ((uint64_t)jw[9] << 32);
			pc2("pl-from-wv: 0x%llx", (unsigned long long)g_pl);
		}
		if (type == 5) {
			vs_vary = v;
			pc2("swap: vertex varyings=0x%llx",
			    (unsigned long long)v);
		} else if (type == 7) {
			pc2("swap: tiler varyings 0x%llx -> 0x%llx",
			    (unsigned long long)v, (unsigned long long)vs_vary);
			if (vs_vary) {
				jw[38] = (uint32_t)vs_vary;
				jw[39] = (uint32_t)(vs_vary >> 32);
			}
			if (getenv("PC2_POINTS")) {
				jw[10] = (jw[10] & ~0xffu) | 0x1;
				pc2("point: primitive -> 0x%08x", jw[10]);
			}
			if (getenv("PC2_BADPOS")) {
				pc2("badpos: position 0x%llx -> 0xdeadb000",
				    (unsigned long long)((uint64_t)jw[20] |
					((uint64_t)jw[21] << 32)));
				jw[20] = 0xdeadb000;
				jw[21] = 0;
			}
			if (getenv("PC2_CPUPOS2")) {
				uint64_t vbuf = (uint64_t)jw[36] |
					((uint64_t)jw[37] << 32);
				uint64_t pos = (uint64_t)jw[20] |
					((uint64_t)jw[21] << 32);
				uint32_t vh, voff, vsize = 0;
				uint32_t ph, poff, psize = 0;
				void *vmap = NULL, *pmap = NULL;
				float tri[12] = {
					0.0f, 0.0f, 0.5f, 1.0f,
					240.0f, 0.0f, 0.5f, 1.0f,
					120.0f, 240.0f, 0.5f, 1.0f
				};
				int q;

				if (!g_pl)
					goto cpupos2_done;
				if (bo_find(vbuf, &vh, &voff) ||
				    bo_find(g_pl, &ph, &poff))
					goto cpupos2_done;
				for (q = 0; q < nbos; q++) {
					if (bos[q].handle == vh)
						vsize = bos[q].size;
					if (bos[q].handle == ph)
						psize = bos[q].size;
				}
				if (vsize < 0x40 || psize < 0x80200)
					goto cpupos2_done;
				vmap = bo_map_rw(vh, vsize);
				pmap = bo_map_rw(ph, psize);
				if (!vmap || !pmap)
					goto cpupos2_done;
				{
					uint32_t *src = (uint32_t *)
						((char *)vmap + (voff & ~3u));
					uint32_t *cpy = (uint32_t *)
						((char *)pmap + 0x80100);
					uint32_t *dst = (uint32_t *)
						((char *)pmap + 0x80000);
					uint64_t npos = g_pl + 0x80000;
					uint64_t nvb = g_pl + 0x80100;

					memcpy(dst, tri, sizeof(tri));
					memcpy(cpy, src, 32);
					cpy[4] = ((uint32_t)npos) | (src[4] & 0x3f);
					cpy[5] = (uint32_t)(npos >> 32);
					jw[20] = (uint32_t)npos;
					jw[21] = (uint32_t)(npos >> 32);
					jw[36] = (uint32_t)nvb;
					jw[37] = (uint32_t)(nvb >> 32);
					pc2("cpupos2: tiler pos 0x%llx vbuf 0x%llx "
					    "(orig pos 0x%llx vbuf 0x%llx pl 0x%llx)",
					    (unsigned long long)npos,
					    (unsigned long long)nvb,
					    (unsigned long long)pos,
					    (unsigned long long)vbuf,
					    (unsigned long long)g_pl);
				}
cpupos2_done:
				if (vmap)
					munmap(vmap, vsize);
				if (pmap)
					munmap(pmap, psize);
			}
			if (getenv("PC2_TILERAFTER2")) {
				g_tiler_jc = job;
				g_tiler_prim = jw[10];
				g_tiler_dep = jw[5];
				g_tiler_valid = 1;
			}
			if (getenv("PC2_TILERAFTER")) {
				g_tiler_jc = job;
				g_tiler_prim = jw[10];
				g_tiler_dep = jw[5];
				g_tiler_valid = 1;
				jw[10] = 0;	/* NONE: chain bins nothing */
				pc2("tafter: chain tiler jc=0x%llx prim 0x%x dep 0x%x disabled",
				    (unsigned long long)job, g_tiler_prim,
				    g_tiler_dep);
			}
			if (getenv("PC2_CPUPOS")) {
				/* Point only the tiler at a CPU-written
				 * full-screen triangle in scratch space of
				 * the position BO, with a copied varying
				 * buffer array; the VS keeps writing the
				 * original slot. */
				uint64_t vbuf = (uint64_t)jw[36] |
					((uint64_t)jw[37] << 32);
				uint64_t pos = (uint64_t)jw[20] |
					((uint64_t)jw[21] << 32);
				uint32_t ph, poff, psize = 0;
				uint32_t vh, voff, vsize = 0;
				void *pmap = NULL, *vmap = NULL;
				float tri[12] = {
					0.0f, 0.0f, 0.5f, 1.0f,
					240.0f, 0.0f, 0.5f, 1.0f,
					120.0f, 240.0f, 0.5f, 1.0f
				};
				int q;

				if (bo_find(pos, &ph, &poff) == 0 &&
				    bo_find(vbuf, &vh, &voff) == 0) {
					for (q = 0; q < nbos; q++) {
						if (bos[q].handle == ph)
							psize = bos[q].size;
						if (bos[q].handle == vh)
							vsize = bos[q].size;
					}
					pmap = bo_map_rw(ph, psize);
					vmap = bo_map_rw(vh, vsize);
				}
				if (pmap && vmap && poff + 0x140 <= psize &&
				    vsize >= 0x40) {
					uint32_t *dst = (uint32_t *)
						((char *)pmap + poff + 0x100);
					uint32_t *src = (uint32_t *)
						((char *)vmap + (voff & ~3u));
					uint32_t *copy = (uint32_t *)
						((char *)vmap + vsize - 0x40);
					uint64_t npos = pos + 0x100;
					uint64_t ngpu = (uint64_t)vh +
						(uint64_t)(vsize - 0x40);

					memcpy(dst, tri, sizeof(tri));
					memcpy(copy, src, 32);
					/* second descriptor entry: position slot */
					copy[4] = ((uint32_t)npos) |
						(src[4] & 0x3f);
					copy[5] = (uint32_t)(npos >> 32);
					jw[36] = (uint32_t)ngpu;
					jw[37] = (uint32_t)(ngpu >> 32);
					jw[20] = (uint32_t)npos;
					jw[21] = (uint32_t)(npos >> 32);
					pc2("cpupos: tiler pos 0x%llx (cpu) vbuf 0x%llx "
					    "copy 0x%llx", (unsigned long long)npos,
					    (unsigned long long)ngpu,
					    (unsigned long long)ngpu);
				} else {
					pc2("cpupos: setup failed p=%p v=%p "
					    "poff=0x%x psize=0x%x vsize=0x%x",
					    pmap, vmap, poff, psize, vsize);
				}
				if (pmap)
					munmap(pmap, psize);
				if (vmap)
					munmap(vmap, vsize);
			}
			if (getenv("PC2_ZEROPRIM")) {
				jw[10] = 0;
				pc2("zeroprim: primitive -> 0");
			}
		}
		if (!next)
			break;
		job = next;
	}
	munmap(map, size);
}

static void census(uint64_t pl, uint64_t pl_size, uint64_t body,
		   uint64_t heap_start, uint64_t heap_end)
{
	uint32_t h, off;
	uint32_t pro[16], w[2];
	uint64_t v;
	unsigned i, nz = 0, inrange = 0, first = ~0u, last = 0;
	unsigned nent = 1024;
	uint64_t body_off = body - pl;
	unsigned body_nz = 0;
	uint32_t bw[64];
	char map[1025];

	memset(map, 0, sizeof(map));
	if (bo_find(pl, &h, &off)) {
		pc2("pl 0x%llx: no BO handle (not in the created set)",
		    (unsigned long long)pl);
		return;
	}
	pc2("pl handle=%u gpu=0x%llx off=0x%x size=0x%llx body_off=0x%llx",
	    h, (unsigned long long)pl, off, (unsigned long long)pl_size,
	    (unsigned long long)body_off);

	if (rd32(h, off, &pro[0]) == 0) {
		for (i = 1; i < 16; i++) {
			if (rd32(h, off + i * 4, &pro[i]))
				break;
		}
		hexline("pl-prologue", pl, pro, 16);
	}

	for (i = 0; i < nent; i++) {
		if (rd32(h, off + 64 + i * 8, &w[0]) ||
		    rd32(h, off + 64 + i * 8 + 4, &w[1])) {
			pc2("entry %u: read failed", i);
			break;
		}
		v = (uint64_t)w[0] | ((uint64_t)w[1] << 32);
		if (v) {
			nz++;
			if (first == ~0u)
				first = i;
			last = i;
			if (nz <= 12) {
				uint32_t eh, eo;

				if (bo_find(v & 0xffffffffULL, &eh, &eo) == 0)
					pc2("nz entry[%u]=0x%016llx lo->h%u+0x%x",
					    i, (unsigned long long)v, eh, eo);
				else
					pc2("nz entry[%u]=0x%016llx lo: no BO",
					    i, (unsigned long long)v);
			}
		}
		if ((v & ~0xffULL) >= pl && (v & ~0xffULL) < pl + pl_size) {
			inrange++;
			if (inrange <= 12)
				pc2("inrange entry[%u]=0x%016llx", i,
				    (unsigned long long)v);
		}
		map[i] = ((v & ~0xffULL) >= pl &&
			  (v & ~0xffULL) < pl + pl_size) ? '#' :
			 (v ? '.' : '_');
	}
	map[nent] = 0;
	{
		uint32_t e[32];
		unsigned j;

		for (j = 0; j < 32; j++)
			if (rd32(h, off + 64 + j * 8, &e[j]))
				break;
		hexline("entries[0..7]", pl + 64, e, 8);
		hexline("entries[8..15]", pl + 96, e + 8, 8);
		if (j == 32) {
			uint32_t e2[32];

			for (j = 16; j < 32; j++) {
				if (rd32(h, off + 64 + j * 8 + 4, &e2[j - 16]))
					break;
			}
			hexline("entry-hi[0..15]", pl + 68, e2, 16);
		}
	}
	pc2("census nent=%u nz=%u inrange=%u first=%u last=%u",
	    nent, nz, inrange, first, last);
	if (nz && !inrange)
		pc2("census: every nonzero entry is OUTSIDE the polygon-list BO");
	for (i = 0; i < 32; i++)
		fprintf(stderr, "PC2 pcol row=%02u %.32s\n", i, &map[i * 32]);

	/* Body: first 64 words at polygon_list_body. */
	if (body_off + 512 <= pl_size && rd_words(body, bw, 64) == 0) {
		unsigned j;

		for (j = 0; j < 64; j++)
			if (bw[j])
				body_nz++;
		hexline("body", body, bw, 16);
		hexline("body+64B", body + 64, bw + 16, 16);
		hexline("body+128B", body + 128, bw + 32, 16);
		hexline("body+192B", body + 192, bw + 48, 16);
		pc2("body nonzero=%u/64", body_nz);
	} else {
		pc2("body at=0x%llx: unreadable/out of range",
		    (unsigned long long)body);
	}

	/* Full scan through a fresh read-only mapping of the same BO
	 * (MMAP_BO + mmap). On int36 the mapping holds page references;
	 * the 0xCF reads above remain the authority for the header. */
	{
		void *cpu = bo_map_ro(h, (size_t)pl_size);
		uint32_t ntiles = (uint32_t)((pl_size - body_off) / 0x200);
		unsigned t, covered = 0, total_nz = 0, shown = 0;

		if (!cpu) {
			pc2("mapscan: mmap of pl handle=%u failed: %s", h,
			    strerror(errno));
		} else {
			const uint32_t *hw = (const uint32_t *)
				((const char *)cpu + 64);
			pc2("pl phys: hdr=%llx body=%llx",
			    (unsigned long long)page_phys(cpu),
			    (unsigned long long)page_phys((char *)cpu + body_off));
			hexline("entries-map[0..15]", pl + 64, hw, 16);
			hexline("entries-map[32..47]", pl + 0x120, hw + 32, 16);
			hexline("prologue-map", pl, (const uint32_t *)cpu, 16);
			if (ntiles > 512)
				ntiles = 512;
			for (t = 0; t < ntiles; t++) {
				const uint32_t *tw = (const uint32_t *)
					((const char *)cpu + body_off + t * 0x200);
				unsigned j, nnz = 0;

				for (j = 0; j < 0x200 / 4; j++)
					if (tw[j])
						nnz++;
				total_nz += nnz;
				if (!nnz)
					continue;
				covered++;
				if (shown < 12) {
					shown++;
					fprintf(stderr, "PC2 bscan tile=%u nz=%u:",
						t, nnz);
					for (j = 0; j < 0x200 / 4 && j < 32; j++)
						fprintf(stderr, " %08x", tw[j]);
					fputc('\n', stderr);
				}
			}
			pc2("mapscan body_off=0x%llx tiles=%u covered=%u "
			    "nonzero_words=%u", (unsigned long long)body_off,
			    ntiles, covered, total_nz);
			/* Cross-check the first covered tile through 0xCF. */
			for (t = 0; t < ntiles; t++) {
				const uint32_t *tw = (const uint32_t *)
					((const char *)cpu + body_off + t * 0x200);
				uint32_t cw[16];
				unsigned j, nnz = 0;

				for (j = 0; j < 0x200 / 4; j++)
					if (tw[j])
						nnz++;
				if (!nnz)
					continue;
				if (rd_words(body + t * 0x200, cw, 16) == 0) {
					hexline("bscan-0xcf", body + t * 0x200,
						cw, 16);
					pc2("bscan-0xcf tile=%u matches-map=%d", t,
					    memcmp(cw, tw, 64) == 0);
				}
				break;
			}
			munmap(cpu, (size_t)pl_size);
			pc2("mapscan: unmapped");
		}
	}

	/* Tiler heap scratch: full scan through a mapping + first 64 words
	 * via 0xCF. */
	if (heap_start && heap_end > heap_start) {
		uint32_t hw[64];
		uint32_t hh, ho;
		unsigned j, hnz = 0;
		void *hmap = NULL;
		uint64_t hsize = heap_end - heap_start;

		if (bo_find(heap_start, &hh, &ho) == 0) {
			if (hsize > (16u << 20))
				hsize = 16u << 20;
			hmap = bo_map_ro(hh, (size_t)hsize);
		}
		if (rd_words(heap_start, hw, 64) == 0) {
			for (j = 0; j < 64; j++)
				if (hw[j])
					hnz++;
			hexline("heap", heap_start, hw, 16);
		}
		if (hmap) {
			const uint32_t *p = (const uint32_t *)hmap;

			pc2("heap phys=%llx",
			    (unsigned long long)page_phys(hmap));
			unsigned i, tot = 0, shown = 0;
			uint64_t lim = hsize / 4;

			for (i = 0; i < lim; i++) {
				if (!p[i])
					continue;
				tot++;
				if (shown < 16) {
					shown++;
					fprintf(stderr, "PC2 hscan off=0x%x:",
						i * 4);
					for (j = 0; j < 8 && i + j < lim; j++)
						fprintf(stderr, " %08x", p[i + j]);
					fputc('\n', stderr);
				}
			}
			pc2("heap-map range=0x%llx..0x%llx nonzero=%u/%llu",
			    (unsigned long long)heap_start,
			    (unsigned long long)heap_end, tot,
			    (unsigned long long)lim);
			munmap(hmap, (size_t)hsize);
		} else {
			pc2("heap range=0x%llx..0x%llx nonzero=%u/64 (map failed)",
			    (unsigned long long)heap_start,
			    (unsigned long long)heap_end, hnz);
		}
	}

}

static void decode_fragment(uint64_t jc, uint32_t out_sync)
{
	uint32_t h, off, hdr;
	uint64_t fbd, pl, pl_size, body, hs, he;
	uint32_t t[10];
	uint32_t pH[2];

	if (bo_find(jc, &h, &off)) {
		pc2("frag jc=0x%llx: no BO handle", (unsigned long long)jc);
		return;
	}
	if (rd32(h, off + 0x10, &hdr)) {
		pc2("frag jc=0x%llx: header read failed",
		    (unsigned long long)jc);
		return;
	}
	pc2("frag jc=0x%llx out_sync=%u type=%u idx=%u dep=%u/%u",
	    (unsigned long long)jc, out_sync, (hdr >> 1) & 0xf,
	    (hdr >> 16) & 0xff, 0, 0);
	if (((hdr >> 1) & 0xf) != 9) {
		pc2("submit jc is not a fragment job; no census");
		return;
	}
	if (rd64(h, off + 0x28, &fbd)) {
		pc2("fbd pointer unreadable");
		return;
	}
	pc2("fbd=0x%llx", (unsigned long long)fbd);
	if (rd_words(fbd + 192, t, 10)) {
		pc2("SFBD tiler section unreadable");
		return;
	}
	hexline("sfbd-tiler", fbd + 192, t, 10);
	pl_size = (uint64_t)t[0] | ((uint64_t)t[1] << 32);
	pl = (uint64_t)t[2] | ((uint64_t)t[3] << 32);
	body = (uint64_t)t[4] | ((uint64_t)t[5] << 32);
	hs = (uint64_t)t[6] | ((uint64_t)t[7] << 32);
	he = (uint64_t)t[8] | ((uint64_t)t[9] << 32);
	pc2("sfbd pls=0x%llx hier=0x%x pl=0x%llx body=0x%llx heap=0x%llx..0x%llx",
	    (unsigned long long)pl_size, t[1],
	    (unsigned long long)pl, (unsigned long long)body,
	    (unsigned long long)hs, (unsigned long long)he);

	/* Fragment payload bounds (tile units). */
	if (rd32(h, off + 0x20, &pH[0]) == 0 &&
	    rd32(h, off + 0x24, &pH[1]) == 0)
		pc2("frag bounds min=0x%08x max=0x%08x tiles x=%u..%u y=%u..%u",
		    pH[0], pH[1], pH[0] & 0xff, pH[1] & 0xff,
		    (pH[0] >> 16) & 0xff, (pH[1] >> 16) & 0xff);

	census(pl, pl_size, body, hs, he);

	/* The preceding chain submit is decoded by decode_chain(). */
}

static void decode_chain(void)
{
	uint64_t job = last_chain.jc;
	uint64_t tpos64 = 0, tstate64 = 0, vs_state64 = 0;
	unsigned hi;

	for (hi = 0; hi < last_chain.nbos; hi++) {
		int j;

		for (j = 0; j < nbos; j++) {
			if (bos[j].handle != last_chain.h[hi])
				continue;
			fprintf(stderr, "PC2 chain-bo h%u gpu=0x%llx size=0x%x\n",
				bos[j].handle, (unsigned long long)bos[j].gpu,
				bos[j].size);
		}
	}
	int k;

	for (k = 0; k < MAXCHAIN && job; k++) {
		uint32_t h, off, w[48];
		uint32_t type;

		if (bo_find(job, &h, &off)) {
			pc2("chain k=%d job=0x%llx: no BO handle", k,
			    (unsigned long long)job);
			break;
		}
		if (rd_words(job, w, 48)) {
			pc2("chain k=%d job=0x%llx: unreadable", k,
			    (unsigned long long)job);
			break;
		}
		type = (w[4] >> 1) & 0xf;
		pc2("chain k=%d job=0x%llx type=%u idx=%u next=0x%llx",
		    k, (unsigned long long)job, type, (w[4] >> 16) & 0xff,
		    (unsigned long long)((uint64_t)w[6] |
					 ((uint64_t)w[7] << 32)));
		if (raw_mode) {
			hexline("job", job, w, 8);
			hexline("job+32", job + 32, w + 8, 8);
			hexline("job+64", job + 64, w + 16, 8);
			hexline("job+96", job + 96, w + 24, 8);
			hexline("job+128", job + 128, w + 32, 8);
			hexline("job+160", job + 160, w + 40, 8);
		}
		if (type == 7 || type == 5) {
			uint64_t pos = (uint64_t)w[20] | ((uint64_t)w[21] << 32);
			uint64_t ubos = (uint64_t)w[22] | ((uint64_t)w[23] << 32);
			uint64_t tex = (uint64_t)w[24] | ((uint64_t)w[25] << 32);
			uint64_t samp = (uint64_t)w[26] | ((uint64_t)w[27] << 32);
			uint64_t push = (uint64_t)w[28] | ((uint64_t)w[29] << 32);
			uint64_t st = (uint64_t)w[30] | ((uint64_t)w[31] << 32);
			uint64_t abuf = (uint64_t)w[32] | ((uint64_t)w[33] << 32);
			uint64_t attr = (uint64_t)w[34] | ((uint64_t)w[35] << 32);
			uint64_t vbuf = (uint64_t)w[36] | ((uint64_t)w[37] << 32);
			uint64_t vary = (uint64_t)w[38] | ((uint64_t)w[39] << 32);
			uint64_t vp = (uint64_t)w[40] | ((uint64_t)w[41] << 32);
			uint32_t vw[8];

			pc2("%s draw: pos=0x%llx ubos=0x%llx tex=0x%llx samp=0x%llx "
			    "push=0x%llx state=0x%llx", type == 7 ? "tiler" : "vertex",
			    (unsigned long long)pos, (unsigned long long)ubos,
			    (unsigned long long)tex, (unsigned long long)samp,
			    (unsigned long long)push, (unsigned long long)st);
			pc2("%s draw: attr_bufs=0x%llx attrs=0x%llx vbuf=0x%llx "
			    "varyings=0x%llx viewport=0x%llx", type == 7 ? "tiler" : "vertex",
			    (unsigned long long)abuf, (unsigned long long)attr,
			    (unsigned long long)vbuf, (unsigned long long)vary,
			    (unsigned long long)vp);
			if (type == 7)
				pc2("tiler primitive-size @+184: %08x %08x",
				    w[46], w[47]);
			if (vp && rd_words(vp, vw, 8) == 0) {
				hexline("viewport", vp, vw, 8);
				pc2("viewport floats min=(%f,%f,%f) max=(%f,%f,%f) "
				    "scissor=(%u,%u)-(%u,%u)",
				    *(float *)&vw[0], *(float *)&vw[1],
				    *(float *)&vw[4], *(float *)&vw[2],
				    *(float *)&vw[3], *(float *)&vw[5],
				    vw[6] & 0xffff, vw[6] >> 16,
				    vw[7] & 0xffff, vw[7] >> 16);
			} else if (vp) {
				pc2("viewport at=0x%llx unreadable",
				    (unsigned long long)vp);
			}
			if (vbuf && rd_words(vbuf, vw, 8) == 0)
				hexline("varying-buffer", vbuf, vw, 8);
			if (vary && rd_words(vary, vw, 8) == 0)
				hexline("varyings", vary, vw, 8);
		}
		if (type == 7) {	/* tiler */
			uint32_t prim = w[10];
			tpos64 = (uint64_t)w[20] | ((uint64_t)w[21] << 32);
			tstate64 = (uint64_t)w[30] | ((uint64_t)w[31] << 32);
			pc2("primitive=0x%08x draw_mode=%u index_type=%u "
			    "index_enable=%u restart=%u jts=%u index_count=%u",
			    prim, prim & 0xff, (prim >> 8) & 0x7,
			    (prim >> 13) & 1, (prim >> 19) & 3,
			    (prim >> 26) & 0x3f, w[13]);
			pc2("tiler position=0x%llx state=0x%llx flags=0x%x",
			    (unsigned long long)tpos64,
			    (unsigned long long)tstate64, w[16]);
		}
		if (type == 5) {
			vs_state64 = (uint64_t)w[30] |
				     ((uint64_t)w[31] << 32);
			pc2("vertex pos_ptr=0x%llx attr=0x%llx state=0x%llx",
			    (unsigned long long)((uint64_t)w[22] |
						 ((uint64_t)w[23] << 32)),
			    (unsigned long long)((uint64_t)w[28] |
						 ((uint64_t)w[29] << 32)),
			    (unsigned long long)((uint64_t)w[30] |
						 ((uint64_t)w[31] << 32)));
		}
		job = (uint64_t)w[6] | ((uint64_t)w[7] << 32);
	}

	if (tpos64) {
		uint32_t pw[32];
		uint32_t ph, poff;
		void *pm = NULL;

		if (bo_find(tpos64, &ph, &poff) == 0) {
			uint32_t psize = bos[0].size;

			for (unsigned bi = 0; bi < (unsigned)nbos; bi++)
				if (bos[bi].handle == ph)
					psize = bos[bi].size;
			pm = bo_map_ro(ph, psize);
			if (pm)
				pc2("pos phys=%llx (iova=0x%llx)",
				    (unsigned long long)page_phys(pm),
				    (unsigned long long)tpos64);
		}
		if (rd_words(tpos64, pw, 32) == 0) {
			hexline("positions", tpos64, pw, 8);
			hexline("positions+32", tpos64 + 32, pw + 8, 8);
			hexline("positions+64", tpos64 + 64, pw + 16, 8);
			hexline("positions+96", tpos64 + 96, pw + 24, 8);
		} else
			pc2("positions at=0x%llx: unreadable",
			    (unsigned long long)tpos64);
		if (pm)
			munmap(pm, 0x10000);
	}
	dump_state(tstate64, "FS");
	dump_state(vs_state64, "VS");
}

struct pf_syncobj_create { uint32_t handle, flags; };
#define REQ_SYNCOBJ_CREATE _IOWR(DRM_IOCTL_BASE, 0xBF, struct pf_syncobj_create)
struct pf_syncobj_destroy { uint32_t handle, pad; };
#define REQ_SYNCOBJ_DESTROY _IOWR(DRM_IOCTL_BASE, 0xC0, struct pf_syncobj_destroy)

/* Run the chain's tiler job again as its own submit, after the frame's
 * fence: the vertex-shader positions are in DRAM by then. If the tiler
 * bins the same 3 vertices this time, the empty lists of the chained run
 * are an ordering/visibility effect, not a tiler config/format effect. */
static void run_tiler_after(void)
{
	struct pf_syncobj_create sc;
	struct pf_syncobj_destroy sd;
	struct pf_submit s;
	uint32_t h, off, size = 0, *jw;
	void *map;
	int j, rc;
	int64_t tmo;

	if (!g_tiler_valid || !g_tiler_jc)
		return;
	g_tiler_valid = 0;
	if (bo_find(g_tiler_jc, &h, &off))
		return;
	for (j = 0; j < nbos; j++)
		if (bos[j].handle == h)
			size = bos[j].size;
	if (!size)
		return;
	map = bo_map_rw(h, size);
	if (!map)
		return;
	jw = (uint32_t *)map + (off >> 2);
	jw[10] = g_tiler_prim;
	jw[5] = 0;
	munmap(map, size);

	memset(&sc, 0, sizeof(sc));
	if (real_ioctl(drv_fd, REQ_SYNCOBJ_CREATE, &sc)) {
		pc2("tafter: syncobj create failed: %s", strerror(errno));
		return;
	}
	memset(&s, 0, sizeof(s));
	s.jc = g_tiler_jc;
	s.bo_handles = (uint64_t)(uintptr_t)last_chain.h;
	s.bo_handle_count = last_chain.nbos;
	s.out_sync = sc.handle;
	rc = real_ioctl(drv_fd, REQ_SUBMIT, &s);
	pc2("tafter: standalone tiler submit rc=%d jc=0x%llx nbos=%u sync=%u",
	    rc ? -errno : 0, (unsigned long long)g_tiler_jc,
	    last_chain.nbos, sc.handle);
	tmo = now_ns() + 5000000000LL;
	if (!rc)
		rc = syncobj_wait(sc.handle, tmo);
	pc2("tafter: standalone tiler %s",
	    rc ? strerror(errno) : "completed");
	memset(&sd, 0, sizeof(sd));
	sd.handle = sc.handle;
	real_ioctl(drv_fd, REQ_SYNCOBJ_DESTROY, &sd);
}

static void on_submit(const struct pf_submit *s)
{
	static unsigned nsub, nchain, nfrag;

	nsub++;
	if (s->bo_handle_count > 64)
		return;
	if (!s->out_sync) {
		/* chain submit: remember its first job for later decode */
		last_chain.jc = s->jc;
		last_chain.nbos = s->bo_handle_count;
		if (s->bo_handles)
			memcpy(last_chain.h, (const void *)(uintptr_t)s->bo_handles,
			       s->bo_handle_count * 4);
		have_chain = 1;
		nchain++;
		return;
	}
	nfrag++;
	census_done++;
	if (!have_chain)
		pc2("frag submit without a preceding chain submit");
	if (census_done > census_max) {
		if (census_done == census_max + 1)
			pc2("further censuses suppressed (PC2_MAX=%u)",
			    census_max);
		return;
	}
	pc2("submit #%u: chain#%u frag#%u jc=0x%llx out_sync=%u nbos=%u",
	    nsub, nchain, nfrag, (unsigned long long)s->jc, s->out_sync,
	    s->bo_handle_count);
	{
		int64_t tmo = now_ns() + 5000000000LL;

		if (syncobj_wait(s->out_sync, tmo)) {
			pc2("out_sync %u wait failed: %s", s->out_sync,
			    strerror(errno));
			return;
		}
	}
	pc2("fence fired (out_sync=%u)", s->out_sync);
	if (getenv("PC2_TILERAFTER") || getenv("PC2_TILERAFTER2"))
		run_tiler_after();
	decode_fragment(s->jc, s->out_sync);
	if (have_chain)
		decode_chain();
}

int ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	void *arg;
	int r, nr;

	resolve();
	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (nr_fd_seen && fd == drv_fd && _IOC_TYPE(req) == DRM_IOCTL_BASE &&
	    _IOC_NR(req) == NR_SUBMIT && arg)
		patch_tiler_varyings(arg);
	r = real_ioctl(fd, req, arg);
	(void)0;
	if (r != 0 || _IOC_TYPE(req) != DRM_IOCTL_BASE)
		return r;

	nr = _IOC_NR(req);
	if (nr >= DRM_COMMAND_BASE && nr <= DRM_COMMAND_BASE + 5) {
		if (!nr_fd_seen) {
			const char *e = getenv("PC2_MAX");

			nr_fd_seen = 1;
			drv_fd = fd;
			if (e)
				census_max = (unsigned)atoi(e);
			raw_mode = getenv("PC2_RAW") != NULL;
			pc2("attached fd=%d PC2_MAX=%u raw=%d", fd,
			    census_max, raw_mode);
		}
		if (nr == NR_CREATE_BO && nbos < MAXBO) {
			struct pf_create_bo *c = arg;

			bos[nbos].handle = c->handle;
			bos[nbos].gpu = c->offset;
			bos[nbos].size = c->size;
			nbos++;
		} else if (nr == NR_MMAP_BO && nfakes < 256) {
			struct pf_mmap_bo *m = arg;

			fakes[nfakes].handle = m->handle;
			fakes[nfakes].fake = m->offset;
			nfakes++;
		} else if (nr == NR_SUBMIT && fd == drv_fd && arg) {
			on_submit(arg);
		}
	}
	return r;
}
