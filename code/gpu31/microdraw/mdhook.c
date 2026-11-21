/* mdhook.c - LD_PRELOAD hook for microdraw tiler forensics.
 * Intercepts drmIoctl(SUBMIT): walks the 32B job chain from the process's own
 * CPU maps, decodes headers/invocation/primitive/DRAW pointers/FBD tiler
 * section against Mesa 20.3 midgard_pack.h offsets, and records per-handle
 * iova/size + per-BO mmap cpu base so post-submit 0xCF reads can translate.
 * Log: /tmp/mdhook.log. Per-handle BO table: /tmp/mdhook.bos (text).
 * No GL/EGL calls; safe under LD_PRELOAD.
 */
#define _GNU_SOURCE
#include <stdarg.h>
 #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <drm/drm.h>
#include <drm/panfrost_drm.h>

static FILE *LF = 0;
static int (*real_ioctl)(int, unsigned long, ...) = 0;
static void *(*real_mmap)(void*, size_t, int, int, int, off_t) = 0;
struct md_create { uint32_t size; uint32_t flags; uint32_t handle; uint32_t pad; uint64_t offset; };
_Static_assert(sizeof(struct md_create) == 24, "md_create");
/* Mesa 20.3 speaks the pre-jm_ctx UAPI: SUBMIT is 40 B (0xc0286440), not the
 * 48 B new-UAPI number in /usr/include. Match type+nr, decode old layout. */
struct md_submit { uint64_t jc, in_syncs; uint32_t in_sync_count, out_sync; uint64_t bo_handles; uint32_t bo_handle_count, requirements; };
static int is_nr(unsigned long r, unsigned n) { return ((r & 0xffu) == n) && (((r >> 8) & 0xffu) == 0x64); }

/* handle -> iova/size */
#define MAXBO 512
static uint32_t bh[MAXBO]; static uint64_t biova[MAXBO]; static uint64_t bsz[MAXBO];
static int nbo = 0;
/* iova page -> cpu (from our own mmaps) */
#define MAXM 1024
static uint64_t miova[MAXM]; static void *mcpu[MAXM]; static size_t mlen[MAXM];
static int nm = 0;
/* Track mmap: Mesa mmaps GEM BOs then fills descriptors on CPU, so capture
 * MMAP_BO offset per handle and match plain-mmap offset argument. */
static uint64_t moff[MAXBO];
static void note_mmap_bo(uint32_t h, uint64_t off)
{
	int i;
	for (i = 0; i < nbo; i++)
		if (bh[i] == h) {
			moff[i] = off;
			return;
		}
}
static void note_mmap(uint64_t off, void *cpu, size_t len)
{
	int i;
	if (cpu == MAP_FAILED)
		return;
	for (i = 0; i < nbo; i++) {
		if (moff[i] == off && moff[i] != 0) {
			if (nm < MAXM && biova[i]) {
				miova[nm] = biova[i];
				mcpu[nm] = cpu;
				mlen[nm] = len;
				nm++;
			}
			return;
		}
	}
}

static void logline(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void logline(const char *fmt, ...) {
	if (!LF) { LF = fopen("/tmp/mdhook.log", "a"); if (!LF) return; setvbuf(LF, 0, _IONBF, 0); }
	va_list a; va_start(a, fmt); vfprintf(LF, fmt, a); va_end(a);
}
static void add_bo(uint32_t h, uint64_t iova, uint64_t sz) {
	for (int i = 0; i < nbo; i++) if (bh[i]==h) { biova[i]=iova; bsz[i]=sz; return; }
	if (nbo < MAXBO) { bh[nbo]=h; biova[nbo]=iova; bsz[nbo]=sz; nbo++; }
}
static uint64_t find_iova(uint32_t h) { for (int i=0;i<nbo;i++) if(bh[i]==h) return biova[i]; return 0; }
static const void *va2cpu(uint64_t va, uint64_t *off) {
	for (int i=0;i<nm;i++) if (va>=miova[i] && va<miova[i]+mlen[i]) { if(off)*off=va-miova[i]; return (char*)mcpu[i]+(va-miova[i]); }
	return 0;
}
static const char *jname(unsigned t) {
	switch(t){case 2:return "WRITE_VALUE";case 4:return "COMPUTE";case 5:return "VERTEX";case 7:return "TILER";case 9:return "FRAGMENT";default:return "?";}
}
/* FBD @fb: TILER section @+192 (10w), WEIGHTS @+256. prints pls/size/hier/pl/body/heap */
static void decode_fbd(uint64_t fb) {
	uint64_t o; const uint32_t *f = va2cpu(fb & ~63ull, &o);
	if (!f) { logline("  FBD 0x%llx UNMAPPED\n", (unsigned long long)fb); return; }
	const uint32_t *t = f + 192/4;
	uint32_t pls = t[0]; uint32_t hier = t[1];
	uint64_t pl = (uint64_t)t[2]|((uint64_t)t[3]<<32);
	uint64_t body = (uint64_t)t[4]|((uint64_t)t[5]<<32);
	uint64_t hs = (uint64_t)t[6]|((uint64_t)t[7]<<32);
	uint64_t he = (uint64_t)t[8]|((uint64_t)t[9]<<32);
	logline("  FBD@0x%llx TILER pls=0x%x hier=0x%x pl=0x%llx body=0x%llx heap=0x%llx-0x%llx bodyoff=0x%llx\n",
		(unsigned long long)fb, pls, hier, (unsigned long long)pl, (unsigned long long)body,
		(unsigned long long)hs, (unsigned long long)he, (unsigned long long)(body-pl));
	/* bounds/RT from PARAMETERS @+32: w8=bounds packed? print raw words +32..+64 */
	logline("  FBD PARAMS w8..15: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		f[8],f[9],f[10],f[11],f[12],f[13],f[14],f[15]);
	/* LOCAL_STORAGE @+0 */
	logline("  FBD LS w0..7: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		f[0],f[1],f[2],f[3],f[4],f[5],f[6],f[7]);
}
static void decode_draw(const uint32_t *d, const char *tag) {
	static const char *fn[] = {"state","attrbuf","attr","varybuf","vary","viewport","occl","tls/fbd"};
	uint64_t ptr[8];
	ptr[0]=(uint64_t)d[14]|((uint64_t)d[15]<<32);
	ptr[1]=(uint64_t)d[16]|((uint64_t)d[17]<<32);
	ptr[2]=(uint64_t)d[18]|((uint64_t)d[19]<<32);
	ptr[3]=(uint64_t)d[20]|((uint64_t)d[21]<<32);
	ptr[4]=(uint64_t)d[22]|((uint64_t)d[23]<<32);
	ptr[5]=(uint64_t)d[24]|((uint64_t)d[25]<<32);
	ptr[6]=(uint64_t)d[26]|((uint64_t)d[27]<<32);
	ptr[7]=(uint64_t)d[28]|((uint64_t)d[29]<<32);
	uint64_t pos=(uint64_t)d[4]|((uint64_t)d[5]<<32);
	uint64_t ubo=(uint64_t)d[6]|((uint64_t)d[7]<<32);
	uint64_t tex=(uint64_t)d[8]|((uint64_t)d[9]<<32);
	uint64_t smp=(uint64_t)d[10]|((uint64_t)d[11]<<32);
	uint64_t push=(uint64_t)d[12]|((uint64_t)d[13]<<32);
	logline("  %s DRAW w0=%08x pos=0x%llx ubo=0x%llx tex=0x%llx smp=0x%llx push=0x%llx\n",
		tag, d[0], (unsigned long long)pos, (unsigned long long)ubo, (unsigned long long)tex, (unsigned long long)smp, (unsigned long long)push);
	for (int i=0;i<8;i++) {
		uint64_t o; const uint32_t *p = ptr[i] ? va2cpu(ptr[i], &o) : 0;
		if (!ptr[i]) logline("    %s=NULL\n", fn[i]);
		else if (!p) logline("    %s=0x%llx UNMAPPED\n", fn[i], (unsigned long long)ptr[i]);
		else logline("    %s=0x%llx w0..3=%08x %08x %08x %08x\n", fn[i], (unsigned long long)ptr[i], p[0],p[1],p[2],p[3]);
	}
	/* viewport detail if mapped */
	if (ptr[5]) { const uint32_t *v = va2cpu(ptr[5], 0); if (v) logline("    viewport raw: %08x %08x %08x %08x %08x %08x\n", v[0],v[1],v[2],v[3],v[4],v[5]); }
}
static void walk_chain(uint64_t jc, unsigned nbos, uint32_t *handles, uint32_t req) {
	logline("SUBMIT jc=0x%llx req=0x%x nbos=%u handles:", (unsigned long long)jc, req, nbos);
	for (unsigned i=0;i<nbos && i<32;i++) logline(" %u=0x%llx", handles[i], (unsigned long long)find_iova(handles[i]));
	logline("\n");
	uint64_t cur = jc; int depth = 0;
	while (cur && depth < 8) {
		const uint32_t *w = va2cpu(cur, 0);
		if (!w) { logline(" job@0x%llx UNMAPPED\n", (unsigned long long)cur); break; }
		unsigned type = (w[4]>>1)&0x7f; unsigned idx = (w[4]>>16)&0xffff;
		unsigned d1 = w[5]&0xffff, d2 = (w[5]>>16)&0xffff;
		uint64_t next = (uint64_t)w[6]|((uint64_t)w[7]<<32);
		logline(" job@0x%llx type=%u(%s) idx=%u dep=%u,%u next=0x%llx\n",
			(unsigned long long)cur, type, jname(type), idx, d1, d2, (unsigned long long)next);
		if (type == 2) {
			uint64_t a=(uint64_t)w[8]|((uint64_t)w[9]<<32);
			logline("  WRITE_VALUE addr=0x%llx type=%u\n", (unsigned long long)a, w[10]);
		} else if (type == 5) {
			logline("  VERTEX invoc=%08x %08x params=%08x\n", w[8], w[9], w[10]);
			decode_draw(w+64/4, "VERTEX");
		} else if (type == 7) {
			logline("  TILER invoc=%08x %08x prim=%08x %08x %08x %08x\n", w[8], w[9], w[10], w[11], w[12], w[13]);
			unsigned dm=w[10]&0xff, jts=(w[10]>>26)&0x3f;
			logline("  TILER prim: draw_mode=%u jts=%u idxcount=%u\n", dm, jts, w[13]+1);
			decode_draw(w+64/4, "TILER");
		} else if (type == 9) {
			logline("  FRAG bounds (%u,%u)-(%u,%u) fbd=0x%llx\n", w[8]&0xfff, (w[8]>>16)&0xfff, w[9]&0xfff, (w[9]>>16)&0xfff,
				(unsigned long long)((uint64_t)(w[10]|((uint64_t)w[11]<<32)) & ~63ull));
			uint64_t fb=(uint64_t)w[10]|((uint64_t)w[11]<<32);
			decode_fbd(fb);
		} else if (type == 4) {
			logline("  COMPUTE invoc=%08x %08x params=%08x\n", w[8], w[9], w[10]);
			decode_draw(w+64/4, "COMPUTE");
		}
		cur = next; depth++;
	}
}

int ioctl(int fd, unsigned long req, ...) {
	if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	va_list a; va_start(a, req); void *arg = va_arg(a, void*); va_end(a);
	if (is_nr(req, 0x42)) {
		int r = real_ioctl(fd, req, arg);
		if (!r) { struct md_create *c = (void *)arg; add_bo(c->handle, c->offset, c->size); }
		return r;
	}
	if (is_nr(req, 0x45)) {
		int r = real_ioctl(fd, req, arg);
		if (!r) { struct drm_panfrost_get_bo_offset *g = arg;
			for (int i=0;i<nbo;i++) if(bh[i]==g->handle){biova[i]=g->offset;break;} }
		return r;
	}
	if (is_nr(req, 0x43)) {
		int r = real_ioctl(fd, req, arg);
		if (!r) {
			struct drm_panfrost_mmap_bo *m = arg;
			note_mmap_bo(m->handle, m->offset);
		}
		return r;
	}
	if (is_nr(req, 0x40)) {
		struct md_submit *s = (void *)arg;
		uint32_t h[64]; unsigned n = s->bo_handle_count > 64 ? 64 : s->bo_handle_count;
		if (s->bo_handles && n) memcpy(h, (void*)(uintptr_t)s->bo_handles, n*4);
		walk_chain(s->jc, s->bo_handle_count, h, s->requirements);
		FILE *bf = fopen("/tmp/mdhook.bos", "w");
		if (bf) { for(int i=0;i<nbo;i++) fprintf(bf, "%u 0x%llx 0x%llx\n", bh[i], (unsigned long long)biova[i], (unsigned long long)bsz[i]); fclose(bf); }
		return real_ioctl(fd, req, arg);
	}
	return real_ioctl(fd, req, arg);
}
int drmIoctl(int fd, unsigned long req, void *arg) {
	static int (*rd)(int, unsigned long, void*) = 0;
	if (!rd) rd = dlsym(RTLD_NEXT, "drmIoctl");
	if (is_nr(req, 0x42)) {
		int r = rd(fd, req, arg);
		if (!r) { struct md_create *c = (void *)arg; add_bo(c->handle, c->offset, c->size); }
		return r;
	}
	if (is_nr(req, 0x45)) {
		int r = rd(fd, req, arg);
		if (!r) { struct drm_panfrost_get_bo_offset *g = arg;
			for (int i = 0; i < nbo; i++) if (bh[i] == g->handle) { biova[i] = g->offset; break; } }
		return r;
	}
	if (is_nr(req, 0x43)) {
		int r = rd(fd, req, arg);
		if (!r) {
			struct drm_panfrost_mmap_bo *m = arg;
			note_mmap_bo(m->handle, m->offset);
		}
		return r;
	}
	if (is_nr(req, 0x40)) {
		struct md_submit *s = (void *)arg;
		uint32_t h[64]; unsigned n = s->bo_handle_count > 64 ? 64 : s->bo_handle_count;
		if (s->bo_handles && n) memcpy(h, (void*)(uintptr_t)s->bo_handles, n*4);
		walk_chain(s->jc, s->bo_handle_count, h, s->requirements);
		FILE *bf = fopen("/tmp/mdhook.bos", "w");
		if (bf) { for(int i=0;i<nbo;i++) fprintf(bf, "%u 0x%llx 0x%llx\n", bh[i], (unsigned long long)biova[i], (unsigned long long)bsz[i]); fclose(bf); }
		return rd(fd, req, arg);
	}
	return rd(fd, req, arg);
}
void *mmap(void *ad, size_t ln, int pr, int fl, int fd, off_t off) {
	if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
	void *r = real_mmap(ad, ln, pr, fl, fd, off);
	note_mmap((uint64_t)off, r, ln);
	return r;
}
__attribute__((destructor)) static void mdump(void){ if(LF) fclose(LF); }
