/*
 * mloader - minimal ELF32/ARM loader for the ROM's Mali EGL blob.
 *
 * Why this exists: the ROM's own bionic runtime (/system/bin/linker + libc)
 * deadlocks under qemu-user before reaching any entry point (see
 * parity/reports/gpu-libhybris.md §4), so "run it the Android way" is not
 * observable on this host.  This loader takes bionic out of the picture and
 * does the four things that matter, by hand:
 *
 *   1. map libGLES_mali.so at a fixed address, honouring PT_LOAD;
 *   2. apply its relocations, including the *packed* Android APS2 format
 *      (DT_ANDROID_REL / DT_ANDROID_RELSZ) that the ROM's linker emits;
 *   3. resolve its undefined bionic/C++/HAL imports against a shim layer, or
 *      against a per-symbol trap stub that names the symbol at first call;
 *   4. run its DT_INIT_ARRAY, then call eglGetDisplay() / eglQueryString()
 *      and print exactly what comes back.
 *
 * It is a probe, not a runtime: no gralloc, no hwcomposer, no /dev/mali0, no
 * GPU.  A NULL display here is a result, not a failure.
 *
 * Freestanding: raw ARM EABI syscalls, no libc, static, no interpreter.
 * Built and run by ../Makefile and ../run-mloader.sh.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long ulong;

/* ------------------------------------------------------------------ syscalls */
static inline long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long r7 __asm__("r7") = n;
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;
	__asm__ volatile("svc 0"
			 : "+r"(r0)
			 : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
			 : "memory");
	return r0;
}

#define SYS_exit        1
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_lseek       19
#define SYS_munmap      91
#define SYS_mmap2       192

#define O_RDONLY 0

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_PRIVATE   2
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

static void *sys_mmap(void *addr, ulong len, int prot, int flags, int fd, ulong off)
{
	return (void *)sc(SYS_mmap2, (long)addr, (long)len, prot, flags, fd, off);
}

/* ------------------------------------------------------------------- output */
static ulong s_len(const char *s) { ulong n = 0; while (s[n]) n++; return n; }

static void w_fd(int fd, const char *s, ulong n)
{
	while (n) {
		long r = sc(SYS_write, fd, (long)s, (long)n, 0, 0, 0);
		if (r <= 0)
			return;
		s += r;
		n -= (ulong)r;
	}
}
static void w(const char *s) { w_fd(1, s, s_len(s)); }
static void e(const char *s) { w_fd(2, s, s_len(s)); }

static char *put_hex(char *p, ulong v)
{
	int i;
	char b[11];
	b[0] = '0'; b[1] = 'x';
	for (i = 10; i >= 2; i--) {
		unsigned d = (unsigned)(v & 0xf);
		b[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v >>= 4;
	}
	for (i = 0; i < 11; i++) *p++ = b[i];
	return p;
}
static char *put_dec(char *p, long v)
{
	char b[24]; int i = 0, j;
	if (v < 0) { *p++ = '-'; v = -v; }
	do { b[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
	for (j = i - 1; j >= 0; j--) *p++ = b[j];
	return p;
}

/* ---------------------------------------------------------------- allocator
 * Small real allocator: 8-byte header (size | free-list link), first-fit with
 * reuse and coalescing-free realloc semantics.  A pure bump allocator is not
 * enough: the blob reallocs tables in a loop and expects the old contents to
 * survive the move. */
struct blk {
	ulong size;         /* payload size of this block */
	struct blk *next;   /* free-list link when free */
};

static u8 *heap_cur, *heap_end;
static struct blk *free_list;
static int g_heap_ready;

static void heap_init(void)
{
	ulong sz = 1024u * 1024 * 1024;
	void *p;

	if (g_heap_ready)
		return;
	g_heap_ready = 1;
	p = sys_mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((long)p < 0) {
		e("[mloader] FATAL: heap mmap failed\n");
		sc(SYS_exit, 127, 0, 0, 0, 0, 0);
	}
	heap_cur = (u8 *)p;
	heap_end = heap_cur + sz;
	w("[mloader] heap ");
	{ char b[24], *q = b; q = put_hex(q, (ulong)heap_cur); *q = 0; w(b); }
	w(" .. ");
	{ char b[24], *q = b; q = put_hex(q, (ulong)(heap_end - 1)); *q = 0; w(b); }
	w("\n");
}

#define ALIGN16(x) (((x) + 15UL) & ~15UL)
#define HDR 8

static void *halloc(ulong n)
{
	struct blk *b, **pp;

	if (!heap_cur)
		heap_init();
	n = ALIGN16(n ? n : 1);

	for (pp = &free_list; (b = *pp) != 0; pp = &b->next) {
		if (b->size >= n) {
			*pp = b->next;
			return (u8 *)b + HDR;
		}
	}
	if (heap_cur + n + HDR > heap_end)
		return 0;
	b = (struct blk *)heap_cur;
	heap_cur += n + HDR;
	b->size = n;
	b->next = 0;
	return (u8 *)b + HDR;
}

static void hfree(void *p)
{
	struct blk *b;

	if (!p)
		return;
	b = (struct blk *)((u8 *)p - HDR);
	b->next = free_list;
	free_list = b;
}

static ulong hsize(void *p)
{
	struct blk *b;

	if (!p)
		return 0;
	b = (struct blk *)((u8 *)p - HDR);
	return b->size;
}

/* ------------------------------------------------------- fault reporting
 * Installed with the raw rt_sigaction(2) ABI so a crash inside the blob names
 * its address and PC instead of just printing "signal 11".  Layouts are the
 * 32-bit ARM kernel ABI: struct sigaction = {handler, flags, restorer, mask[2]},
 * siginfo si_addr at +12, ucontext.uc_mcontext as struct sigcontext (pc at
 * sigcontext+72, which is ucontext+92). */
static u8 *g_base;              /* load bias of the blob (defined below) */

#define SYS_rt_sigaction 67
#define SA_SIGINFO 4

struct ksa {
	u32 handler;
	u32 flags;
	u32 restorer;
	u32 mask[2];
};

static void fault_handler(int sig, void *info, void *uc)
{
	u32 *si = (u32 *)info;
	u32 *c = (u32 *)uc;
	char b[24], *q;
	int i;

#define PUT_HEX(v) do { q = b; q = put_hex(q, (ulong)(v)); *q = 0; e(b); } while (0)
#define PUT_DEC(v) do { q = b; q = put_dec(q, (long)(v)); *q = 0; e(b); } while (0)

	e("\n[mloader] FAULT: signal "); PUT_DEC(sig);
	e(" info="); PUT_HEX(info);
	e(" uc="); PUT_HEX(uc);
	if ((ulong)si > 0x1000) {
		e(" si_signo="); PUT_DEC(si[0]);
		e(" si_code="); PUT_DEC(si[2]);
		e(" fault_addr="); PUT_HEX(si[3]);
	}
	if ((ulong)c > 0x1000) {
		e("\n[mloader] pc="); PUT_HEX(c[23]);
		e(" lr="); PUT_HEX(c[22]);
		e(" sp="); PUT_HEX(c[21]);
		e(" blob_pc="); PUT_HEX(c[23] - (u32)(ulong)g_base);
		e("\n[mloader] r0..r10:");
		for (i = 0; i < 11; i++) { e(" "); PUT_HEX(c[8 + i]); }
	}
	e("\n");
	sc(SYS_exit, 9, 0, 0, 0, 0, 0);
	for (;;) ;
}

static void install_fault_handlers(void)
{
	struct ksa sa;
	int sigs[4];
	int i;

	sa.handler = (u32)(ulong)fault_handler;
	sa.flags = SA_SIGINFO;
	sa.restorer = 0;
	sa.mask[0] = 0;
	sa.mask[1] = 0;
	sigs[0] = 11; /* SIGSEGV */
	sigs[1] = 7;  /* SIGBUS  */
	sigs[2] = 4;  /* SIGILL  */
	sigs[3] = 8;  /* SIGFPE  */
	for (i = 0; i < 4; i++)
		sc(SYS_rt_sigaction, sigs[i], (long)&sa, 0, 8, 0, 0);
}

/* ------------------------------------------------------------------- ELF32 */
struct ehdr {
	u8  ident[16];
	u16 type, machine;
	u32 version, entry, phoff, shoff, flags;
	u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
	u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};
struct dyn {
	i32 tag;
	u32 val;
};
struct sym {
	u32 name, value, size;
	u8  info, other;
	u16 shndx;
};

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_SYMTAB 6
#define DT_STRTAB 5
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_SYMENT 11
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELSZ 0x60000010
#define DT_RELCOUNT 0x6ffffffa

#define R_ARM_NONE 0
#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

#define ET_DYN 3

/* --------------------------------------------------------------- shims API */
struct shim { const char *name; void *addr; };

static void shim_trap_named(const char *name) __attribute__((noreturn));

static void shim_trap(void) __attribute__((noreturn));

/* ----------------------------------------------------------------- globals */
static u32 g_image_size;        /* mapped image size */
static u32 g_reloc_count;
static u32 g_unresolved;

/* name of the symbol a trap stub was made for, identified by stub address */
struct stub { u32 addr; const char *name; };
static struct stub g_stubs[512];
static u32 g_stub_count;
static u8 *g_stub_pool;         /* RWX: the stubs are executed */
#define STUB_SIZE 20

/* =============================================================== shim impls */
static int g_errno;
static int *shim___errno(void) { return &g_errno; }

static void *shim_memset(void *d, int c, ulong n);
static void *shim_memcpy(void *d, const void *s, ulong n);

static void *shim_malloc(ulong n) { return halloc(n ? n : 1); }
static void shim_free(void *p) { hfree(p); }
static void *shim_calloc(ulong a, ulong b)
{
	ulong n = a * b;
	void *p = halloc(n ? n : 1);
	if (p) shim_memset(p, 0, n);
	return p;
}
static void *shim_realloc(void *old, ulong n)
{
	void *p;
	ulong have;

	if (!old)
		return halloc(n ? n : 1);
	if (!n) {
		hfree(old);
		return 0;
	}
	have = hsize(old);
	if (have >= n)
		return old;
	p = halloc(n);
	if (!p)
		return 0;
	shim_memcpy(p, old, have);
	hfree(old);
	return p;
}
static void *shim_memalign(ulong al, ulong n) { (void)al; return halloc(n ? n : 1); }
static int shim_posix_memalign(void **out, ulong al, ulong n) { (void)al; *out = halloc(n ? n : 1); return *out ? 0 : 12; }
static ulong shim_malloc_usable_size(void *p) { return hsize(p); }

static void *shim_memset(void *d, int c, ulong n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
static void *shim_memcpy(void *d, const void *s, ulong n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
static void *shim_memmove(void *d, const void *s, ulong n) { u8 *a = d; const u8 *b = s; if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; } return d; }
static int shim_memcmp(const void *x, const void *y, ulong n) { const u8 *a = x, *b = y; while (n--) { if (*a != *b) return (int)*a - (int)*b; a++; b++; } return 0; }
static void *shim_memchr(const void *s, int c, ulong n) { const u8 *p = s; while (n--) { if (*p == (u8)c) return (void *)p; p++; } return 0; }

static ulong shim_strlen(const char *s) { const char *p = s; while (*p) p++; return (ulong)(p - s); }
static int shim_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (int)(u8)*a - (int)(u8)*b; }
static int shim_strncmp(const char *a, const char *b, ulong n) { while (n && *a && *a == *b) { a++; b++; n--; } if (!n) return 0; return (int)(u8)*a - (int)(u8)*b; }
static char *shim_strcpy(char *d, const char *s) { char *p = d; while ((*p++ = *s++)) { } return d; }
static char *shim_strncpy(char *d, const char *s, ulong n) { char *p = d; while (n && (*p++ = *s++)) n--; while (n--) *p++ = 0; return d; }
static char *shim_strchr(const char *s, int c) { while (*s) { if (*s == (char)c) return (char *)s; s++; } return c ? 0 : (char *)s; }
static char *shim_strrchr(const char *s, int c) { const char *r = 0; do { if (*s == (char)c) r = s; } while (*s++); return (char *)r; }
static char *shim_strdup(const char *s) { ulong n = shim_strlen(s) + 1; char *p = halloc(n); if (p) shim_memcpy(p, s, n); return p; }
static char *shim_strstr(const char *h, const char *n) { ulong l = shim_strlen(n); if (!l) return (char *)h; for (; *h; h++) if (!shim_strncmp(h, n, l)) return (char *)h; return 0; }
static char *shim_strsep(char **sp, const char *delim)
{
	char *s = *sp, *p;
	if (!s) return 0;
	for (p = s; *p; p++)
		for (const char *d = delim; *d; d++)
			if (*p == *d) { *p = 0; *sp = p + 1; return s; }
	*sp = 0;
	return s;
}

/* checked variants: bionic's __*_chk take (dst, dstlen, src, len) */
static void *shim_memcpy_chk(void *d, const void *s, ulong n, ulong dl) { (void)dl; return shim_memcpy(d, s, n); }
static void *shim_memset_chk(void *d, int c, ulong n, ulong dl) { (void)dl; return shim_memset(d, c, n); }
static void *shim_memchr_chk(const void *s, int c, ulong n, ulong sl) { (void)sl; return shim_memchr(s, c, n); }
static ulong shim_strlen_chk(const char *s, ulong sl) { (void)sl; return shim_strlen(s); }
static char *shim_strcpy_chk(char *d, const char *s, ulong dl) { (void)dl; return shim_strcpy(d, s); }
static char *shim_strncpy_chk(char *d, const char *s, ulong n, ulong dl) { (void)dl; return shim_strncpy(d, s, n); }
static char *shim_strcat_chk(char *d, const char *s, ulong dl)
{ (void)dl; char *p = d + shim_strlen(d); while ((*p++ = *s++)) { } return d; }

/* libc odds and ends */
static int shim_open(const char *path, int flags, int mode) { return (int)sc(SYS_open, (long)path, flags, mode, 0, 0, 0); }
static int shim_close(int fd) { return (int)sc(SYS_close, fd, 0, 0, 0, 0, 0); }
static long shim_read(int fd, void *b, ulong n) { return sc(SYS_read, fd, (long)b, (long)n, 0, 0, 0); }
static long shim_write(int fd, const void *b, ulong n) { return sc(SYS_write, fd, (long)b, (long)n, 0, 0, 0); }
static long shim_lseek(int fd, long off, int whence) { return sc(SYS_lseek, fd, off, whence, 0, 0, 0); }
static int shim_ioctl(int fd, unsigned long req, void *arg) { return (int)sc(54, fd, (long)req, (long)arg, 0, 0, 0); }
static void *shim_mmap(void *a, ulong l, int p, int f, int fd, long o) { return sys_mmap(a, l, p, f, fd, (ulong)(o / 4096)); }
static void *shim_mmap64(void *a, ulong l, int p, int f, int fd, long long o) { return sys_mmap(a, l, p, f, fd, (ulong)(o / 4096)); }
static int shim_munmap(void *a, ulong l) { return (int)sc(SYS_munmap, (long)a, (long)l, 0, 0, 0, 0); }
static int shim_getpid(void) { return (int)sc(20, 0, 0, 0, 0, 0, 0); }
static int shim_prctl(int op, ulong a, ulong b, ulong c, ulong d) { return (int)sc(172, op, (long)a, (long)b, (long)c, (long)d, 0); }
static char *shim_getenv(const char *n) { (void)n; return 0; }
static int shim_access(const char *p, int m) { (void)p; (void)m; return -1; }
static int shim_atrace_marker_fd = -1;
static u32 shim_atrace_enabled_tags = 0;
static int shim_atrace_is_ready(void) { return 0; }
static void shim_atrace_setup(void) { }
static void shim_atrace_begin_body(const char *n) { (void)n; }
static void shim_atrace_end_body(void) { }

static void shim_abort(void) __attribute__((noreturn));
static void shim_abort(void)
{
	e("[shim] abort() called by the blob\n");
	sc(SYS_exit, 134, 0, 0, 0, 0, 0);
	for (;;) ;
}
static void shim_stack_chk_fail(void) __attribute__((noreturn));
static void shim_stack_chk_fail(void) { e("[shim] __stack_chk_fail\n"); shim_abort(); }
static u32 shim_stack_guard = 0xdeadbeef;
static void shim_pure_virtual(void) __attribute__((noreturn));
static void shim_pure_virtual(void) { e("[shim] __cxa_pure_virtual\n"); shim_abort(); }

/* C++ runtime: operator new/delete, the only ones the blob imports */
static void *shim_op_new(u32 n) { return halloc(n ? n : 1); }
static void *shim_op_new_array(u32 n) { return halloc(n ? n : 1); }
static void shim_op_delete(void *p) { (void)p; }
static void shim_op_delete_array(void *p) { (void)p; }
static u8 shim_nothrow_obj;

static int shim_cxa_atexit(void (*f)(void *), void *a, void *d) { (void)f; (void)a; (void)d; return 0; }
static void shim_cxa_finalize(void *d) { (void)d; }
static int shim_aeabi_atexit(void *o, void (*f)(void *), void *d) { (void)o; (void)f; (void)d; return 0; }
static int shim_cxa_guard_acquire(u32 *g) { return *g ? 0 : 1; }
static void shim_cxa_guard_release(u32 *g) { *g = 1; }

/* pthread: single-threaded shims; lock state is kept so trylock is sane */
static int shim_pthread_mutex_init(void *m, const void *a) { (void)m; (void)a; return 0; }
static int shim_pthread_mutex_lock(void *m) { (void)m; return 0; }
static int shim_pthread_mutex_trylock(void *m) { (void)m; return 0; }
static int shim_pthread_mutex_unlock(void *m) { (void)m; return 0; }
static int shim_pthread_mutex_destroy(void *m) { (void)m; return 0; }
static int shim_pthread_mutexattr_init(void *a) { (void)a; return 0; }
static int shim_pthread_mutexattr_settype(void *a, int t) { (void)a; (void)t; return 0; }
static int shim_pthread_mutexattr_destroy(void *a) { (void)a; return 0; }
static int shim_pthread_cond_init(void *c, const void *a) { (void)c; (void)a; return 0; }
static int shim_pthread_cond_destroy(void *c) { (void)c; return 0; }
static int shim_pthread_cond_wait(void *c, void *m) { (void)c; (void)m; return 0; }
static int shim_pthread_cond_timedwait(void *c, void *m, const void *t) { (void)c; (void)m; (void)t; return 0; }
static int shim_pthread_cond_broadcast(void *c) { (void)c; return 0; }
static int shim_pthread_cond_signal(void *c) { (void)c; return 0; }
static int shim_pthread_condattr_init(void *a) { (void)a; return 0; }
static int shim_pthread_condattr_destroy(void *a) { (void)a; return 0; }
static int shim_pthread_attr_init(void *a) { (void)a; return 0; }
static int shim_pthread_attr_destroy(void *a) { (void)a; return 0; }
static int shim_pthread_attr_setstacksize(void *a, ulong s) { (void)a; (void)s; return 0; }
static unsigned long shim_pthread_self(void) { return 1; }
static int shim_pthread_key_create(void *k, void *d) { (void)k; (void)d; return 0; }
static int shim_pthread_key_delete(u32 k) { (void)k; return 0; }
static void *shim_pthread_getspecific(u32 k) { (void)k; return 0; }
static int shim_pthread_setspecific(u32 k, const void *v) { (void)k; (void)v; return 0; }
static int shim_pthread_create(void *t, const void *a, void *(*f)(void *), void *arg)
{ (void)t; (void)a; (void)f; (void)arg; return -1; }
static int shim_pthread_join(ulong t, void **r) { (void)t; (void)r; return -1; }
static int shim_pthread_rwlock_init(void *l, const void *a) { (void)l; (void)a; return 0; }
static int shim_pthread_rwlock_destroy(void *l) { (void)l; return 0; }
static int shim_pthread_rwlock_rdlock(void *l) { (void)l; return 0; }
static int shim_pthread_rwlock_wrlock(void *l) { (void)l; return 0; }
static int shim_pthread_rwlock_unlock(void *l) { (void)l; return 0; }
static int shim_sem_init(void *s, int p, unsigned v) { (void)s; (void)p; (void)v; return 0; }
static int shim_sem_destroy(void *s) { (void)s; return 0; }
static int shim_sem_wait(void *s) { (void)s; return 0; }
static int shim_sem_trywait(void *s) { (void)s; return 0; }
static int shim_sem_timedwait(void *s, const void *t) { (void)s; (void)t; return 0; }
static int shim_sem_post(void *s) { (void)s; return 0; }

/* ---- enough libc to let the blob's own init run ------------------------- */
#define SYS_time         13
#define SYS_gettimeofday 78
#define SYS_nanosleep    162
#define SYS_clock_gettime 263

struct timeval_s { long tv_sec, tv_usec; };
struct timespec_s { long tv_sec, tv_nsec; };

static int shim_gettimeofday(void *tv, void *tz)
{
	(void)tz;
	return (int)sc(SYS_gettimeofday, (long)tv, 0, 0, 0, 0, 0);
}
static int shim_clock_gettime(int clk, void *ts)
{
	return (int)sc(SYS_clock_gettime, clk, (long)ts, 0, 0, 0, 0);
}
static long shim_time(long *out)
{
	long t = sc(SYS_time, 0, 0, 0, 0, 0, 0);
	if (out) *out = t;
	return t;
}
static int shim_usleep(ulong us)
{
	struct timespec_s ts;
	ts.tv_sec = (long)(us / 1000000);
	ts.tv_nsec = (long)((us % 1000000) * 1000);
	return (int)sc(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
}

static long shim_sysconf(int name)
{
	switch (name) {
	case 0:  return 4096;   /* _SC_PAGESIZE */
	case 1:  return 65536;  /* _SC_PHYS_PAGES (in pages) */
	case 96: return 8;      /* _SC_NPROCESSORS_CONF */
	case 97: return 8;      /* _SC_NPROCESSORS_ONLN */
	default: return -1;
	}
}
static int shim_getrlimit(int res, ulong *lim)
{
	(void)res;
	if (lim) { lim[0] = 8u * 1024 * 1024; lim[1] = ~0UL; }
	return 0;
}
static int shim_fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return -1; }
static int shim_fstat(int fd, void *st) { (void)fd; (void)st; return -1; }
static int shim_stat(const char *p, void *st) { (void)p; (void)st; return -1; }
static void shim_perror(const char *s) { (void)s; }
static char *shim_strsignal(int s) { (void)s; return (char *)"signal"; }
static int shim_fileno(void *fp) { (void)fp; return -1; }
static int shim_fputc(int c, void *fp) { (void)c; (void)fp; return 0; }
static int shim_fputs(const char *s2, void *fp) { (void)s2; (void)fp; return 0; }
static ulong shim_fwrite(const void *p2, ulong a, ulong b, void *fp) { (void)p2; (void)a; (void)b; (void)fp; return 0; }
static ulong shim_fread(void *p2, ulong a, ulong b, void *fp) { (void)p2; (void)a; (void)b; (void)fp; return 0; }
static int shim_fclose(void *fp) { (void)fp; return 0; }
static int shim_fflush(void *fp) { (void)fp; return 0; }
static int shim_feof(void *fp) { (void)fp; return 0; }

static int shim_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int shim_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int shim_isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int shim_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static int shim_islower(int c) { return c >= 'a' && c <= 'z'; }
static int shim_isdigit(int c) { return c >= '0' && c <= '9'; }
static int shim_isalnum(int c) { return shim_isalpha(c) || shim_isdigit(c); }
static int shim_isprint(int c) { return c >= 0x20 && c < 0x7f; }
static int shim_isxdigit(int c) { return shim_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static long shim_strtol(const char *s, char **end, int base)
{
	long v = 0;
	int neg = 0;
	if (!s) return 0;
	while (*s == ' ' || *s == '\t') s++;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	if (base == 0) {
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
		else if (s[0] == '0') { base = 8; s++; }
		else base = 10;
	}
	for (;;) {
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
		else break;
		if (d >= base) break;
		v = v * base + d;
		s++;
	}
	if (end) *end = (char *)s;
	return neg ? -v : v;
}
static ulong shim_strtoul(const char *s, char **end, int base)
{
	return (ulong)shim_strtol(s, end, base);
}
static double shim_strtod(const char *s, char **end)
{
	double v = 0.0, frac = 1.0;
	int neg = 0;
	if (!s) { if (end) *end = (char *)s; return 0; }
	while (*s == ' ') s++;
	if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
	while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
	if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { frac /= 10; v += (*s - '0') * frac; s++; } }
	if (end) *end = (char *)s;
	return neg ? -v : v;
}
static int shim_atoi(const char *s) { return (int)shim_strtol(s, 0, 10); }
static double shim_atof(const char *s) { return shim_strtod(s, 0); }

static char *shim_strtok_r(char *str, const char *delim, char **save)
{
	char *start, *p;
	if (!str) str = *save;
	if (!str) return 0;
	while (*str) {
		int isd = 0;
		for (const char *d = delim; *d; d++) if (*str == *d) { isd = 1; break; }
		if (!isd) break;
		str++;
	}
	if (!*str) { *save = str; return 0; }
	start = str;
	for (p = str; *p; p++) {
		for (const char *d = delim; *d; d++) {
			if (*p == *d) { *p = 0; *save = p + 1; return start; }
		}
	}
	*save = p;
	return start;
}

static void shim_qsort(void *base, ulong n, ulong sz, int (*cmp)(const void *, const void *))
{
	u8 *b = base;
	u8 tmp[128];
	ulong i, j;

	if (sz == 0 || sz > sizeof(tmp))
		return;
	for (i = 1; i < n; i++) {
		shim_memcpy(tmp, b + i * sz, sz);
		for (j = i; j > 0 && cmp(b + (j - 1) * sz, tmp) > 0; j--)
			shim_memcpy(b + j * sz, b + (j - 1) * sz, sz);
		shim_memcpy(b + j * sz, tmp, sz);
	}
}

static u32 g_rand_state = 1;
static void shim_srand(u32 seed) { g_rand_state = seed ? seed : 1; }
static int shim_rand(void)
{
	g_rand_state = g_rand_state * 1103515245u + 12345u;
	return (int)((g_rand_state >> 16) & 0x7fff);
}

/* ---- tiny printf: enough for the blob's own diagnostics ---------------- */
typedef __builtin_va_list va_t;
#define VA_NEXT(ap) ((ulong)__builtin_va_arg(ap, ulong))

static void fmt_uint(char *out, ulong cap, ulong *pos, ulong u, int base, int width, char pad)
{
	char tmp[24];
	int n = 0;

	do {
		unsigned d = (unsigned)(u % (ulong)base);
		tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		u /= (ulong)base;
	} while (u);
	while (width > n) { if (*pos + 1 < cap) out[(*pos)++] = pad; width--; }
	while (n) {
		if (*pos + 1 < cap) out[(*pos)++] = tmp[--n];
		else n--;
	}
}

static int shim_format(char *buf, ulong cap, const char *fmt, va_t ap)
{
	ulong pos = 0;

	if (!buf || cap == 0)
		return 0;
	while (*fmt && pos + 1 < cap) {
		int width = 0, pad = ' ';
		if (*fmt != '%') { buf[pos++] = *fmt++; continue; }
		fmt++;
		if (*fmt == '0') { pad = '0'; fmt++; }
		while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
		switch (*fmt) {
		case 's': {
			const char *s2 = (const char *)VA_NEXT(ap);
			if (!s2) s2 = "(null)";
			while (*s2 && pos + 1 < cap) buf[pos++] = *s2++;
			break;
		}
		case 'd': case 'i': {
			long v = (long)(int)VA_NEXT(ap);
			ulong u;
			if (v < 0) { buf[pos++] = '-'; u = (ulong)(-v); } else u = (ulong)v;
			fmt_uint(buf, cap, &pos, u, 10, width, pad);
			break;
		}
		case 'u': fmt_uint(buf, cap, &pos, (u32)VA_NEXT(ap), 10, width, pad); break;
		case 'x': fmt_uint(buf, cap, &pos, (u32)VA_NEXT(ap), 16, width, pad); break;
		case 'p':
			buf[pos++] = '0';
			if (pos + 1 < cap) buf[pos++] = 'x';
			fmt_uint(buf, cap, &pos, (u32)VA_NEXT(ap), 16, 8, '0');
			break;
		case 'c': buf[pos++] = (char)(int)VA_NEXT(ap); break;
		case '%': buf[pos++] = '%'; break;
		case 'l':
			fmt++;
			if (*fmt == 'd') {
				long v = (long)VA_NEXT(ap);
				if (v < 0) { buf[pos++] = '-'; fmt_uint(buf, cap, &pos, (ulong)(-v), 10, width, pad); }
				else fmt_uint(buf, cap, &pos, (ulong)v, 10, width, pad);
			} else if (*fmt == 'u') fmt_uint(buf, cap, &pos, (u32)(ulong)VA_NEXT(ap), 10, width, pad);
			else if (*fmt == 'x') fmt_uint(buf, cap, &pos, (u32)(ulong)VA_NEXT(ap), 16, width, pad);
			break;
		default:
			(void)VA_NEXT(ap);
			break;
		}
		fmt++;
	}
	buf[pos] = 0;
	return (int)pos;
}

/* logging: print the blob's own tag + format text, args unexpanded */
/* logging: print the blob's own tag + format text, args unexpanded */
static int shim_log_print(int prio, const char *tag, const char *fmt, ...)
{
	(void)prio;
	e("[blob-log] "); e(tag ? tag : "?"); e(": "); e(fmt ? fmt : "?"); e("\n");
	return 0;
}
static int shim_log_write(int prio, const char *tag, const char *text)
{
	(void)prio;
	e("[blob-log] "); e(tag ? tag : "?"); e(": "); e(text ? text : "?"); e("\n");
	return 0;
}

/* Android HAL / ION: absent on purpose, and it says so */
static int g_hw_get_module_calls;
static int shim_hw_get_module(const char *id, const void *module)
{
	(void)module;
	if (g_hw_get_module_calls++ < 8) {
		e("[shim] hw_get_module(\""); e(id ? id : "?"); e("\") -> -5 (no HAL here)\n");
	}
	return -5;
}
static int shim_ion_open(void) { e("[shim] ion_open -> -1 (no /dev/ion)\n"); return -1; }
static int shim_ion_close(int fd) { (void)fd; return 0; }
static int shim_ion_alloc_fd(int fd, ulong len, ulong align, int heap, int *h)
{ (void)fd; (void)len; (void)align; (void)heap; (void)h; return -1; }

/* dl*: the blob may try to dlopen vendor add-ons; there are none */
static void *shim_dlopen(const char *p, int f) { (void)f; e("[shim] dlopen(\""); e(p ? p : "?"); e("\") -> NULL\n"); return 0; }
static void *shim_dlsym(void *h, const char *s) { (void)h; (void)s; return 0; }
static int shim_dlclose(void *h) { (void)h; return 0; }
static char *shim_dlerror(void) { return (char *)"mloader: no dynamic linker"; }

/* The blob asks its own loader where it is (dladdr) before patching code at
 * fixed offsets; returning an empty struct leaves it dereferencing NULL. */
struct dl_info {
	const char *dli_fname;
	void *dli_fbase;
	const char *dli_sname;
	void *dli_saddr;
};

static const char *g_blob_path;

static int shim_dladdr(const void *addr, struct dl_info *info)
{
	unsigned long a = (unsigned long)addr, b = (unsigned long)g_base;

	if (!info)
		return 0;
	if (g_base && a >= b && a < b + g_image_size) {
		info->dli_fname = g_blob_path;
		info->dli_fbase = g_base;
		info->dli_sname = 0;
		info->dli_saddr = 0;
		return 1;
	}
	info->dli_fname = 0;
	info->dli_fbase = 0;
	info->dli_sname = 0;
	info->dli_saddr = 0;
	return 0;
}

static int shim_snprintf(char *b, ulong n, const char *f, ...)
{
	va_t ap;
	int r;
	__builtin_va_start(ap, f);
	r = shim_format(b, n, f, ap);
	__builtin_va_end(ap);
	return r;
}
static int shim_vsnprintf(char *b, ulong n, const char *f, va_t ap)
{
	return shim_format(b, n, f, ap);
}
static int shim_printf(const char *f, ...)
{
	char tmp[1024];
	va_t ap;
	int r;
	__builtin_va_start(ap, f);
	r = shim_format(tmp, sizeof(tmp), f, ap);
	__builtin_va_end(ap);
	w_fd(1, tmp, (ulong)r);
	return r;
}
static int shim_fprintf(void *fp, const char *f, ...)
{
	char tmp[1024];
	va_t ap;
	int r;
	(void)fp;
	__builtin_va_start(ap, f);
	r = shim_format(tmp, sizeof(tmp), f, ap);
	__builtin_va_end(ap);
	w_fd(2, tmp, (ulong)r);
	return r;
}
static int shim_vfprintf(void *fp, const char *f, va_t ap)
{
	char tmp[1024];
	int r;
	(void)fp;
	r = shim_format(tmp, sizeof(tmp), f, ap);
	w_fd(2, tmp, (ulong)r);
	return r;
}
static int shim_puts(const char *s2)
{
	w_fd(1, s2, s_len(s2));
	w_fd(1, "\n", 1);
	return 0;
}
static int shim_sscanf(const char *s2, const char *f, ...) { (void)s2; (void)f; return 0; }
static char *shim_strerror(int e2) { (void)e2; return (char *)"error"; }

/* ------------------------------------------------------------- shim table */
static const struct shim g_shims[] = {
	{ "__errno", (void *)shim___errno },
	{ "malloc", (void *)shim_malloc },
	{ "free", (void *)shim_free },
	{ "calloc", (void *)shim_calloc },
	{ "realloc", (void *)shim_realloc },
	{ "memalign", (void *)shim_memalign },
	{ "posix_memalign", (void *)shim_posix_memalign },
	{ "malloc_usable_size", (void *)shim_malloc_usable_size },
	{ "memset", (void *)shim_memset },
	{ "memcpy", (void *)shim_memcpy },
	{ "memmove", (void *)shim_memmove },
	{ "memcmp", (void *)shim_memcmp },
	{ "memchr", (void *)shim_memchr },
	{ "__memcpy_chk", (void *)shim_memcpy_chk },
	{ "__memset_chk", (void *)shim_memset_chk },
	{ "__memchr_chk", (void *)shim_memchr_chk },
	{ "__strlen_chk", (void *)shim_strlen_chk },
	{ "__strcpy_chk", (void *)shim_strcpy_chk },
	{ "__strncpy_chk", (void *)shim_strncpy_chk },
	{ "__strncpy_chk2", (void *)shim_strncpy_chk },
	{ "__strcat_chk", (void *)shim_strcat_chk },
	{ "strlen", (void *)shim_strlen },
	{ "strcmp", (void *)shim_strcmp },
	{ "strncmp", (void *)shim_strncmp },
	{ "strcpy", (void *)shim_strcpy },
	{ "strncpy", (void *)shim_strncpy },
	{ "strchr", (void *)shim_strchr },
	{ "strrchr", (void *)shim_strrchr },
	{ "strdup", (void *)shim_strdup },
	{ "strstr", (void *)shim_strstr },
	{ "strsep", (void *)shim_strsep },
	{ "__aeabi_memcpy", (void *)shim_memcpy },
	{ "__aeabi_memcpy4", (void *)shim_memcpy },
	{ "__aeabi_memcpy8", (void *)shim_memcpy },
	{ "__aeabi_memmove", (void *)shim_memmove },
	{ "__aeabi_memmove4", (void *)shim_memmove },
	{ "__aeabi_memmove8", (void *)shim_memmove },
	{ "__aeabi_memset", (void *)shim_memset },
	{ "__aeabi_memset4", (void *)shim_memset },
	{ "__aeabi_memset8", (void *)shim_memset },
	{ "open", (void *)shim_open },
	{ "close", (void *)shim_close },
	{ "read", (void *)shim_read },
	{ "write", (void *)shim_write },
	{ "lseek", (void *)shim_lseek },
	{ "ioctl", (void *)shim_ioctl },
	{ "mmap", (void *)shim_mmap },
	{ "mmap64", (void *)shim_mmap64 },
	{ "munmap", (void *)shim_munmap },
	{ "getpid", (void *)shim_getpid },
	{ "prctl", (void *)shim_prctl },
	{ "getenv", (void *)shim_getenv },
	{ "access", (void *)shim_access },
	{ "atrace_is_ready", (void *)shim_atrace_is_ready },
	{ "atrace_setup", (void *)shim_atrace_setup },
	{ "atrace_begin_body", (void *)shim_atrace_begin_body },
	{ "atrace_end_body", (void *)shim_atrace_end_body },
	{ "atrace_enabled_tags", (void *)&shim_atrace_enabled_tags },
	{ "atrace_marker_fd", (void *)&shim_atrace_marker_fd },
	{ "abort", (void *)shim_abort },
	{ "__stack_chk_fail", (void *)shim_stack_chk_fail },
	{ "__stack_chk_guard", (void *)&shim_stack_guard },
	{ "__cxa_pure_virtual", (void *)shim_pure_virtual },
	{ "_Znwj", (void *)shim_op_new },
	{ "_ZnwjRKSt9nothrow_t", (void *)shim_op_new },
	{ "_Znaj", (void *)shim_op_new_array },
	{ "_ZdlPv", (void *)shim_op_delete },
	{ "_ZdaPv", (void *)shim_op_delete },
	{ "_ZSt7nothrow", (void *)&shim_nothrow_obj },
	{ "__cxa_atexit", (void *)shim_cxa_atexit },
	{ "__cxa_finalize", (void *)shim_cxa_finalize },
	{ "__aeabi_atexit", (void *)shim_aeabi_atexit },
	{ "__cxa_guard_acquire", (void *)shim_cxa_guard_acquire },
	{ "__cxa_guard_release", (void *)shim_cxa_guard_release },
	{ "pthread_mutex_init", (void *)shim_pthread_mutex_init },
	{ "pthread_mutex_lock", (void *)shim_pthread_mutex_lock },
	{ "pthread_mutex_trylock", (void *)shim_pthread_mutex_trylock },
	{ "pthread_mutex_unlock", (void *)shim_pthread_mutex_unlock },
	{ "pthread_mutex_destroy", (void *)shim_pthread_mutex_destroy },
	{ "pthread_mutexattr_init", (void *)shim_pthread_mutexattr_init },
	{ "pthread_mutexattr_settype", (void *)shim_pthread_mutexattr_settype },
	{ "pthread_mutexattr_destroy", (void *)shim_pthread_mutexattr_destroy },
	{ "pthread_cond_init", (void *)shim_pthread_cond_init },
	{ "pthread_cond_destroy", (void *)shim_pthread_cond_destroy },
	{ "pthread_cond_wait", (void *)shim_pthread_cond_wait },
	{ "pthread_cond_timedwait", (void *)shim_pthread_cond_timedwait },
	{ "pthread_cond_broadcast", (void *)shim_pthread_cond_broadcast },
	{ "pthread_cond_signal", (void *)shim_pthread_cond_signal },
	{ "pthread_condattr_init", (void *)shim_pthread_condattr_init },
	{ "pthread_condattr_destroy", (void *)shim_pthread_condattr_destroy },
	{ "pthread_attr_init", (void *)shim_pthread_attr_init },
	{ "pthread_attr_destroy", (void *)shim_pthread_attr_destroy },
	{ "pthread_attr_setstacksize", (void *)shim_pthread_attr_setstacksize },
	{ "pthread_self", (void *)shim_pthread_self },
	{ "pthread_key_create", (void *)shim_pthread_key_create },
	{ "pthread_key_delete", (void *)shim_pthread_key_delete },
	{ "pthread_getspecific", (void *)shim_pthread_getspecific },
	{ "pthread_setspecific", (void *)shim_pthread_setspecific },
	{ "pthread_create", (void *)shim_pthread_create },
	{ "pthread_join", (void *)shim_pthread_join },
	{ "pthread_rwlock_init", (void *)shim_pthread_rwlock_init },
	{ "pthread_rwlock_destroy", (void *)shim_pthread_rwlock_destroy },
	{ "pthread_rwlock_rdlock", (void *)shim_pthread_rwlock_rdlock },
	{ "pthread_rwlock_wrlock", (void *)shim_pthread_rwlock_wrlock },
	{ "pthread_rwlock_unlock", (void *)shim_pthread_rwlock_unlock },
	{ "sem_init", (void *)shim_sem_init },
	{ "sem_destroy", (void *)shim_sem_destroy },
	{ "sem_wait", (void *)shim_sem_wait },
	{ "sem_trywait", (void *)shim_sem_trywait },
	{ "sem_timedwait", (void *)shim_sem_timedwait },
	{ "sem_post", (void *)shim_sem_post },
	{ "__android_log_print", (void *)shim_log_print },
	{ "__android_log_write", (void *)shim_log_write },
	{ "hw_get_module", (void *)shim_hw_get_module },
	{ "ion_open", (void *)shim_ion_open },
	{ "ion_close", (void *)shim_ion_close },
	{ "ion_alloc_fd", (void *)shim_ion_alloc_fd },
	{ "dlopen", (void *)shim_dlopen },
	{ "dlsym", (void *)shim_dlsym },
	{ "dlclose", (void *)shim_dlclose },
	{ "dlerror", (void *)shim_dlerror },
	{ "dladdr", (void *)shim_dladdr },
	{ "snprintf", (void *)shim_snprintf },
	{ "sprintf", (void *)shim_snprintf },
	{ "__sprintf_chk", (void *)shim_snprintf },
	{ "__snprintf_chk", (void *)shim_snprintf },
	{ "printf", (void *)shim_printf },
	{ "fprintf", (void *)shim_fprintf },
	{ "vfprintf", (void *)shim_vfprintf },
	{ "puts", (void *)shim_puts },
	{ "fputc", (void *)shim_fputc },
	{ "gettimeofday", (void *)shim_gettimeofday },
	{ "clock_gettime", (void *)shim_clock_gettime },
	{ "time", (void *)shim_time },
	{ "usleep", (void *)shim_usleep },
	{ "sysconf", (void *)shim_sysconf },
	{ "getrlimit", (void *)shim_getrlimit },
	{ "fcntl", (void *)shim_fcntl },
	{ "fstat", (void *)shim_fstat },
	{ "lstat", (void *)shim_fstat },
	{ "stat", (void *)shim_stat },
	{ "perror", (void *)shim_perror },
	{ "strsignal", (void *)shim_strsignal },
	{ "tolower", (void *)shim_tolower },
	{ "toupper", (void *)shim_toupper },
	{ "isalpha", (void *)shim_isalpha },
	{ "isupper", (void *)shim_isupper },
	{ "islower", (void *)shim_islower },
	{ "isdigit", (void *)shim_isdigit },
	{ "isalnum", (void *)shim_isalnum },
	{ "isprint", (void *)shim_isprint },
	{ "isxdigit", (void *)shim_isxdigit },
	{ "strtol", (void *)shim_strtol },
	{ "strtoul", (void *)shim_strtoul },
	{ "strtod", (void *)shim_strtod },
	{ "atoi", (void *)shim_atoi },
	{ "atof", (void *)shim_atof },
	{ "strtok_r", (void *)shim_strtok_r },
	{ "qsort", (void *)shim_qsort },
	{ "srand", (void *)shim_srand },
	{ "rand", (void *)shim_rand },
	{ "fopen", (void *)shim_malloc },
	{ "fclose", (void *)shim_fclose },
	{ "fflush", (void *)shim_fflush },
	{ "fwrite", (void *)shim_fwrite },
	{ "fread", (void *)shim_fread },
	{ "feof", (void *)shim_feof },
	{ "fileno", (void *)shim_fileno },
	{ "fputs", (void *)shim_fputs },
	{ "strncat", (void *)shim_strcat_chk },
	{ "vsnprintf", (void *)shim_vsnprintf },
	{ "sscanf", (void *)shim_sscanf },
	{ "__gnu_strerror_r", (void *)shim_strerror },
};

static struct shim *shim_find(const char *name)
{
	u32 i;
	for (i = 0; i < sizeof(g_shims) / sizeof(g_shims[0]); i++)
		if (!shim_strcmp(g_shims[i].name, name))
			return (struct shim *)&g_shims[i];
	return 0;
}

/* trap stub: r12 = name pointer, then branch to shim_trap_named */
static void shim_trap_named(const char *name)
{
	e("\n[mloader] UNIMPLEMENTED IMPORT CALLED: ");
	e(name);
	e("\n[mloader] the blob reached a symbol this loader does not provide.\n");
	sc(SYS_exit, 3, 0, 0, 0, 0, 0);
	for (;;) ;
}

static void *make_trap_stub(const char *name)
{
	u8 *p;
	u32 *iw;

	if (!g_stub_pool) {
		g_stub_pool = sys_mmap(0, 512 * STUB_SIZE,
				       PROT_READ | PROT_WRITE | PROT_EXEC,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if ((long)g_stub_pool < 0) {
			e("[mloader] cannot map stub pool\n");
			sc(SYS_exit, 8, 0, 0, 0, 0, 0);
		}
		w("[mloader] trap stubs at ");
		{ char b[24], *q = b; q = put_hex(q, (ulong)g_stub_pool); *q = 0; w(b); }
		w("\n");
	}
	if (g_stub_count >= 512)
		return 0;

	p = g_stub_pool + g_stub_count * STUB_SIZE;
	iw = (u32 *)p;
	/* The pool is far (>32 MiB) from the loader, so an ARM 'b' cannot reach:
	 *   ldr r0,  [pc, #4]   ; r0 = name (first argument)
	 *   ldr pc,  [pc, #4]   ; pc  = trap handler (absolute)
	 *   .word name, .word handler */
	iw[0] = 0xe59f0004u;        /* ldr r0, [pc, #4]   -> p+12 */
	iw[1] = 0xe59ff004u;        /* ldr pc,  [pc, #4]  -> p+16 */
	iw[2] = 0xe1a00000u;        /* nop */
	iw[3] = (u32)(ulong)name;
	iw[4] = (u32)(ulong)shim_trap_named;

	g_stubs[g_stub_count].addr = (u32)(ulong)p;
	g_stubs[g_stub_count].name = name;
	g_stub_count++;
	return p;
}

/* ------------------------------------------------------------ symbol layer */
static struct {
	u8 *symtab, *strtab;
	u32 strsz, syment;
} g_dyn;

static struct sym *sym_at(u32 idx) { return (struct sym *)(g_dyn.symtab + idx * (g_dyn.syment ? g_dyn.syment : 16)); }
static const char *sym_name(u32 idx)
{
	u32 off = sym_at(idx)->name;
	if (off >= g_dyn.strsz) return "?";
	return (const char *)(g_dyn.strtab + off);
}

static const char *g_trapped_names[512];
static u32 g_trapped_n;

static u32 resolve(u32 idx, int *is_stub)
{
	struct sym *s = sym_at(idx);
	const char *n;
	struct shim *sh;

	*is_stub = 0;
	if (s->shndx != 0)
		return (u32)(ulong)(g_base + s->value);
	n = sym_name(idx);
	sh = shim_find(n);
	if (sh)
		return (u32)(ulong)sh->addr;
	*is_stub = 1;
	g_unresolved++;
	for (u32 i = 0; i < g_trapped_n; i++)
		if (!shim_strcmp(g_trapped_names[i], n))
			return (u32)(ulong)make_trap_stub(g_trapped_names[i]);
	if (g_trapped_n < 512)
		g_trapped_names[g_trapped_n++] = n;
	return (u32)(ulong)make_trap_stub(n);
}

static void apply_reloc(u32 off, u32 info)
{
	u32 type = info & 0xff, symidx = info >> 8;
	u32 *where;
	u32 val;
	int stub;

	if (off + 4 > g_image_size) {
		e("[mloader] reloc offset out of image: ");
		{ char b[24], *q = b; q = put_hex(q, off); *q = 0; e(b); }
		e(" type ");
		{ char b[24], *q = b; q = put_dec(q, (long)type); *q = 0; e(b); }
		e("\n");
		sc(SYS_exit, 7, 0, 0, 0, 0, 0);
	}
	where = (u32 *)(g_base + off);

	switch (type) {
	case R_ARM_NONE:
		return;
	case R_ARM_RELATIVE:
		*where += (u32)(ulong)g_base;
		break;
	case R_ARM_ABS32:
		val = resolve(symidx, &stub);
		*where += val;
		break;
	case R_ARM_GLOB_DAT:
	case R_ARM_JUMP_SLOT:
		val = resolve(symidx, &stub);
		if (stub && g_unresolved <= 64) {
			e("[mloader] trap stub: ");
			e(sym_name(symidx));
			e("\n");
		}
		*where = val;
		break;
	default:
		e("[mloader] unhandled reloc type ");
		{
			char b[24], *p = b;
			p = put_dec(p, (long)type);
			*p = 0;
			e(b);
			e(" at ");
			p = b; p = put_hex(p, off); *p = 0;
			e(b);
			e("\n");
		}
		sc(SYS_exit, 4, 0, 0, 0, 0, 0);
	}
	g_reloc_count++;
}

/* Signed LEB128 reader (bionic's sleb128_decoder).  Signedness matters: the
 * stream is sorted by relocation type, so offsets jump backwards at each
 * type boundary, and the delta is encoded as a negative SLEB128.  Verified
 * two ways: every decoded r_offset lands inside the mapped image, and the
 * count of type-23 (R_ARM_RELATIVE) entries equals DT_RELCOUNT exactly. */
static long leb128(const u8 **pp, const u8 *end)
{
	const u8 *p = *pp;
	unsigned long result = 0;
	unsigned shift = 0;
	u8 b;

	do {
		if (p >= end) { *pp = p; return 0; }
		b = *p++;
		result |= (unsigned long)(b & 0x7f) << shift;
		shift += 7;
	} while (b & 0x80);
	*pp = p;
	if (shift < 64 && (b & 0x40))
		result |= ~0UL << shift;
	return (long)result;
}

/* Android APS2 packed relocations (bionic linker_reloc_iterators.h) */
#define RG_BY_INFO   1
#define RG_BY_OFFSET 2
#define RG_BY_ADDEND 4
#define RG_HAS_ADDEND 8

static u32 g_rel, g_glob, g_abs;

static void apply_packed(const u8 *p, u32 sz)
{
	const u8 *end = p + sz;
	long num, off, idx = 0;
	u32 info = 0;
	u32 groups = 0;

	if (sz < 4 || p[0] != 'A' || p[1] != 'P' || p[2] != 'S' || p[3] != '2') {
		e("[mloader] packed reloc stream has no APS2 magic\n");
		sc(SYS_exit, 5, 0, 0, 0, 0, 0);
	}
	p += 4;
	num = leb128(&p, end);
	off = leb128(&p, end);
	(void)off;

	while (idx < num && p < end) {
		long group_size = leb128(&p, end);
		long flags = leb128(&p, end);
		long off_delta = 0;

		groups++;
		if (flags & RG_BY_OFFSET)
			off_delta = leb128(&p, end);
		if (flags & RG_BY_INFO)
			info = (u32)leb128(&p, end);
		if (flags & RG_HAS_ADDEND) {
			e("[mloader] unexpected r_addend in android.rel\n");
			sc(SYS_exit, 5, 0, 0, 0, 0, 0);
		}
		for (long i = 0; i < group_size; i++) {
			if (flags & RG_BY_OFFSET)
				off += off_delta;
			else
				off += leb128(&p, end);
			if (!(flags & RG_BY_INFO))
				info = (u32)leb128(&p, end);
			switch (info & 0xff) {
			case R_ARM_RELATIVE: g_rel++; break;
			case R_ARM_GLOB_DAT: g_glob++; break;
			case R_ARM_ABS32:    g_abs++; break;
			default: break;
			}
			apply_reloc((u32)off, info);
		}
		idx += group_size;
	}
	w("[mloader] packed relocs: ");
	{ char b[24], *q = b; q = put_dec(q, (long)idx); *q = 0; w(b); }
	w(" decoded (");
	{ char b[24], *q = b; q = put_dec(q, (long)groups); *q = 0; w(b); }
	w(" groups) = ");
	{ char b[24], *q = b; q = put_dec(q, (long)g_rel); *q = 0; w(b); }
	w(" RELATIVE + ");
	{ char b[24], *q = b; q = put_dec(q, (long)g_glob); *q = 0; w(b); }
	w(" GLOB_DAT + ");
	{ char b[24], *q = b; q = put_dec(q, (long)g_abs); *q = 0; w(b); }
	w(" ABS32\n");
	if (idx != num) {
		e("[mloader] packed reloc count mismatch\n");
		sc(SYS_exit, 6, 0, 0, 0, 0, 0);
	}
}

/* ------------------------------------------------------------------- loader */
#define LOAD_AT 0x20000000u

static u8 *g_file;
static ulong g_file_size;

static int read_file(const char *path)
{
	int fd = shim_open(path, O_RDONLY, 0);
	long sz;

	if (fd < 0) return -1;
	sz = shim_lseek(fd, 0, 2 /*SEEK_END*/);
	if (sz <= 0) { shim_close(fd); return -1; }
	shim_lseek(fd, 0, 0);
	g_file = sys_mmap(0, (ulong)sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if ((long)g_file < 0) { shim_close(fd); return -1; }
	g_file_size = (ulong)sz;
	shim_close(fd);
	return 0;
}

int main(int argc, char **argv);

static u32 load_blob(const char *path)
{
	struct ehdr *eh;
	u32 i, total = 0;

	if (read_file(path) < 0) {
		e("[mloader] cannot read "); e(path); e("\n");
		return 0;
	}
	eh = (struct ehdr *)g_file;
	if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[4] != 1 ||
	    eh->machine != 40 /*EM_ARM*/) {
		e("[mloader] not an ELF32 ARM object\n");
		return 0;
	}
	w("[mloader] blob: ELF32 EM_ARM, type ");
	{ char b[16], *p = b; p = put_dec(p, eh->type); *p = 0; w(b); }
	w(", ");
	{ char b[16], *p = b; p = put_dec(p, eh->phnum); *p = 0; w(b); }
	w(" program headers\n");

	for (i = 0; i < eh->phnum; i++) {
		struct phdr *ph = (struct phdr *)(g_file + eh->phoff + i * eh->phentsize);
		if (ph->type == PT_LOAD && ph->vaddr + ph->memsz > total)
			total = ph->vaddr + ph->memsz;
	}
	total = (total + 0xffffu) & ~0xffffu;
	g_image_size = total;
	g_base = sys_mmap((void *)LOAD_AT, total, PROT_READ | PROT_WRITE | PROT_EXEC,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if ((long)g_base < 0) {
		e("[mloader] fixed mmap failed\n");
		return 0;
	}
	w("[mloader] mapped "); 
	{ char b[24], *p = b; p = put_hex(p, (ulong)g_base); *p = 0; w(b); }
	w(" + "); 
	{ char b[24], *p = b; p = put_dec(p, (long)total); *p = 0; w(b); }
	w(" bytes\n");

	for (i = 0; i < eh->phnum; i++) {
		struct phdr *ph = (struct phdr *)(g_file + eh->phoff + i * eh->phentsize);
		if (ph->type != PT_LOAD) continue;
		shim_memcpy(g_base + ph->vaddr, g_file + ph->offset, ph->filesz);
		if (ph->memsz > ph->filesz)
			shim_memset(g_base + ph->vaddr + ph->filesz, 0, ph->memsz - ph->filesz);
	}

	/* dynamic section */
	struct dyn *d = 0;
	for (i = 0; i < eh->phnum; i++) {
		struct phdr *ph = (struct phdr *)(g_file + eh->phoff + i * eh->phentsize);
		if (ph->type == PT_DYNAMIC) d = (struct dyn *)(g_base + ph->vaddr);
	}
	if (!d) { e("[mloader] no PT_DYNAMIC\n"); return 0; }

	u32 dt_rel = 0, dt_relsz = 0, dt_jmprel = 0, dt_jmprelsz = 0;
	u32 dt_arel = 0, dt_arelsz = 0, dt_init_array = 0, dt_init_arraysz = 0;
	u32 dt_relcount = 0;

	for (; d->tag != DT_NULL; d++) {
		switch (d->tag) {
		case DT_SYMTAB: g_dyn.symtab = g_base + d->val; break;
		case DT_STRTAB: g_dyn.strtab = g_base + d->val; break;
		case DT_SYMENT: g_dyn.syment = d->val; break;
		case DT_REL: dt_rel = d->val; break;
		case DT_RELSZ: dt_relsz = d->val; break;
		case DT_JMPREL: dt_jmprel = d->val; break;
		case DT_PLTRELSZ: dt_jmprelsz = d->val; break;
		case DT_ANDROID_REL: dt_arel = d->val; break;
		case DT_ANDROID_RELSZ: dt_arelsz = d->val; break;
		case DT_INIT_ARRAY: dt_init_array = d->val; break;
		case DT_INIT_ARRAYSZ: dt_init_arraysz = d->val; break;
		case DT_RELCOUNT: dt_relcount = d->val; break;
		case 10 /*DT_STRSZ*/: g_dyn.strsz = d->val; break;
		default: break;
		}
	}
	if (!g_dyn.symtab || !g_dyn.strtab || !g_dyn.strsz) {
		e("[mloader] dynamic table incomplete\n");
		return 0;
	}

	/* plain .rel.dyn (usually absent with packed relocs) */
	if (dt_rel && dt_relsz) {
		u32 *r = (u32 *)(g_base + dt_rel);
		for (i = 0; i < dt_relsz / 8; i++)
			apply_reloc(r[2 * i], r[2 * i + 1]);
	}
	if (dt_arel && dt_arelsz) {
		w("[mloader] APS2 packed relocations: ");
		{ char b[24], *p = b; p = put_dec(p, (long)dt_arelsz); *p = 0; w(b); }
		w(" bytes, expecting ");
		{ char b[24], *p = b; p = put_dec(p, (long)dt_relcount); *p = 0; w(b); }
		w(" entries\n");
		apply_packed(g_base + dt_arel, dt_arelsz);
	}
	if (dt_jmprel && dt_jmprelsz) {
		u32 *r = (u32 *)(g_base + dt_jmprel);
		for (i = 0; i < dt_jmprelsz / 8; i++)
			apply_reloc(r[2 * i], r[2 * i + 1]);
	}

	w("[mloader] relocations applied: ");
	{ char b[24], *p = b; p = put_dec(p, (long)g_reloc_count); *p = 0; w(b); }
	w(" (DT_RELCOUNT said ");
	{ char b[24], *p = b; p = put_dec(p, (long)dt_relcount); *p = 0; w(b); }
	w(" for .rel.dyn)\n");
	w("[mloader] distinct trapped imports: ");
	{ char b[24], *p = b; p = put_dec(p, (long)g_trapped_n); *p = 0; w(b); }
	w("\n");

	/* constructors */
	if (dt_init_array && dt_init_arraysz) {
		u32 *a = (u32 *)(g_base + dt_init_array);
		u32 n = dt_init_arraysz / 4, k;
		w("[mloader] running ");
		{ char b[24], *p = b; p = put_dec(p, (long)n); *p = 0; w(b); }
		w(" constructor(s)\n");
		for (k = 0; k < n; k++) {
			void (*fn)(void) = (void (*)(void))(ulong)a[k];
			if (!fn) continue;
			e("[mloader]  ctor ");
			{ char b[24], *p = b; p = put_dec(p, (long)k); *p = 0; e(b); }
			e(" @ ");
			{ char b[24], *p = b; p = put_hex(p, (ulong)fn); *p = 0; e(b); }
			e("\n");
			fn();
		}
	}
	return 1;
}

/* find an exported symbol by name in the blob */
static u32 blob_symbol(const char *want)
{
	u32 i;
	for (i = 1; i < 200000; i++) {
		struct sym *s = sym_at(i);
		if (s->name == 0 || s->name >= g_dyn.strsz)
			break;
		if (s->shndx == 0)
			continue;
		if (!shim_strcmp((const char *)(g_dyn.strtab + s->name), want))
			return (u32)(ulong)(g_base + s->value);
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = "/vendor/lib/egl/libGLES_mali.so";
	typedef void *(*getdpy_t)(void *);
	typedef const char *(*qstr_t)(void *, u32);
	typedef int (*init_t)(void *, int *, int *);
	typedef int (*err_t)(void);
	getdpy_t p_get;
	qstr_t p_qstr;
	init_t p_init;
	err_t p_err;
	void *dpy;
	const char *s;
	int maj = -1, min = -1;

	w("mloader: hand-rolled ELF32/ARM loader for the ROM Mali blob\n");
	w("----------------------------------------------------------\n");
	if (argc > 1)
		path = argv[1];
	g_blob_path = path;
	/* --nofault: leave faults unhandled so an external debugger sees them */
	int nofault = 0;
	for (int ai = 1; ai < argc; ai++)
		if (!shim_strcmp(argv[ai], "--nofault"))
			nofault = 1;
	if (!nofault)
		install_fault_handlers();
	if (!load_blob(path)) {
		e("[mloader] load failed\n");
		return 1;
	}


	p_get = (getdpy_t)(ulong)blob_symbol("eglGetDisplay");
	p_qstr = (qstr_t)(ulong)blob_symbol("eglQueryString");
	p_init = (init_t)(ulong)blob_symbol("eglInitialize");
	p_err = (err_t)(ulong)blob_symbol("eglGetError");

	w("\n[mloader] eglGetDisplay     = ");
	{ char b[24], *p = b; p = put_hex(p, (ulong)p_get); *p = 0; w(b); }
	w("\n[mloader] eglQueryString    = ");
	{ char b[24], *p = b; p = put_hex(p, (ulong)p_qstr); *p = 0; w(b); }
	w("\n[mloader] eglInitialize     = ");
	{ char b[24], *p = b; p = put_hex(p, (ulong)p_init); *p = 0; w(b); }
	w("\n[mloader] eglGetError       = ");
	{ char b[24], *p = b; p = put_hex(p, (ulong)p_err); *p = 0; w(b); }
	w("\n");

	if (!p_get) {
		e("[mloader] eglGetDisplay not exported\n");
		return 1;
	}

	if (p_qstr) {
		s = p_qstr(0, 0x3055 /*EGL_EXTENSIONS*/);
		w("[mloader] eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) = ");
		w(s ? s : "(null)");
		w("\n");
	}

	dpy = p_get(0);
	w("[mloader] eglGetDisplay(EGL_DEFAULT_DISPLAY) = ");
	{ char b[24], *p = b; p = put_hex(p, (ulong)dpy); *p = 0; w(b); }
	w("\n");
	if (p_err) {
		int err = p_err();
		w("[mloader] eglGetError() = ");
		{ char b[24], *p = b; p = put_hex(p, (ulong)err); *p = 0; w(b); }
		w(err == 0x3000 ? " (EGL_SUCCESS)\n" : "\n");
	}
	if (!dpy) {
		w("[mloader] VERDICT: no display. The blob's EGL entry points ran, but\n"
		  "          display creation failed. See the shim log above for the\n"
		  "          exact HAL / kernel-interface calls it attempted.\n");
		return 2;
	}
	if (p_qstr) {
		s = p_qstr(dpy, 0x3053 /*EGL_VENDOR*/);
		w("[mloader] EGL_VENDOR  = "); w(s ? s : "(null)"); w("\n");
		s = p_qstr(dpy, 0x3054 /*EGL_VERSION*/);
		w("[mloader] EGL_VERSION = "); w(s ? s : "(null)"); w("\n");
	}
	if (p_init) {
		int ok = p_init(dpy, &maj, &min);
		w("[mloader] eglInitialize = ");
		w(ok ? "true\n" : "false\n");
		(void)maj; (void)min;
	}
	return 0;
}

__asm__(".globl _start\n"
	".type _start,%function\n"
	"_start:\n"
	"mov r0, sp\n"
	"b start_c\n");

void start_c(void *raw);

void start_c(void *raw)
{
	/* argc/argv straight off the kernel argument block */
	long *sp = (long *)raw;
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	int rc = main(argc, argv);
	sc(SYS_exit, rc, 0, 0, 0, 0, 0);
	for (;;) ;
}
