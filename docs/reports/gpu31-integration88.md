# integration88 — blob lifetime + fbdev teardown order + platform rename

int88 = int87 + the two rmmod/oops fixes + the panfrost platform-driver
rename.  All three changes are peer-owned patches already on main
(8c761af, 9fb8c9f, 505b0ab); this integration only composes and gates them.

## Series (delta over int87)

1. `patches/drm-kms/0138-kms-blob-lifetime-fix.patch` (9fb8c9f,
   sha256 `adb787dd…326c1d5`) — v4.2-style blob reference split: the device's
   `property_blob_list` now owns its own reference, `drm_property_create_blob()`
   returns two (list + caller), a new static
   `drm_property_unlist_blob_locked()` removes `head_global` and drops only the
   list reference (idempotent via `list_empty`), `drm_property_free_blob()`
   `list_del_init()`s both heads and runs once at kref 0 under blob_lock, and
   every teardown path (destroyblob, client close, EDID replacement,
   mode_config_cleanup, create-failure) unlists instead of unrefing the shared
   reference.
2. `patches/drm-kms/0139-kms-fbdev-teardown-order.patch` (505b0ab,
   sha256 `38b460ad…5dd4be08`) — `exynos7580_fbdev_fini()` clears the
   file-private fbdev and calls `drm_fbdev_cma_fini()` once; `->unload()`
   calls it after unregistering the vblank cb and **before**
   `drm_vblank_cleanup()`/`drm_mode_config_cleanup()`; the load `err_vblank`
   path calls it too; the devm action delegates to it and is a no-op after the
   explicit call.  Contract header gains the declaration in-tree and in
   `code/gpu31/kms/exynos_drm7580.h` (0201 untouched, cmp gate passes).
3. `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (8c761af) —
   `p7580_platform_driver.driver.name` and `MODULE_ALIAS` are `panfrost`;
   DT table (`arm,mali-t720`, `arm,mali`), device name `11400000.mali`, module
   name `panfrost7580.ko`, CONFIG_DRM_PANFROST7580, IRQ/fence/debugfs labels
   unchanged.

## Gates (all PASS)

int87 set, plus:

- **blob lifetime**: unlist helper present, >= 6 call sites, `kref_get` after
  `list_add_tail` in create_blob, both `list_del_init` in free_blob, and
  `drm_mode_config_cleanup()` unlists (does not unref) the drained list;
- **fbdev order**: `exynos7580_fbdev_fini` implemented + declared, devm
  release guarded by the handle check and delegates to it, and in
  `exynos7580_unload()` the fini call precedes `drm_mode_config_cleanup()`
  (awk line-order check); `nm` of the KMS `.ko` shows
  `T exynos7580_fbdev_fini`;
- **rename**: `.name = "panfrost"` inside `p7580_platform_driver`,
  `MODULE_ALIAS("platform:panfrost")`, both DT compatibles still present;
- carried: llseek slot proof (`dma_buf_fops=0xffffffc000b5ab08
  llseek_slot=dma_buf_llseek=0xffffffc0005d3120`), FMODE_LSEEK, DRM_RDWR,
  panfrost built-in, blobs/MODE_ID ioctls, PRIME, two .ko, imports resolved,
  stock-dt boot, pstore/lockup cmdline.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int88` | 18191112 | `87feb4f46f4cfe0b01892b3e8fc2b5a3d564eabf5273ef261a304f71c5317fc3` |
| `kernel-boot-int88.img` | 27330560 | `b2cfdcf722a2f67e959ee866e6be9b3f7f741c8624a6b095e9a59e33200c6335` |
| `exynos-drm7580-int88.ko` | — | `1701dac9f4a89fc6784169eedd486e558c66e87583b9765581bfd0c99e03630f` |
| `drm_kms_helper-int88.ko` | — | `cbd38c99c9894b30f46f356d66f03085cb1b3b20c5ff94d2f568e5ffcb9721f9` |

Build log `int88-kms-build.log`: 0 errors.  Log sha256s: build `1286c35080d7e46f0d57563b45129dace8323c51181e80ea4a03f9dfe9c9c041`, gates
`a73180a89dfc86aa7740310098764a02655da807a665ff4c62c59a869bcae666`, evidence `e1c9f618f418ef2b25cfc43e8e2d95da3f6939800ecec170e4d3318c6656dfa6`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Runtime acceptance (Main)

- modules load; `dmesg` has `Initialized panfrost 1.0.0 20261119 on minor 128
  (render-only)` and device prefixes `panfrost 11400000.mali:`;
- `grep -qx DRIVER=panfrost /sys/dev/char/226:128/device/uevent`;
  `/sys/bus/platform/drivers/panfrost` exists,
  `/sys/bus/platform/drivers/panfrost7580` does not;
- rmmod gate: client sets MODE_ID (or just fbdev mode), exits, then
  `rmmod exynos_drm7580` with no `drm_property_free_blob` /
  `Unable to handle kernel paging request` / `drm_fbdev_cma_fini` /
  `drm_framebuffer_cleanup` in the log; `/dev/fb0` gone; repeat
  insmod -> rmmod twice, also with a client killed without closing its fd;
- kmscube renders (Panfrost), `mesaprobe --gbm card0` Panfrost, kwin_wayland
  --drm with `/data/mesa20sel` survives 20 s (atomic-ams/planes/gpu/gl/output,
  no SIGSEGV), modetest still works.
