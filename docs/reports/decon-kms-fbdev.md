# DECON KMS fbdev emulation (`/dev/fb0`) — M1 slice `KmsFbdev`

Status: **implemented; build-verified only** (no flash, no reboot — every runtime
statement below is an expectation derived from the 3.10 sources cited, not an
observation on the phone). Owner: `KmsFbdev`. Slice 13 of
`docs/reports/decon-kms-plan.md` §6.

Deliverable: `exynos_drm7580_fbdev.c` implementing `exynos7580_fbdev_init()`
from `code/gpu31/kms/exynos_drm7580.h`, plus the helper changes it needs.

---

## 1. Decision

`/dev/fb0` on the KMS image is a **`drm_fb_helper` fbdev with a CMA buffer**,
registered from the KMS probe:

```
/dev/fb0  ->  drm_fb_cma framebuffer  ->  drm_gem_cma_create() CMA object
              (drm_fb_cma_helper.c)       (drm_gem_cma_helper.c)
```

Chosen because:

* it is what `docs/reports/decon-kms-coexist.md` §2.4 and §4.3 item 3 already
  mandate (`drm_fbdev_cma_init`), and its `fb_ops` (`sys_fillrect/copyarea/
  imageblit`, `drm_fb_helper_check_var/set_par/blank/pan_display/setcmap`,
  `drm_fb_cma_helper.c:236-245`) are exactly what the Xorg fbdev DDX and the
  framebuffer console need;
* the buffer is physically contiguous, so `fix.smem_start` is valid and the
  generic `fb_mmap` (`drivers/video/fbmem.c`) can map it for X without a driver
  `fb_mmap` op — the same property the vendor fb0 has;
* no new cross-slice global is needed: the contract header stays as it is
  (`ex` in, `int` out), and the helper handle is file-private.

Not chosen: a private `drm_framebuffer` implementation. fbdev cannot reach
`exynos7580_fb_create()` (it needs a `struct drm_file` and a GEM handle that
only userspace has), and a private fb type would have added a second
"framebuffer kind" to the contract. The KMS fbdev therefore produces the only
fb type the core can also produce, and `KmsDmabuf`'s
`exynos7580_fb_scanout()` resolves it (see §4).

Geometry: the helper sizes the buffer from the mode the DSI connector reports,
i.e. the ea8061 panel mode — **720x1280, 32 bpp XRGB8888, pitch 2880**, so
`xres_virtual,yres_virtual = 720,1280`, the same virtual size as the vendor
`/dev/fb0` (`docs/reports/decon-kms-req.md` §1).

## 2. Deliverables

| File | Content |
|---|---|
| `code/gpu31/kms/exynos_drm7580_fbdev.c` | the slice (248 lines); install as `drivers/gpu/drm/exynos/exynos_drm7580_fbdev.c` |
| `patches/drm-kms/0001-core-drm-crtc-helper-atomic-aware-unused-function-teardown.patch` | core prerequisite (§3.1) |
| `patches/drm-kms/0002-core-drm-fb-helper-fix-id-drmfb.patch` | core prerequisite (§3.2) |
| `patches/drm-kms/0003-core-drm-gem-cma-helper-arm64-dma-attrs.patch` | core prerequisite (§3.3) |
| `patches/drm-kms/0004-kmsfbdev-decon-kms-fbdev-emulation.patch` | the slice: new `.c`, `exynos/Makefile`, `exynos/Kconfig` |

All four are `git apply --check` clean on the pristine
`android_kernel_samsung_universal7580@2a746c1b` tree, applied in order. They
touch only `drivers/gpu/drm/{drm_crtc_helper.c,drm_fb_helper.c,
drm_gem_cma_helper.c}` and `drivers/gpu/drm/exynos/{Kconfig,Makefile}` plus the
new file — **no file that the M0 atomic series modifies**, so "pristine" and
"pristine + M0" are the same patch base. No DT hunk, no new compatible, no
change to `include/uapi/drm`.

## 3. Why core helper changes are in this series

Three generic-core gaps block `drm_fbdev_cma_init()` on a 3.10 atomic driver.
Justification for (1) and (2) is that atomic CRTCs/encoders legitimately leave
the legacy callback set empty; (3) is an arm64 build fix in the helper that
was never enabled on arm64 in this tree.

### 3.1 `drm_helper_disable_unused_functions()` NULL-derefs on atomic objects

`drm_fbdev_cma_init()` calls it (`drm_fb_cma_helper.c:369`) before the first
modeset. In this tree the function does, unguarded:

* `drm_encoder_disable()`: `encoder->helper_private->disable` /
  `->dpms` (`drm_crtc_helper.c:254-263`) — an atomic encoder has no legacy
  callbacks, and before it is added it has no helper funcs at all;
* the CRTC loop: `crtc->helper_private->disable` / `->dpms`
  (`drm_crtc_helper.c:296-306`).

An atomic driver always takes the "unused" path at fbdev-init time
(`crtc->enabled` is 0, no encoder is attached to a connector yet), so this is
a guaranteed NULL dereference, not a corner case. The patch skips the legacy
teardown when the callbacks are absent and notes that an atomic driver's
output is torn down from `drm_atomic_helper_commit()` instead. Drivers that
still provide the legacy callbacks (the vendor `exynos_drm` CRTC, every other
in-tree user) take the identical code path as before.

### 3.2 `fix.id` is empty

`drm_fb_helper_fill_fix()` (`drm_fb_helper.c:981-1000`) never sets
`fb_info->fix.id`, so `/sys/class/graphics/fb0/name` would be an empty line and
the fb would not be identifiable as a DRM fb (`show_name()` returns
`fix.id`, `drivers/video/fbsysfs.c:494-499`). One line, `strcpy(info->fix.id,
"drmfb")`, matching later kernels.

### 3.3 `drm_gem_cma_helper.c` does not build on arm64

It calls `dma_alloc_writecombine()` / `dma_free_writecombine()`, which exist
only for 32 bit ARM (`arch/arm/include/asm/dma-mapping.h:195-210`). This is
why `CONFIG_DRM_KMS_CMA_HELPER` had never been enabled in this tree. The patch
switches to `DEFINE_DMA_ATTRS()` + `dma_set_attr(DMA_ATTR_WRITE_COMBINE)` with
`dma_alloc_attrs()` / `dma_free_attrs()` — literally what the 32 bit wrappers
expand to — and arm64 honours the attribute in `__get_dma_pgprot()`
(`arch/arm64/mm/dma-mapping.c:37-45`). It also fixes the two debugfs format
specifiers that the file's first arm64 build exposes
(`%08Zx` on a `dma_addr_t`, `%d` on a `size_t`).

Note for the assembler: `patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch`
(the superseded driver series) carries the same `drm_gem_cma_helper.c` hunk.
Keep one of the two, not both.

## 4. Contract dependencies (what the assembler must guarantee)

`exynos7580_fbdev_init()` runs last in the KMS probe and depends only on the
header, but it hard-fails (so the vendor fb0 comes back) unless:

1. **`ex->crtc.funcs->set_config == drm_atomic_helper_set_config`.**
   `drm_fbdev_cma_restore_mode()` → `drm_fb_helper_restore_fbdev_mode()` →
   `drm_mode_set_config_internal()` → `crtc->funcs->set_config(set)`
   (`drm_crtc.c:1906-1925`, `drm_fb_helper.c:285-300`). fb0 mode sets and pans
   go through the same op (`drm_fb_helper.c:770-840`). Checked with an explicit
   `-EINVAL` before anything is registered.
2. **`ex->crtc.primary` exists** — `drm_atomic_helper_set_config()` programs
   `crtc->primary` (`drm_atomic_helper.c`, M0 patch 0003), and the scanout
   verification reads `crtc->primary->fb`.
3. **`exynos7580_fb_scanout(fb, 0, &addr)` works for the CMA fb.** It is called
   *after* the first modeset, i.e. after the CRTC enabled the DECON iovmm
   domain, so a failure is a real mapping failure and not a "domain not up
   yet" artefact; `KmsDmabuf` maps the CMA object into that domain with
   `iovmm_map()` and caches it per `(fb, plane)` — the plane commit during the
   modeset already used it. The returned address is the **IOVA** used for
   scanout — `cma_obj->paddr` / `fix.smem_start` is a DMA-direct address and
   must never be programmed into a window.
4. `dev->mode_config.funcs->atomic_commit` is the helper commit whose tail is
   `exynos7580_commit_tail()` (KmsFence) — otherwise the initial modeset has
   nowhere to go.
5. The DSI connector reports the panel mode and a working
   `->fill_modes` (DsiEncoder). Without modes the helper falls back to
   1024x768 and this slice logs a warning (§7).

`main` only has to call `exynos7580_fbdev_init(ex)` and treat a non-zero return
as probe failure. It must **not** repeat the initial modeset: this function
already does `drm_fbdev_cma_restore_mode()` (= `drm_modeset_lock_all()` +
`drm_fb_helper_restore_fbdev_mode()` + `drm_modeset_unlock_all()`, the step
`decon-kms-coexist.md` §4.3 item 3 assigns to the driver) and verifies it. No
accessor/`weak` symbol is added — the contract header is unchanged.

The slice uses `ex->drm` (contract `8048d08`): 3.10's `drm_platform_init()`
owns the `drm_device`, our state hangs off `drm->dev_private` (hence
`to_exynos7580()` is `drm->dev_private`, never `container_of`), and
`ex->drm`/`ex->dev` are only borrowed pointers. `exynos7580_fbdev_init()`
rejects a missing `ex->drm`/`ex->drm->dev` with `-EINVAL` instead of
dereferencing them.

## 5. Optionality

Two switches, both off-switches; the default is **on**:

* `CONFIG_DRM_EXYNOS7580_FBDEV` (`drivers/gpu/drm/exynos/Kconfig`,
  `depends on DRM_EXYNOS7580`, `select DRM_KMS_CMA_HELPER`, `default y`).
  `select` pulls in `DRM_GEM_CMA_HELPER`, `FB_SYS_FILLRECT/COPYAREA/IMAGEBLIT`.
* `exynosdrm.fbdev=0` (`/sys/module/exynosdrm/parameters/fbdev`, 0444;
  built-in, so it is a build-time choice on this device because
  `CONFIG_CMDLINE_FORCE=y` discards the boot cmdline —
  `decon-kms-coexist.md` §2.2). `exynosdrm.fbdev_bpp` selects 32/24/16/15/8,
  default 32; anything else falls back to 32 with a `pr_warn`.

With the emulation off, `exynos7580_fbdev_init()` returns 0 and **no** fb0 is
created (the KMS image then has card0 only, and X11/nura-shell have no
framebuffer). That is a deliberate, documented configuration, not the default.

Memory: the buffer is `720 * 1280 * 4 = 3 686 400 B` (3.5 MiB), allocated with
`dma_alloc_attrs(..., DMA_ATTR_WRITE_COMBINE)` from the CMA area. The KMS image
must therefore have a CMA pool of at least ~4 MiB:
`CONFIG_CMA=y`, `CONFIG_DMA_CMA=y`, and `CONFIG_CMA_SIZE_MBYTES>=32`
(the current `config-samsung-j7elte.aarch64` has `CMA_SIZE_MBYTES=0`; the
`display-owner-kms.fragment` already sets 32). Without it the allocation falls
back to the page allocator, will fail for an order-10 block, and the probe
fails with `drm_fbdev_cma_init failed: -12` and the vendor fb0 comes back.

## 6. Coexistence with the vendor fbdev driver

* **KMS image, this path on** — the KMS driver binds `14830000.decon_fb` at
  `late_initcall`, so the vendor driver
  (`late_initcall_sync(exynos_decon_register)`, KmsCoexist patch) finds no
  unbound device and never registers a second fb0. Only one driver can own the
  node; there is no double-`register_framebuffer()` race by construction.
* **`/dev/fb0` cannot silently fail.** `drm_fbdev_cma_init()` cannot report a
  failing `register_framebuffer()`: it assigns
  `drm_fb_helper_initial_config()`'s **bool** result to an `int` and tests
  `ret < 0` (`drm_fb_cma_helper.c:344-392`, `drm_fb_helper.c:1503-1523`), so
  `false` looks like success. This slice therefore checks
  `registered_fb[0]` (node 0, `fb_info->device == ex->dev`; the helper passes
  `drm->dev` to `framebuffer_alloc()`, `drivers/video/fbsysfs.c:39-60`),
  the buffer's scannability, and `crtc->primary->fb`, and returns non-zero if
  any of them fails → main's probe fails → the vendor driver binds and
  recreates today's fb0. That is the property the previous KMS attempt lacked.
* **X11 vs KWin** — one CRTC, one master: Xorg's fbdev DDX and KWin must never
  run at the same time (`decon-kms-coexist.md` §5). `fbwin`'s vendor
  `S3CFB_WIN_CONFIG` ioctl is absent in this variant.

## 7. Expected boot log (KMS image, panel up, fbdev on)

Order is deterministic: `video/` level-7 initcalls (DSIM + panel) →
`gpu/` level 7 (KMS probe: card0, then fb0) → level 7s vendor DECON fbdev
(which stays unbound).

Success (fbdev-related lines only; other slices log their own):

```
14830000.decon_fb: fb0: drmfb frame buffer device
14830000.decon_fb: registered panic notifier
14830000.decon_fb: fb0: drmfb 720x1280-32, virtual 720x1280, pitch 2880, scanout IOVA 0x1.....
```

* line 1 is `drm_fb_helper_single_fb_probe()`'s
  `dev_info(dev->dev, "fb%d: %s frame buffer device\n", ...)`
  (`drm_fb_helper.c:948-951`); the device name comes from the DT node
  `decon_fb@14830000`;
* line 2 is the helper's one-time panic notifier registration
  (`drm_fb_helper.c:955-962`);
* line 3 is this slice (`exynos_drm7580_fbdev.c`, `dev_info(ex->dev, ...)`),
  printed only after `registered_fb[0]`, `exynos7580_fb_scanout()` and
  `crtc->primary->fb` have all been verified. `scanout IOVA` is what
  `KmsDmabuf`'s `iovmm_map()` returned (DECON IOVA space starts at
  `0x10000000`), i.e. proof the fb0 buffer is mapped for the display engine.

With `drm.debug=0x04` (`DRM_DEBUG_KMS`) the helper additionally prints
`surface width(720), height(1280) and bpp(32)` (`drm_fb_cma_helper.c:329-332`),
`picking CRTCs for 720x1280 config` and
`desired mode 720x1280 set on crtc 0` (`drm_fb_helper.c:1428, 1450-1453`).

Failure / alternate lines (all from this slice unless marked; any of them
fails the probe, so the vendor fb0 registers instead):

| line | meaning |
|---|---|
| `14830000.decon_fb: fb0 emulation disabled (exynosdrm.fbdev=0)` | optional path switched off, no fb0 from KMS |
| `... no CRTC or connector, not creating fb0` | `num_crtc`/`num_connector` zero — CRTC/DSI slice ordering bug in main |
| `... CRTC lacks an atomic set_config/primary plane, not creating fb0` | CRTC not created with `drm_atomic_helper_set_config`/a primary plane |
| `... drm_fbdev_cma_init failed: -12` | CMA allocation failed (see §5) |
| `... fb helper did not create a framebuffer` | `fb_probe` never ran (no mode) |
| `... fb0 buffer is not scannable: -19` | `exynos7580_fb_scanout()` returned `-ENODEV` (no iovmm domain) |
| `... fb0 was not registered` | `register_framebuffer()` failed silently inside the helper |
| `... CRTC does not scan fb0 out` | `restore_fbdev_mode` did not take effect (`crtc->primary->fb`/`state->enable` wrong) |
| `... fb0 is 1024x768 (virtual 1024x768), expected 720x1280 (virtual 720x1280)` | warning: connector reported no mode; helper used its 1024x768 fallback |
| `drm: Cannot find any crtc or sizes - going 1024x768` | helper-side line for the same condition (`drm_fb_helper.c:913-918`) |

## 8. `/sys/class/graphics/fb0` expectations

Attributes are those of this tree's `drivers/video/fbsysfs.c:501-514` (note
that `blank`, `console` and `cursor` are stubs here and return 0 bytes):

| path | expected | source |
|---|---|---|
| `/dev/fb0` | char `29:0`, `root:video 0660` | `register_framebuffer()` |
| `.../fb0/name` | `drmfb` | `fix.id`, core patch 0002 (empty without it) |
| `.../fb0/virtual_size` | `720,1280` | `var.xres_virtual,yres_virtual` |
| `.../fb0/stride` | `2880` | `fix.line_length` = 720 × 4 |
| `.../fb0/bits_per_pixel` | `32` | `var.bits_per_pixel` |
| `.../fb0/state` | `0` | `FBINFO_STATE_RUNNING` |
| `.../fb0/rotate` | `0` | `var.rotate` |
| `.../fb0/pan` | `0,0` | `var.xoffset,yoffset` |
| `.../fb0/modes` | empty | the DRM helper never fills the fb modelist |
| `.../fb0/mode` | empty | `fb_info->mode == NULL` |
| `.../fb0/blank`, `.../fb0/console`, `.../fb0/cursor` | empty (0 bytes) | vendor stubs, `fbsysfs.c` |
| `.../fb0/device` | symlink to `../../../14830000.decon_fb` | parent device = the DECON platform device |
| `/sys/class/graphics/fbcon` | **absent** | `FRAMEBUFFER_CONSOLE=n` in the KMS image |
| `/sys/class/graphics/fb1` | **absent** | the vendor DECON fbdev never binds |

Sanity checks that the node is the KMS one and not a vendor leftover:

```sh
readlink -f /sys/class/graphics/fb0/device     # .../14830000.decon_fb
cat /sys/class/graphics/fb0/name               # drmfb
cat /sys/class/graphics/fb0/virtual_size       # 720,1280
dmesg | grep -c "frame buffer device"          # 1
ls /sys/bus/platform/drivers/decon/            # 14830000.decon_fb is NOT listed
```

## 9. Build evidence

Work copy `/run/media/asdf/db69437b-…/j7k/kms/KmsFbdev` (a `cp -a` of the
read-only tree); M0 `patches/drm-atomic/0001-0004` applied with `git apply`
(all four clean), then this series.

```
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 drivers/gpu/drm/
  CC      drivers/gpu/drm/drm_gem_cma_helper.o
  CC      drivers/gpu/drm/drm_fb_helper.o
  CC      drivers/gpu/drm/drm_crtc_helper.o
  CC      drivers/gpu/drm/drm_fb_cma_helper.o
  ...
  CC      drivers/gpu/drm/exynos/exynos_drm7580_fbdev.o
  LD      drivers/gpu/drm/exynos/exynosdrm.o
  LD      drivers/gpu/drm/exynos/built-in.o
  LD      drivers/gpu/drm/built-in.o
```

* 0 errors; **0 warnings from `exynos_drm7580_fbdev.c`** (and none from the
  three core files after 3.1-3.3). Remaining warnings in the build are
  pre-existing vendor-code warnings in files this series does not touch:
  `exynos_drm_fb.h:14` header guard, `exynos_drm_gem.c:211,476,483` pointer
  casts, `exynos_drm_g2d.c:1156,1174` casts, `exynos_drm_dmabuf.c:289`
  `%x` on a `dma_addr_t`.
* Toolchain: `aarch64-linux-gnu-gcc (GCC) 16.2.0`, `-Wno-error` as prescribed.
* Build-only scaffolding in that copy (never committed): a
  `config DRM_EXYNOS7580` Kconfig stub (owned by the DeconCrtc/KmsMain slice)
  and appended `.config` lines `CONFIG_DRM_KMS_CMA_HELPER=y`,
  `CONFIG_DRM_EXYNOS7580=y`, `CONFIG_DRM_EXYNOS7580_FBDEV=y`, plus
  `CONFIG_CMA_SIZE_MBYTES=32`.
* Patch verification: on a pristine copy of the five touched files,
  `git apply --check` + `git apply` succeed for 0001→0004 in order, and the
  resulting `exynos_drm7580_fbdev.c` is byte-identical to
  `code/gpu31/kms/exynos_drm7580_fbdev.c`.

## 10. Risks and limits

| item | note |
|---|---|
| Not boot-verified | no flash/reboot was allowed for this slice; §7/§8 are derived from the code paths cited, and `KmsBoot` has to confirm them (int50 harness) |
| `drm_gem_cma_helper.c` writes to `dev->mode_config.fb_base` | `drm_fbdev_cma_create()` sets `fb_base = obj->paddr` (`drm_fb_cma_helper.c:319`); harmless for one CRTC, but it is a DMA-direct paddr, not a DECON IOVA — do not reuse it for window programming |
| panning | the buffer is exactly one screen (720x1280), so `ypanstep=1` never has slack: console/X panning is a no-op, same as the vendor fb0 (`virtual_size` = mode size). A larger virtual buffer needs a custom `fb_probe` — out of scope |
| fbcon off | the KMS image keeps `FRAMEBUFFER_CONSOLE=n`; if it is ever enabled, `register_framebuffer()` calls `drm_fb_helper_set_par()` through fbcon while `drm_fbdev_cma_init()` holds no modeset locks (this slice deliberately does not hold them), which is the normal 3.10 pattern but untested here |
| teardown | cleanup is a `devres` action calling `drm_fbdev_cma_fini()`, so probe-failure rollback cannot leave a registered fb. The driver is built-in with no unbind path; if one is ever added, it must run that cleanup **before** `drm_platform_exit()` (and the contract would need a `fini`) |
| `KmsDmabuf` fb cache | its scanout cache holds one extra fb reference, so `drm_fbdev_cma_fini()` may not free the fb at teardown — the documented bounded leak in `decon-kms-dmabuf.md`, not a regression here |

## 11. Verification runbook (read-only, for `KmsBoot`)

Every command under `flock /tmp/gpsdev.lock`, no device writes:

```sh
dmesg | grep -E "fb0: drmfb|frame buffer device|fb0 (was not|buffer is not|is 1024)"
ls -l /dev/fb0; readlink -f /sys/class/graphics/fb0/device
cat /sys/class/graphics/fb0/{name,virtual_size,stride,bits_per_pixel,state}
cat /sys/module/exynosdrm/parameters/fbdev        # Y unless disabled
ls /sys/class/graphics/                            # fb0, no fbcon, no fb1
fbset -i                                           # 720x1280-32 (busybox fbset if present)
# X11 fallback (KMS image, no KWin running):
startx -- -keeptty                                 # Xorg fbdev DDX opens /dev/fb0
```

Pass criteria: the three success lines of §7 in that order, `name` = `drmfb`,
`virtual_size` = `720,1280`, exactly one "frame buffer device" line, and no
`fb0 ...` error line from this slice.
