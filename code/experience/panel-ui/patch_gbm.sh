#!/bin/sh
# Add the two libgbm entry points KWin 6.8 links against to Mesa 20.3.5.
set -e
B=/data/mesa203-build
SRC=$B/src
cd "$SRC"

python3 - <<'PY'
p = 'src/gbm/main/gbm.c'
s = open(p).read()
if 'gbm_bo_create_with_modifiers2' in s:
    print('already patched')
else:
    add = r'''
/* gpu31: KWin 6.8 (libkwin.so.6) links these two libgbm entry points.  Mesa
 * 20.3.5 predates them; the semantics KWin needs are the plain modifier
 * create and plane-0 fd export. */
GBM_EXPORT struct gbm_bo *
gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
                              uint32_t width, uint32_t height,
                              uint32_t format, const uint64_t *modifiers,
                              const unsigned int count, uint32_t flags)
{
   (void)flags;
   return gbm_bo_create_with_modifiers(gbm, width, height, format,
                                       modifiers, count);
}

GBM_EXPORT int
gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane)
{
   if (plane != 0)
      return -1;
   return gbm_bo_get_fd(bo);
}
'''
    s = s + add
    open(p, 'w').write(s)
    print('patched gbm.c')
PY

python3 - <<'PY2'
p = 'src/gbm/main/gbm.h'
s = open(p).read()
if 'gbm_bo_create_with_modifiers2' not in s:
    add = r'''
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *gbm,
                                             uint32_t width, uint32_t height,
                                             uint32_t format,
                                             const uint64_t *modifiers,
                                             const unsigned int count,
                                             uint32_t flags);
int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane);
'''
    open(p, 'w').write(s + add)
    print('patched gbm.h')
PY2

ninja -C "$B/build" src/gbm/libgbm.so.1.0.0 2>&1 | tail -5
ninja -C "$B/build" install 2>&1 | tail -3
nm -D /data/mesa203-install/lib/libgbm.so.1 | grep -E "gbm_bo_create_with_modifiers2|gbm_bo_get_fd_for_plane"
