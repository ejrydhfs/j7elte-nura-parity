# int84: combined image — KMS card0 + panfrost renderD128, MODE_ID blobs, PRIME hooks

int83 had card0 but no render node and no PRIME capability; Mesa therefore had
no kmsro join.  int84 is the combined image.

## Series (in order)

1. int83 KMS series: `0104`–`0130`, `0133` (dead-owner lock release), `0134`
   (explicit page-flip state);
2. `patches/drm-kms/0135-kms-blob-props-and-crtc-mode-id.patch` — v4.2 blob
   property lifetime, CREATEPROPBLOB/DESTROYPROPBLOB (0xBD/0xBE), MODE_ID on
   every atomic CRTC + atomic MODE_ID → `crtc_state->mode`, ACTIVE set/get fix;
3. `patches/drm-backport/0001`–`0004` (dma-fence, dma-resv, drm_syncobj,
   drm_sched job-only shim); `0005`'s build wiring is **hand-applied**:
   `drm-y += drm_syncobj.o drm_sched.o` before `drm-$(CONFIG_COMPAT)` and
   `obj-$(CONFIG_DMA_SHARED_BUFFER) += dma-fence.o dma-resv.o` in
   `drivers/base/Makefile`;
4. `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch` with
   `--exclude=include/drm/drmP.h`; the two drmP.h pieces are inserted by hand:
   `#define DRIVER_RENDER_ONLY 0x10000` (0x8000 is `DRIVER_ATOMIC` in this
   tree) and `struct drm_minor *render;` after `*primary`;
5. `code/gpu31/driver/0001` + `0002` (panfrost7580 sources and build wiring;
   `.name = "panfrost"` from 2d19f91; platform driver name unchanged);
6. `patches/drm-kms/0136-kms-card0-prime-ioctl-hooks.patch` — card0
   `drm_driver` gains `.prime_handle_to_fd = drm_gem_prime_handle_to_fd` and
   `.prime_fd_to_handle = drm_gem_prime_fd_to_handle` next to the existing
   `.gem_prime_import/.gem_prime_export` (`DRM_CAP_PRIME` was 0x0 without them;
   kmsro's first step is `drmPrimeHandleToFD(card0, dumb_handle)`).

Not applied: `code/gpu31/dmabuf/0001` (drm-kms 0003 already carries the arm64
CMA attrs fix), `dmabuf/0002` and `iommu-dma/0001` (they target the pre-0200
`exynos7580_drm.c`; the two hooks are ported by 0136 instead).

Config: new `patches/drm-kms-coexist/gpu-pairing.fragment` =
`CONFIG_DRM_PANFROST7580=y` (built in, so the loadable set stays the two KMS
modules), added to the script's CONFIG_EXPECT closure.  `EXYNOS7580_DISPLAY_KMS=y`,
the vendor refusal, DRM_EXYNOS7580=m, MODULES/MODULE_UNLOAD and the lockup
cmdline are unchanged from int83.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int84` | 18191048 | `2fee6bef69a48af930660656bd5b1d14c5d59f25753ed364c6f562bda8328ae9` |
| `kernel-boot-int84.img` | 27330560 | `eec3c2a2f451eba4d68d9fda599be71b1a123cd901c2e0ad068713974c811b62` |
| `exynos-drm7580-int84.ko` | 1434352 | `8317703a256dcf3f8f89d51ea0c7356e44e67ad1230157b931909af2eb93cede` |
| `drm_kms_helper-int84.ko` | 1443520 | `0781cf9d849fca0f00310b8a323a459f73a89cc8a3655992ff2a7521bed1cb31` |

Build 5369 log lines, 0 errors.  The image grew ~115 KB (panfrost built in).

## Gates (all PASS)

int83 set plus the new combined gates:

* `CONFIG_DRM_PANFROST7580=y`; `p7580_drv_probe` and `p7580_drm_driver` in
  `vmlinux`; render driver `.name = "panfrost"`; `DRIVER_RENDER_ONLY 0x10000`
  and `struct drm_minor *render` in `drmP.h`; `drm_get_platform_dev` and the
  `(render-only)` init string present;
* blob props: `drm_property_create_blob`/`lookup_blob`,
  `drm_atomic_set_mode_for_crtc`/`drm_atomic_set_mode_prop_for_crtc`,
  `drm_mode_convert_umode` in `vmlinux`; `"MODE_ID"` created and
  `prop_mode_id` attached to CRTCs; CREATEPROPBLOB/DESTROYPROPBLOB ioctls;
  `drm_property_destroy_user_blobs(dev, file_priv)` in `drm_release`;
* PRIME: both hook lines in the KMS `drm_driver`, `drm_gem_prime_handle_to_fd`
  / `drm_gem_prime_fd_to_handle` in `vmlinux` (so `DRM_CAP_PRIME` = import |
  export = 0x3 at runtime);
* exactly two `.ko` (panfrost is `=y`), imports resolved, unwind-free
  relocations only, stock-dt boot, pstore + lockup cmdline unchanged.

## Device expectations (Main runs these)

* `ls /dev/dri` → `card0` + `renderD128`; `modetest -c` shows a MODE_ID
  property block on CRTC 18 (value 0 via OBJ_GETPROPERTIES is expected; the blob
  id is only via GETPROPBLOB);
* `mesaprobe --info` → `DUMB_BUFFER=1`, `PRIME=0x3`;
* `drmPrimeHandleToFD(card0, dumb_handle)` succeeds (0136);
* kmscube on mesa20-kmsro → panfrost renderer with the
  `exynos7580_gem_prime_import`/`fd_to_handle` traces (system Mesa 26 still
  needs drm 1.1 + PANFROST_CREATE_BO 6.x, separate);
* the int83 flip path (0134) still needs a clean-log run: the call trace seen
  inside `exynos7580_crtc_page_flip` during `drm_mode_page_flip_ioctl` should
  be captured then.

Log sha256s: build `923ea99f3bccfcec665d78830a2611e5676b99fd432b50b061738ff22a4100a6`, gates `205422424a50655ea75026a90bbfc6807cbc8ca2c8a3549becef30587b051f11`, evidence `e6d612a7ddf187c3859f978f4fabfa0a5fa777ed1be492075e1c9948dcc43726`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
