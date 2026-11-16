/* Fix Xorg's fbdev rendering on this vendor kernel.
 *
 * 1) fbdev's shadow24 path installs shadowUpdate32to24 even though the screen
 *    pixmap is 32bpp; 32to24 writes 3 bytes per pixel and leaves the right
 *    25% of every row unwritten (white band).
 * 2) fbScreenInit() is called with 24bpp while the framebuffer is 32bpp, so
 *    every PutImage/fill writes a 3-byte stream into a 4-byte-stride pixmap
 *    (horizontal squash, R/B rotation).
 *
 * Both are fixed here: force 32bpp into fbScreenInit, and route the 32to24
 * shadow blit to the packed 32bpp copy.
 */
#include <dlfcn.h>
#include <X11/Xdefs.h>
#include <screenint.h>

typedef struct _shadowBuf *shadowBufPtr;
typedef void (*updateproc)(ScreenPtr, shadowBufPtr);
typedef Bool (*screeninitproc)(ScreenPtr, void *, int, int, int, int, int, int);

int fbScreenInit(ScreenPtr pScreen, void *pbits, int xsize, int ysize,
                 int dpix, int dpiy, int width, int bpp)
{
    static screeninitproc real;
    if (!real)
        real = (screeninitproc)dlsym(RTLD_NEXT, "fbScreenInit");
    if (!real)
        return 0;
    return real(pScreen, pbits, xsize, ysize, dpix, dpiy, width, 32);
}

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
    if (packed)
        packed(pScreen, pBuf);
}
