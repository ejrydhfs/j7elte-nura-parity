/*
 * kbase-probe — freestanding aarch64 Linux diagnostic for Samsung kbase (Mali-T720)
 *
 * Purpose: prove, on a running Nura/postmarketOS device, what the GPU kernel
 * interface actually is:
 *   - /dev/mali0 is a kbase misc device speaking ARM's UK ioctl ABI (not DRM)
 *   - the kbase UK version (handshake + KBASE_FUNC_GET_VERSION)
 *   - no DRM render/card node exists (Mesa Panfrost has nothing to bind to)
 *   - /dev/fb0 exists (fbdev display path; the only 3D route is software Mesa)
 *
 * No hardware access beyond open()/ioctl() of /dev/mali0 is performed; the two
 * calls used (version handshake, set_flags, get_version) do not submit jobs and
 * do not power up the GPU cores. It is safe to run on a live device as root.
 *
 * Built freestanding (no libc) so the same binary runs on aarch64 musl or glibc.
 *
 * ABI source of truth (Nura tree, DDK r15p0):
 *   drivers/gpu/arm/t7xx/r15p0/mali_uk.h          union uk_header, UK_FUNC_ID=512,
 *                                                 UKP_FUNC_ID_CHECK_VERSION=0,
 *                                                 struct uku_version_check_args
 *   drivers/gpu/arm/t7xx/r15p0/mali_kbase_uku.h   KBASE_FUNC_SET_FLAGS=512+18,
 *                                                 KBASE_FUNC_GET_VERSION=512+16,
 *                                                 struct kbase_uk_set_flags,
 *                                                 struct kbase_uk_get_ddk_version
 *   .../mali_kbase_core_linux.c                   kbase_ioctl(): only _IOC_SIZE(cmd)
 *                                                 is used; kbase_dispatch() requires
 *                                                 handshake -> SET_FLAGS -> rest.
 *
 * Exit status: 0 if the kbase version handshake succeeded, 1 otherwise.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;

/* ---- raw syscalls (aarch64 generic ABI; Linux >= 3.10 has all of them) ---- */
static long sc1(long n, long a)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
	return x0;
}

static long sc3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}

static long sc4(long n, long a, long b, long c, long d)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	__asm__ volatile("svc #0" : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

#define SYS_IOCTL    29
#define SYS_WRITE    64
#define SYS_OPENAT   56
#define SYS_CLOSE    57
#define SYS_EXIT     93

#define AT_FDCWD     (-100)
#define O_RDONLY     0
#define O_RDWR       2

static long sys_open(const char *path, long flags)
{
	return sc4(SYS_OPENAT, AT_FDCWD, (long)path, flags, 0);
}

static long sys_close(long fd)
{
	return sc1(SYS_CLOSE, fd);
}

/* ---- tiny output helpers ---- */
static u64 str_len(const char *s)
{
	u64 n = 0;

	while (s[n])
		n++;
	return n;
}

static void put(const char *s)
{
	sc3(SYS_WRITE, 1, (long)s, (long)str_len(s));
}

static void put_u64(u64 v)
{
	char buf[21];
	int i = 20;

	buf[20] = 0;
	do {
		buf[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v);
	put(&buf[i]);
}

static void put_dec(long v)
{
	if (v < 0) {
		put("-");
		put_u64((u64)(-v));
	} else {
		put_u64((u64)v);
	}
}

static void pad(const char *label)
{
	u64 n = str_len(label);

	put(label);
	while (n++ < 28)
		put(" ");
	put("= ");
}

/* ---- libc calls GCC may synthesise even in freestanding mode ---- */
void *memset(void *d, int c, unsigned long n)
{
	u8 *p = (u8 *)d;

	while (n--)
		*p++ = (u8)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	u8 *dp = (u8 *)d;
	const u8 *sp = (const u8 *)s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

/* ---- kbase UK ABI (r15p0 / UK 10.6) ---- */
struct uk_header {
	u32 id;
	u32 ret;
	u64 sizer;	/* keeps 64-bit alignment, must exist */
};

struct uku_version_check_args {
	struct uk_header header;
	u16 major;
	u16 minor;
	u8  padding[4];
};

struct kbase_uk_set_flags {
	struct uk_header header;
	u32 create_flags;
	u32 padding;
};

#define KBASE_GET_VERSION_BUFFER_SIZE 64
struct kbase_uk_get_ddk_version {
	struct uk_header header;
	char version_buffer[KBASE_GET_VERSION_BUFFER_SIZE];
	u32 version_string_size;
	u32 padding;
};

#define UKP_FUNC_ID_CHECK_VERSION 0
#define UK_FUNC_ID                512
#define KBASE_FUNC_GET_VERSION    (UK_FUNC_ID + 16)
#define KBASE_FUNC_SET_FLAGS      (UK_FUNC_ID + 18)

/* ARM ioctl encoding: the kbase driver only consumes _IOC_SIZE(cmd). */
#define IOC_NRBITS   8
#define IOC_TYPEBITS 8
#define IOC_SIZEBITS 14
#define IOC_DIRBITS  2
#define IOC_NRSHIFT   0
#define IOC_TYPESHIFT (IOC_NRSHIFT + IOC_NRBITS)
#define IOC_SIZESHIFT (IOC_TYPESHIFT + IOC_TYPEBITS)
#define IOC_DIRSHIFT  (IOC_SIZESHIFT + IOC_SIZEBITS)
#define IOC_NONE  0
#define IOC_WRITE 1
#define IOC_READ  2
#define IOC(dir, type, nr, size) \
	(((dir) << IOC_DIRSHIFT) | ((type) << IOC_TYPESHIFT) | \
	 ((nr) << IOC_NRSHIFT) | ((size) << IOC_SIZESHIFT))

#define MBASE_IOCTL(len) \
	IOC(IOC_READ | IOC_WRITE, 'k', 0, (unsigned long)(len))

static int probe_device(const char *path, long flags)
{
	long fd = sys_open(path, flags);

	pad(path);
	put_dec(fd);
	if (fd < 0)
		put(" (open failed)");
	put("\n");
	if (fd >= 0)
		sys_close(fd);
	return (int)fd;
}

void _start(void)
{
	struct uku_version_check_args vc;
	struct kbase_uk_set_flags sf;
	struct kbase_uk_get_ddk_version gv;
	long fd, r;
	u32 i, n;
	int ok;

	put("kbase-probe: Mali kernel-interface probe (kbase vs DRM)\n");
	put("------------------------------------------------------\n");

	/* 1. Device nodes: one kbase misc node, no DRM, fbdev display. */
	probe_device("/dev/mali0", O_RDWR);
	probe_device("/dev/dri/renderD128", O_RDWR);
	probe_device("/dev/dri/card0", O_RDWR);
	probe_device("/dev/fb0", O_RDWR);
	put("\n");

	fd = sys_open("/dev/mali0", O_RDWR);
	if (fd < 0) {
		put("kbase-probe: /dev/mali0 not available; is the kbase driver "
		    "bound to 11400000.mali?\n");
		sc1(SYS_EXIT, 1);
	}

	/* 2. Version handshake: must be the first call on the context. */
	memset(&vc, 0, sizeof(vc));
	vc.header.id = UKP_FUNC_ID_CHECK_VERSION;
	vc.major = 10;
	vc.minor = 6;
	r = sc3(SYS_IOCTL, fd, MBASE_IOCTL(sizeof(vc)), (long)&vc);
	pad("handshake 10.6 -> (major.minor)");
	put_u64(vc.major);
	put(".");
	put_u64(vc.minor);
	put(", ret ");
	put_dec(r);
	put("\n");
	if (r < 0) {
		put("kbase-probe: handshake ioctl failed\n");
		sys_close(fd);
		sc1(SYS_EXIT, 1);
	}

	/* 3. SET_FLAGS: completes per-context setup (kbase_dispatch gate). */
	memset(&sf, 0, sizeof(sf));
	sf.header.id = KBASE_FUNC_SET_FLAGS;
	sf.create_flags = 0;	/* BASE_CONTEXT_CREATE_FLAG_NONE */
	r = sc3(SYS_IOCTL, fd, MBASE_IOCTL(sizeof(sf)), (long)&sf);
	pad("SET_FLAGS(0) ret");
	put_dec(r);
	put(", header.ret ");
	put_dec(sf.header.ret);
	put("\n");
	if (r < 0 || sf.header.ret != 0) {
		put("kbase-probe: context setup failed\n");
		sys_close(fd);
		sc1(SYS_EXIT, 1);
	}

	/* 4. GET_VERSION: kernel DDK release string. */
	memset(&gv, 0, sizeof(gv));
	gv.header.id = KBASE_FUNC_GET_VERSION;
	r = sc3(SYS_IOCTL, fd, MBASE_IOCTL(sizeof(gv)), (long)&gv);
	pad("GET_VERSION ret");
	put_dec(r);
	put(", header.ret ");
	put_dec(gv.header.ret);
	put("\n");
	pad("kernel DDK version");
	n = gv.version_string_size;
	if (n > sizeof(gv.version_buffer))
		n = sizeof(gv.version_buffer);
	for (i = 0; i < n && gv.version_buffer[i]; i++)
		;
	sc3(SYS_WRITE, 1, (long)gv.version_buffer, (long)i);
	put("\n");

	sys_close(fd);

	/* 5. Verdict: kbase speaks, DRM does not exist. */
	ok = (r == 0 && gv.header.ret == 0);
	put("\nkbase-probe: kbase UK ABI present; ");
	{
		long rd = sys_open("/dev/dri/renderD128", O_RDWR);
		long cd = sys_open("/dev/dri/card0", O_RDWR);

		if (rd >= 0)
			sys_close(rd);
		if (cd >= 0)
			sys_close(cd);
		if (rd < 0 && cd < 0)
			put("no DRM node (Mesa Panfrost/GBM cannot bind)\n");
		else
			put("a DRM node exists (unexpected for this kernel config)\n");
	}
	put(ok ? "kbase-probe: OK\n" : "kbase-probe: FAILED\n");

	sc1(SYS_EXIT, ok ? 0 : 1);
}
