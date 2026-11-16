/* Redirect the fbdev shadow blit from 32to24 to the packed 32bpp copy.
 * fbdev picks shadowUpdate32to24 whenever shadow24 is set, but the screen
 * pixmap here is 32bpp: 32to24 writes 3 bytes per pixel and leaves the last
 * 25% of every row stale (the white band on the right edge). */
#include <dlfcn.h>
#include <X11/Xdefs.h>
#include <screenint.h>

typedef struct _shadowBuf *shadowBufPtr;
typedef void (*updateproc)(ScreenPtr, shadowBufPtr);

void
shadowUpdate32to24(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    static updateproc packed;
    if (!packed) {
        void *h = dlopen("libshadow.so", RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            h = dlopen("/usr/lib/xorg/modules/libshadow.so", RTLD_NOW | RTLD_GLOBAL);
        if (h)
            packed = (updateproc)dlsym(h, "shadowUpdatePacked");
    }
    if (packed) {
        packed(pScreen, pBuf);
        return;
    }
}
