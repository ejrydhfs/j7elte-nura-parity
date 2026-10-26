/*
 * egl-probe: load the ROM's Android 23 / ARM32 libGLES_mali.so into a bionic
 * process (under qemu-arm) and query EGL without any Mali kernel driver.
 *
 * Built freestanding against the ROM's own bionic (no NDK): the executable
 * declares only the symbols it uses and is linked directly against
 * /vendor/lib/egl/libGLES_mali.so, so the bionic linker loads the blob at
 * process start (no dlopen dependency).
 */
typedef unsigned int u32;
typedef int (*slingshot_t)(int, char**, char**);

/* bionic API 23 */
struct structors_array {
    void** preinit_array; unsigned preinit_array_count;
    void** init_array;    unsigned init_array_count;
    void** fini_array;    unsigned fini_array_count;
    void (*init_func)(void); void (*fini_func)(void);
};
extern void __libc_init(void* raw_args, void (*onexit)(void),
                        int (*slingshot)(int, char**, char**),
                        struct structors_array const* structors);
extern int printf(const char*, ...);
extern int puts(const char*);

/* EGL 1.4 subset, exactly the signatures in the blob's export table */
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef unsigned int EGLBoolean;
typedef int EGLint;

extern EGLDisplay eglGetDisplay(void* display_id);
extern EGLBoolean eglInitialize(EGLDisplay dpy, EGLint* major, EGLint* minor);
extern const char* eglQueryString(EGLDisplay dpy, EGLint name);
extern void* eglGetProcAddress(const char* name);
extern EGLBoolean eglTerminate(EGLDisplay dpy);
extern EGLint eglGetError(void);

#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_DISPLAY      ((void*)0)
#define EGL_VENDOR      0x3053
#define EGL_VERSION     0x3054
#define EGL_EXTENSIONS  0x3055
#define EGL_CLIENT_APIS 0x308D

struct structors_array g_structors = { 0 };

int main(int argc, char** argv, char** envp)
{
    EGLDisplay dpy;
    const char* s;
    (void)argc; (void)argv; (void)envp;

    printf("egl-probe: pid=%d, blob linked at process start\n", 0);

    /* Client-only query: valid with EGL_NO_DISPLAY, needs no driver instance. */
    s = eglQueryString(EGL_NO_DISPLAY, EGL_CLIENT_APIS);
    printf("eglQueryString(NO_DISPLAY, EGL_CLIENT_APIS) = %s\n", s ? s : "(null)");

    s = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("eglQueryString(NO_DISPLAY, EGL_EXTENSIONS) = %s\n", s ? s : "(null)");

    printf("eglGetProcAddress(eglGetDisplay) = %p (own symbol %p)\n",
           eglGetProcAddress("eglGetDisplay"), (void*)eglGetDisplay);
    printf("eglGetProcAddress(glGetString)  = %p\n",
           eglGetProcAddress("glGetString"));

    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay(EGL_DEFAULT_DISPLAY) = %p\n", dpy);

    if (dpy != EGL_NO_DISPLAY) {
        s = eglQueryString(dpy, EGL_VENDOR);
        printf("eglQueryString(dpy, EGL_VENDOR) = %s\n", s ? s : "(null)");
        s = eglQueryString(dpy, EGL_VERSION);
        printf("eglQueryString(dpy, EGL_VERSION) = %s\n", s ? s : "(null)");
        s = eglQueryString(dpy, EGL_EXTENSIONS);
        printf("eglQueryString(dpy, EGL_EXTENSIONS) = %.200s%s\n",
               s ? s : "(null)", (s && s[200]) ? "..." : "");

        /* This is the call that opens /dev/mali0 through kbase. */
        {
            EGLint maj = -1, min = -1;
            EGLBoolean ok = eglInitialize(dpy, &maj, &min);
            printf("eglInitialize -> %d (EGL %d.%d), eglGetError=0x%x\n",
                   (int)ok, maj, min, eglGetError());
            if (ok)
                printf("eglQueryString(dpy, EGL_VERSION) after init = %s\n",
                       eglQueryString(dpy, EGL_VERSION));
        }
    }
    printf("egl-probe: done (no GPU work submitted, no acceleration claimed)\n");
    return 0;
}

