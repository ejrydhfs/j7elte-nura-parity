/*
 * eglprobe - 32-bit ARM (EABI5) bionic executable that dlopen()s the ROM's
 * Mali EGL/GLES blob and drives its EGL entry points.
 *
 * Purpose: prove, by execution rather than by inspection, exactly how far the
 * stock Android GL stack gets on a non-Android kernel (qemu-user here; the
 * same binary is what a 32-bit bionic chroot would run on the device:
 * CONFIG_COMPAT=y + CONFIG_ANDROID_BINDER_IPC/ASHMEM/ION).
 *
 * It is linked against the ROM's own bionic (/system/bin/linker, libc.so,
 * libdl.so) and uses no AArch64/musl code at all: every byte executed is
 * ELF32 EM_ARM from the Android 6 image or from this file (also EM_ARM).
 *
 * No exit status ever claims GPU success: success means "the blob loaded and
 * its EGL entry points returned", which is exactly what it prints.
 *
 * Built with: make            (clang -target armv7a-linux-gnueabi + ld.lld)
 */
typedef unsigned int u32;
typedef unsigned long ulong;

/* ---- bionic libc, declared by hand: no NDK headers/sysroot are available -- */
extern void *dlopen(const char *filename, int flag);
extern void *dlsym(void *handle, const char *symbol);
extern char *dlerror(void);
extern long write(int fd, const void *buf, ulong count);
extern void _exit(int status) __attribute__((noreturn));
extern void (*signal(int signum, void (*handler)(int)))(int);
extern void __libc_init(const void *raw_args, void (*onexit)(void),
			int (*slingshot)(int, char **, char **), const void *constors);

#define RTLD_LAZY 1
#define RTLD_NOW  2

/* ---- EGL 1.4 / 1.5 constants (no EGL headers available) ------------------ */
#define EGL_NO_DISPLAY      ((void *)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_SUCCESS         0x3000
#define EGL_NOT_INITIALIZED 0x3001
#define EGL_BAD_DISPLAY     0x3008
#define EGL_BAD_PARAMETER   0x300C
#define EGL_VENDOR          0x3053
#define EGL_VERSION         0x3054
#define EGL_EXTENSIONS      0x3055
#define EGL_CLIENT_APIS     0x308D

typedef void *EGLDisplay;
typedef u32 EGLint;
typedef EGLDisplay (*PFN_eglGetDisplay)(void *);
typedef const char *(*PFN_eglQueryString)(EGLDisplay, EGLint);
typedef unsigned int (*PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef unsigned int (*PFN_eglTerminate)(EGLDisplay);
typedef EGLint (*PFN_eglGetError)(void);
typedef void *(*PFN_eglGetProcAddress)(const char *);

/* ---- output helpers ------------------------------------------------------ */
static void out(const char *s)
{
	ulong n = 0;
	while (s[n])
		n++;
	write(1, s, n);
}

static void out_hex(ulong v)
{
	char b[11];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 10; i >= 2; i--) {
		unsigned int d = (unsigned int)(v & 0xf);
		b[i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		v >>= 4;
	}
	write(1, b, 11);
}

static void out_dec(long v)
{
	char b[12];
	int i = 11;

	b[i--] = '\n';
	if (v == 0)
		b[i--] = '0';
	while (v > 0) {
		b[i--] = (char)('0' + (v % 10));
		v /= 10;
	}
	write(1, b + i + 1, (ulong)(11 - i));
}

static void line(const char *a, const char *b)
{
	out(a);
	if (b)
		out(b);
	out("\n");
}

static void line_hex(const char *a, ulong v)
{
	out(a);
	out_hex(v);
	out("\n");
}

/* ---- crash reporting: a SIGSEGV here is a result, not a failure ---------- */
static void on_fault(int sig)
{
	(void)sig;
	out("\n[probe] FAULT: guest signal ");
	out_dec(sig);
	out("[probe] the blob (or a ROM library it pulled in) touched an address "
	    "that does not exist on this kernel.\n"
	    "[probe] see the qemu trace for the faulting PC; compare with the "
	    "blob base printed above.\n");
	_exit(3);
}

/* ---- the probe ----------------------------------------------------------- */
#define BLOB "/vendor/lib/egl/libGLES_mali.so"

int main(int argc, char **argv)
{
	void *h;
	char *err;
	PFN_eglGetDisplay p_get_display;
	PFN_eglQueryString p_query_string;
	PFN_eglInitialize p_initialize;
	PFN_eglTerminate p_terminate;
	PFN_eglGetError p_get_error;
	PFN_eglGetProcAddress p_get_proc;
	EGLDisplay dpy;
	const char *s;
	EGLint maj = -1, min = -1;
	unsigned int ok;

	(void)argc;
	(void)argv;

	line("[probe] 32-bit bionic userland alive: /system/bin/linker loaded this "
	     "EM_ARM binary", 0);

	signal(11 /* SIGSEGV */, on_fault);
	signal(7 /* SIGBUS  */, on_fault);
	signal(4 /* SIGILL  */, on_fault);

	line("[probe] dlopen(\"" BLOB "\", RTLD_LAZY) ...", 0);
	h = dlopen(BLOB, RTLD_LAZY);
	if (!h) {
		err = dlerror();
		line("[probe] dlopen FAILED: ", err ? err : "(no dlerror text)");
		return 1;
	}
	line_hex("[probe] dlopen OK, handle = ", (ulong)h);

	p_get_display     = (PFN_eglGetDisplay)dlsym(h, "eglGetDisplay");
	p_query_string    = (PFN_eglQueryString)dlsym(h, "eglQueryString");
	p_initialize      = (PFN_eglInitialize)dlsym(h, "eglInitialize");
	p_terminate       = (PFN_eglTerminate)dlsym(h, "eglTerminate");
	p_get_error       = (PFN_eglGetError)dlsym(h, "eglGetError");
	p_get_proc        = (PFN_eglGetProcAddress)dlsym(h, "eglGetProcAddress");

	line_hex("[probe] dlsym eglGetDisplay      = ", (ulong)p_get_display);
	line_hex("[probe] dlsym eglQueryString     = ", (ulong)p_query_string);
	line_hex("[probe] dlsym eglInitialize      = ", (ulong)p_initialize);
	line_hex("[probe] dlsym eglTerminate       = ", (ulong)p_terminate);
	line_hex("[probe] dlsym eglGetError        = ", (ulong)p_get_error);
	line_hex("[probe] dlsym eglGetProcAddress  = ", (ulong)p_get_proc);

	if (!p_get_display || !p_query_string) {
		line("[probe] blob is not an EGL implementation (no eglGetDisplay/"
		     "eglQueryString)", 0);
		return 1;
	}

	/* Client-level query, legal with EGL_NO_DISPLAY, needs no display at all. */
	s = p_query_string ? p_query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS) : 0;
	line("[probe] eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) = ",
	     s ? s : "(null)");

	line("[probe] eglGetDisplay(EGL_DEFAULT_DISPLAY) ...", 0);
	dpy = p_get_display(EGL_DEFAULT_DISPLAY);
	line_hex("[probe] eglGetDisplay -> ", (ulong)dpy);
	if (p_get_error) {
		EGLint e = p_get_error();
		out("[probe] eglGetError -> ");
		out_hex((ulong)e);
		out(e == EGL_SUCCESS ? " (EGL_SUCCESS)\n" : "\n");
	}
	if (!dpy) {
		line("[probe] VERDICT: eglGetDisplay returned EGL_NO_DISPLAY; the "
		     "blob refused to create a display on this kernel.", 0);
		return 2;
	}

	s = p_query_string(dpy, EGL_VENDOR);
	line("[probe] EGL_VENDOR   = ", s ? s : "(null)");
	s = p_query_string(dpy, EGL_VERSION);
	line("[probe] EGL_VERSION  = ", s ? s : "(null)");
	s = p_query_string(dpy, EGL_CLIENT_APIS);
	line("[probe] EGL_CLIENT_APIS = ", s ? s : "(null)");

	line("[probe] eglInitialize(dpy) ... (this is where gralloc/mali are "
	     "required)", 0);
	ok = p_initialize(dpy, &maj, &min);
	out("[probe] eglInitialize -> ");
	out(ok ? "EGL_TRUE" : "EGL_FALSE");
	if (ok) {
		out(", EGL "); out_dec(maj);
		out(".");     out_dec(min);
	} else {
		EGLint e = p_get_error ? p_get_error() : 0;
		out(", eglGetError "); out_hex((ulong)e);
		out(e == EGL_NOT_INITIALIZED ? " (EGL_NOT_INITIALIZED)\n" : "\n");
	}

	if (ok)
		p_terminate(dpy);

	line(ok ? "[probe] VERDICT: blob initialized an EGL display."
		: "[probe] VERDICT: blob loaded and its EGL query path works, but "
		  "display initialization failed on this kernel.",
	     0);
	return ok ? 0 : 2;
}

/* bionic crt entry: the linker zeroes the argument registers and passes the
 * kernel argument block on the stack (see bionic crtbegin_dynamic.S).
 *
 * Only the stack capture needs to be naked; the tail branch and the &main
 * argument are emitted by the C compiler, so they use PC-relative / GOT forms
 * and this file links as PIE (Android 6's linker rejects non-PIE). */
void start_c(void *raw_args);

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("mov r0, sp\n"
			 "b start_c\n");
}

static int slingshot(int argc, char **argv, char **envp)
{
	return main(argc, argv);
}

void start_c(void *raw_args)
{
	__libc_init(raw_args, (void (*)(void))0, slingshot, (const void *)0);
	__builtin_unreachable();
}
