# DECON KMS main bind (M1 slice `KmsMain`) — card0, atomic drm_driver, late_initcall

Status: **complete (build-verified; no device access, no flash, no reboot)**. This
slice owns driver registration and probe only. The CRTC, plane, DSI encoder,
dma-buf, fence and fbdev implementations are the sibling slices' files and are
declared in the frozen contract `code/gpu31/kms/exynos_drm7580.h`
(commit `8048d08`).

Deliverables
- `code/gpu31/kms/exynos_drm7580_main.c` (sha256 `4d13bafe…482a0`, 554 lines)
- `patches/drm-kms/0200-kms-main-bind.patch` (sha256 `ef2b88b3…98858`, 729 lines)
- this report

The patch creates `drivers/gpu/drm/exynos7580/` (`Kconfig`, `Makefile`,
`exynos_drm7580_main.c`), wires `drivers/gpu/drm/{Kconfig,Makefile}`, and carries
the display-owner Kconfig choice plus the vendor fbdev `late_initcall_sync` move
that were specified by `patches/drm-kms-coexist/0001-display-owner-kconfig.patch`
— see §6 for the two rebased deltas and why `coexist/0001` must not be applied a
second time.

## 1. What the driver is

| Item | Value | Why |
|---|---|---|
| `DRIVER_NAME` | `exynos` | Mesa's kmsro pairs `/usr/lib/dri/exynos_dri.so` with the KMS node by driver name; changing it sends KWin to software rendering (`docs/reports/decon-kms-req.md` req. 2) |
| `driver_features` | `DRIVER_ATOMIC \| DRIVER_MODESET \| DRIVER_GEM \| DRIVER_PRIME` | req. 5/6 (atomic), 1 (primary KMS node), 9 (planes/props created by the backport only for atomic drivers), 18/19/21 (PRIME + dumb) |
| GEM flavour | CMA | `dumb_create/dumb_map_offset/dumb_destroy = drm_gem_cma_*`, `gem_vm_ops = drm_gem_cma_vm_ops`, `gem_free_object = drm_gem_cma_free_object` |
| PRIME | `gem_prime_import = exynos7580_gem_prime_import` (KmsDmabuf), `gem_prime_export = drm_gem_prime_export` + local sg-table/vmap callbacks for CMA buffers; the sg table carries `sg_set_buf()` as well as the DMA address/length | import = product of the dmabuf slice; export is what KWin's gbm dumb path needs (req. 21) and needs a real `sg_page()` so `dma_map_sg()` has something to map |
| probe match | `samsung,exynos5-decon_driver` only | stock `dt.img`; the DSIM node belongs to the vendor `dsim_drv.c` and the encoder calls into it |
| bind point | `late_initcall(exynos7580_drm_init)` | level 7 in `drivers/gpu/`, after `drivers/video/` (vendor DSIM/panel, also level 7) and before `late_initcall_sync` |
| registration | `drm_platform_init()` | 3.10 has no `drm_dev_alloc()`; the core allocates the `drm_device`, so state hangs off `drm->dev_private` and `to_exynos7580()` reads it |

`DRIVER_RENDER` is not used: the primary node must be a KMS card node, and this
tree has no `DRIVER_RENDER` bit (the separate render-only panfrost7580 driver
owns `renderD128`).

## 2. Bind sequence

`late_initcall` → `platform_driver_register(&exynos7580_drm_platform_driver)`
triggers `exynos7580_probe()` for the already-populated platform device
(`arch_initcall_sync` populated them long before):

1. `dev->of_node` present, `of_device_is_compatible(np, "samsung,exynos5-decon_driver")`,
   not the DSIM compatible, `of_device_is_available()` — otherwise `-ENODEV`.
2. `devm_kzalloc(struct exynos7580_device)`, `platform_set_drvdata(pdev, ex)`.
3. `decon_kms_get(&pdev->dev)` → `ex->decon`. In KMS mode the vendor
   `decon_probe()` never runs (this driver owns the device), so the shim must
   stand the DECON core up here: clocks, SFR mapping, runtime PM/iovmm and the
   panel hand-off (`docs/reports/decon-kms-coexist.md` §4.3 item 4,
   `docs/reports/decon-kms-api.md` §6). Failure is fatal before any DRM object
   exists.
4. `drm_platform_init(&exynos7580_drm_driver, pdev)` → core allocates the
   `drm_device`, allocates the control+primary minors (this is `card0`),
   calls `->load()`.
5. `->load()` (`exynos7580_load`):
   1. `ex = platform_get_drvdata(drm->platformdev)`, `drm->dev_private = ex`,
      `ex->drm = drm`, `ex->dev = &pdev->dev`.
   2. `drm_mode_config_init()`; `min 64x64`, `max 2048x2048`; `funcs = {fb_create =
      exynos7580_fb_create, atomic_check = exynos7580_atomic_check,
      atomic_commit = exynos7580_atomic_commit}` — the check/commit pair is
      implemented and exported by the CRTC slice (see §4).
   3. `drm_vblank_init(drm, 1)` — one pipe; also installs the vblank timestamp
      machinery the flip events are stamped from.
   4. `exynos7580_planes_init(ex)` **before** `exynos7580_crtc_init(ex)`; the
      CRTC is created with `drm_crtc_init_with_planes(primary = &ex->planes[0])`
      at index 0 (`EXYNOS7580_PIPE`).
   5. `drm_mode_config_reset(drm)` — allocates the per-object atomic states
      that the backport does not create in the init helpers.
   6. `exynos7580_dsi_init(ex)`; if the connector was created after the reset and
      has no state, its `funcs->reset()` is called by hand (a second
      `drm_mode_config_reset()` would force `connector->status` back to
      `unknown` and KWin drops the CRTC for lack of a connected output).
   7. `decon_kms_register_vblank_cb(ex->decon, exynos7580_vblank_isr, ex)` —
      arm the DECON FRAME interrupt before the first commit, because the
      initial modeset below arms its flip event at vblank and the counter has
      to advance.
   8. `exynos7580_fbdev_init(ex)` — registers `/dev/fb0` and runs the initial
      modeset (see §3).
   9. `exynos7580_initial_modeset(ex, drm)` — postcondition check under
      `drm_modeset_lock_all()`: `crtc.state->enable` and `crtc.primary->fb` must
      be set, i.e. fb0 is actually being scanned out.

## 3. Initial modeset: where the coexist step actually runs

`decon-kms-coexist.md` §4.3 item 3 requires the KMS driver to do

```c
drm_modeset_lock_all(dev);
drm_fb_helper_restore_fbdev_mode(&fbdev->fb_helper);
drm_modeset_unlock_all(dev);
```

because `drm_fbdev_cma_init()` only registers the fb; it does not commit a mode,
and the KMS image runs with `FRAMEBUFFER_CONSOLE` off.

The `drm_fb_helper` lives in the fbdev slice's file-private state (the contract
header has no fbdev member and no accessor), so **the call is executed inside
`exynos7580_fbdev_init()`**, as `drm_fbdev_cma_restore_mode()` — which is exactly
`drm_modeset_lock_all()` + `drm_fb_helper_restore_fbdev_mode()` +
`drm_modeset_unlock_all()` (`drivers/gpu/drm/drm_fb_cma_helper.c:426-441`,
`drivers/gpu/drm/drm_fb_helper.c:285-300` in K3.10) — followed by the slice's own
verification that fb0 registered and the primary plane scans it out. Main does
not repeat the modeset (that would double-commit the same state). Main's own
contribution to the step is item 8 above: it holds the same modeset locks and
turns a "fb0 exists but nothing scans it out" result into a probe failure, which
is what triggers the vendor fallback. It also refuses to continue when
`exynos7580_fbdev_init()` reports failure, per the same section.

## 4. vblank / DRM_IRQ hooks (KmsFence dependency)

- `drm_vblank_init(drm, 1)` in `->load()`; `drm_vblank_cleanup()` in the unwind
  and in `->unload()`.
- `->get_vblank_counter = exynos7580_get_vblank_counter` returns
  `drm_vblank_count(dev, 0)`; the core accounts for interrupts missed while the
  IRQ was off.
- `->enable_vblank` / `->disable_vblank` only refcount users of the single pipe
  (`exynos7580_vblank_users`, a file static because the contract struct is
  frozen). They do not touch the DECON: the DRM core calls them with
  `dev->vbl_lock` held (spinlock, IRQs off, `drm_vblank_get()`/
  `drm_vblank_put()`), while `decon_kms_register_vblank_cb()` is documented as
  process context.
- The DECON FRAME callback is installed once from `->load()` (§2 step 7) and
  removed in the load unwind and in `->unload()`. `decon_kms_register_vblank_cb()`
  enables the vendor interrupt while a callback is registered and leaves the
  vendor `decon_activate_vsync()` refcounting alone; the vendor fbdev path
  enables the same interrupt whenever the display is on, so this adds no new
  interrupt load.
- The callback (`exynos7580_vblank_isr`, hard IRQ) calls
  `drm_handle_vblank(drm, 0)` and then `exynos7580_vblank_irq(drm, 0)`, the
  fence slice's event completion. It does not sleep and does not re-enter the
  decon_kms API.
- `DRIVER_HAVE_IRQ` is deliberately not set: DRM must not try to install an IRQ
  handler of its own, and `drm_wait_vblank` then does not require
  `dev->irq_enabled`.

Atomic commit path (`->load` → `atomic_commit` callback):

The two `mode_config.funcs` entries are `exynos7580_atomic_check()` and
`exynos7580_atomic_commit()`, both implemented and exported by
`exynos_drm7580_crtc.c` (declared at the top of the mode-config section of
`exynos_drm7580_main.c`; the frozen contract header does not carry them yet).
Their shape, which main relies on:

```text
exynos7580_atomic_check(dev, state)     = drm_atomic_helper_check()
exynos7580_atomic_commit(dev, state, async):
    -ENODEV guard (no dev_private/decon)
    for each crtc state that becomes active or has mode_changed:
        exynos7580_crtc_enable_link(ex)     (fallible: DSIM/clock/iovmm)
    drm_atomic_helper_prepare_planes()
    drm_atomic_helper_swap_state()
    exynos7580_commit_tail(state)           (plane hooks, fence event, cleanup)
```

`async` is **not** rejected: KWin issues `NONBLOCK|PAGE_FLIP_EVENT` page flips,
and they must run the same tail and complete through the flip event (the plane
and fence slices own that). `drm_atomic_helper_commit()` is not used because the
link enable has to happen before any state is prepared or swapped and may fail;
a failure returns to userspace with its previous state intact. Window
programming stays in the plane slice: this file never touches
`decon_kms_commit()`.

## 5. Failure injection table (vendor fallback)

The fallback is the driver-core mechanism, not a retry loop: after a failed
probe `dd.c` clears `dev->driver` (`K3.10 drivers/base/dd.c:278,317-334`) and
the vendor `decon_driver` (`late_initcall_sync`, moved by this patch) matches
`samsung,exynos5-decon_driver` and runs its own `decon_probe()`, registering the
vendor `/dev/fb0`. `-EPROBE_DEFER` is never returned, so there is no deferred
retry.

| # | Failure point | Returned | State left behind | Fallback result |
|---|---|---|---|---|
| 1 | not the DECON node / node disabled | `-ENODEV` | nothing | vendor binds at level 7s (`/dev/fb0`, X11) |
| 2 | `devm_kzalloc(ex)` | `-ENOMEM` | nothing | as 1 |
| 3 | `decon_kms_get()` fails (shim: `-ENODEV`, `-EBUSY`, or its own error) | shim errno | handle dropped, drvdata cleared by probe | as 1 |
| 4 | `drm_platform_init()` (core out of memory, minor allocation, `->load` error) | errno | `->load` unwound (below), DECON handle put, drvdata cleared | as 1 |
| 5 | `drm_mode_config_init()`/`drm_vblank_init()` | `-ENOMEM` | `mode_config_cleanup()` (after vblank cleanup for 5b) | as 1 |
| 6 | `exynos7580_planes_init()` | slice errno | `drm_vblank_cleanup()` + `drm_mode_config_cleanup()` (cleans every object registered so far, incl. the planes) | as 1 |
| 7 | `exynos7580_crtc_init()` | slice errno | as 6 (CRTC not on the list yet) | as 1 |
| 8 | `exynos7580_dsi_init()` | slice errno | as 6 (encoder/connector cleaned by `drm_mode_config_cleanup()`) | as 1 |
| 9 | `exynos7580_fbdev_init()` | slice errno | as 6; the fbdev slice must leave no registered fb0 when it fails (its own contract), otherwise the vendor driver would get fb1 | as 1 |
| 10 | initial-modeset postcondition (`crtc.state->enable`/`primary->fb` unset) | `-EIO` | as 6 | as 1 |
| 11 | DECON link enable on a runtime commit | `enable_link` errno | commit aborted, userspace keeps its previous state; card0 stays bound | n/a (not a probe failure) |

Notes
- `drm_get_platform_dev()`'s error path only releases the minors and frees the
  `drm_device`; it does not call `->unload()`. That is why `->load()` itself
  unwinds the modeset state (items 5–10) instead of relying on core teardown.
- After a successful bind, `->unload()` (called by `drm_platform_exit()` from
  `exynos7580_remove()`) drops the vblank callback, the vblank core, the mode
  config objects and the DECON handle, and clears `drm->dev_private`.

## 6. Kconfig / Makefile wiring and the rebased coexist patch

Created by the patch:

```text
drivers/gpu/drm/exynos7580/Kconfig      config DRM_EXYNOS7580 (bool)
                                        depends on DRM=y && OF && ARM64 &&
                                                   ARCH_EXYNOS &&
                                                   EXYNOS7580_DISPLAY_KMS &&
                                                   EXYNOS_IOVMM_V6
                                        select DRM_KMS_HELPER DRM_KMS_CMA_HELPER
                                        VIDEOMODE_HELPERS
drivers/gpu/drm/exynos7580/Makefile     obj-$(CONFIG_DRM_EXYNOS7580) += exynos-drm7580.o
                                        exynos-drm7580-y = main crtc plane dsi
                                                           dmabuf fence fbdev
                                        ccflags-y += -I drivers/video/exynos/decon_7580
drivers/gpu/drm/Kconfig                 source "drivers/gpu/drm/exynos7580/Kconfig"
drivers/gpu/drm/Makefile                obj-$(CONFIG_DRM_EXYNOS7580) += exynos7580/
```

`bool`, never `tristate`: a module would load after the vendor fallback bound the
device, and `module_platform_driver` is level 6, i.e. before the panel driver.

`EXYNOS_IOVMM_V6` is in the dependency list because `iovmm_map()` is only
declared in `include/linux/exynos_iovmm.h` for that configuration, and the
dmabuf slice's import path needs it (`drivers/iommu/Kconfig:225-229`;
`CONFIG_EXYNOS_IOVMM_V6=y` in the stock j7 config).

The patch also inserts the `EXYNOS7580_DISPLAY_FBDEV`/`EXYNOS7580_DISPLAY_KMS`
choice at the top of `drivers/video/exynos/decon_7580/Kconfig` and rewrites
`late_initcall(exynos_decon_register)` to `late_initcall_sync(...)`. Both are the
content of `patches/drm-kms-coexist/0001-display-owner-kconfig.patch`; **because
this patch carries them, `coexist/0001` is subsumed and must not be applied
again** (its hunks would conflict: the Kconfig entry it rewrites is already
`bool`/gated here).

Two deltas against `coexist/0001`, required to keep Kconfig valid on this tree
(otherwise `conf` prints `error: recursive dependency detected!` and may leave a
half-resolved display-owner choice):

1. the choice's first default is `default EXYNOS7580_DISPLAY_FBDEV` **without**
   `if FB`, and `EXYNOS7580_DISPLAY_FBDEV` has no `depends on FB`. Both forms
   make the choice depend on FB, and FB is selected by `DRM_KMS_HELPER`, which
   is selected by `DRM_EXYNOS7580`, which depends on `EXYNOS7580_DISPLAY_KMS`
   (part of that choice) — a cycle. Semantics are preserved: FBDEV is the
   shipping default, KMS still requires `DRM=y`, the KMS image selects it in its
   fragment, and `EXYNOS_DECON_EXYNOS7580` still `depends on FB`.
2. nothing else in the vendor stack changes (no `depends on FB` edit, no symbol
   rename).

Build fragments are unchanged: `patches/drm-kms-coexist/display-owner-fbdev.fragment`
and `display-owner-kms.fragment` still describe the two images.

Integration requirements this patch places on assembly:
- the contract header `exynos_drm7580.h` and the shim header
  `include/linux/exynos_decon_kms.h` must be in the tree before this driver
  compiles (they are not part of this patch);
- the six sibling sources must be in `drivers/gpu/drm/exynos7580/` and compiled
  into `exynos-drm7580.o`. As of this writing the sibling patches add
  `drivers/gpu/drm/exynos/exynos_drm7580_*.c` instead (and
  `0004-kmsfbdev…` wires them into `exynosdrm.o` through
  `CONFIG_DRM_EXYNOS7580_FBDEV`); the object must be the standalone KMS driver
  registered here, not the legacy in-tree exynos driver, so those files must be
  re-pathed by `KmsAssemble`. The `fbdev` object is listed unconditionally in this
  Makefile: `exynos7580_fbdev_init()` is referenced unconditionally by `->load()`.

## 7. Expected dmesg

Success (KMS image, `EXYNOS7580_DISPLAY_KMS=y`):

```
[drm] Supports vblank timestamp caching Rev 1 (10.10.2010).
[drm] No driver support for vblank timestamp query.
[drm] exynos7580: samsung,exynos5-decon_driver bound, card0 ready (atomic)
[drm] Initialized exynos 1.0.0 20261123 on minor 0
```

The first two lines come from `drm_vblank_init()` inside `->load()`, the third
is this file's bind line, the last is `drm_get_platform_dev()`'s. The fbdev
slice adds its own `fb0: drmfb 720x1280-32 …` line when the initial modeset
succeeded, and the vendor `decon registered successfully` line must be absent —
the vendor probe did not run.

Probe failure (example: the shim refuses the DECON with `-EBUSY`):

```
exynos7580-decon-drm 14830000.decon_fb: DECON handle unavailable (-16), vendor fbdev fallback
exynos7580-decon-drm: probe of 14830000.decon_fb failed with error -16
decon 14830000.decon_fb: decon_probe start
…
decon 14830000.decon_fb: decon registered successfully
```

The `probe of … failed` line is `drivers/base/dd.c:331-335` (`KERN_WARNING`) and
appears for every errno except `-ENODEV`/`-ENXIO`, which only reach a
`pr_debug` "rejects match" line. The last two lines are the property the previous
attempt lacked: `/dev/fb0` and the X11/nura-shell path come back without
re-flashing.

## 8. Verification performed

Kernel tree is the read-only vendor tree copy
(`/run/media/…/j7k/kms/KmsMain`, `cp -a`); the source tree itself was never
written.

1. **The whole assembled series applies cleanly to a pristine tree**
   (`KmsVerify`, fresh `cp -a`), `git apply --check` then `git apply`, in this
   order — all **OK**, no fuzz, no rejects:

   ```
   patches/drm-atomic/0001..0005
   patches/drm-kms/0001-core-drm-crtc-helper-atomic-aware-unused-function-teardown.patch
   patches/drm-kms/0002-core-drm-fb-helper-fix-id-drmfb.patch
   patches/drm-kms/0003-core-drm-gem-cma-helper-arm64-dma-attrs.patch
   patches/drm-kms/0005-core-drm-gem-cma-import-free.patch
   patches/drm-kms/0100-decon-export-shim.patch
   patches/drm-kms/0101-crtc-decon-atomic-crtc-and-link-enable.patch
   patches/drm-kms/0001-dsi-encoder-connector.patch
   patches/drm-kms/0001-kmsfence-vblank-flip-events.patch
   patches/drm-kms/0001-plane-primary-overlays-and-formats.patch
   patches/drm-kms/0004-kmsfbdev-decon-kms-fbdev-emulation.patch
   patches/drm-kms/0001-kmsdmabuf-import-and-framebuffer.patch
   patches/drm-kms/0200-kms-main-bind.patch          (this slice)
   ```

   `0005-core-drm-gem-cma-import-free.patch` is required for imported GEM
   objects and must come after `0003-core…`; main does not duplicate that
   teardown. `patches/drm-kms-coexist/0001-display-owner-kconfig.patch` is
   *not* in the list: 0200 carries its content.
2. **Kconfig resolves in both owner modes** (`make olddefconfig`):
   - stock config (`CONFIG_FB=y`, `CONFIG_DRM=y`, vendor DECON on):
     `CONFIG_EXYNOS7580_DISPLAY_FBDEV=y`, `# CONFIG_DRM_EXYNOS7580 is not set` —
     i.e. the wrapper image is unchanged;
   - with `CONFIG_EXYNOS7580_DISPLAY_KMS=y`:
     `CONFIG_DRM_EXYNOS7580=y`, `# CONFIG_EXYNOS7580_DISPLAY_FBDEV is not set`;
   - `conf` prints **no** `recursive dependency` / `error` in either run.
3. **Build** (assignment command), fresh tree, stubs for the sibling sources:

   ```
   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 \
        drivers/gpu/drm/
   → exit 0, 0 errors
   CC  drivers/gpu/drm/exynos7580/exynos_drm7580_main.o
   LD  drivers/gpu/drm/exynos7580/exynos-drm7580.o
   LD  drivers/gpu/drm/exynos7580/built-in.o
   LD  drivers/gpu/drm/built-in.o
   ```

   Warnings in `exynos_drm7580_main.c` or anywhere in
   `drivers/gpu/drm/exynos7580/`: **none**. The pre-existing
   `drivers/gpu/drm/exynos/` warnings listed in `decon-kms-m0.md` §6.2 are
   unchanged, and the run reports no Kconfig error or recursion.
4. **Integration compile against the real sibling sources** (same fresh tree,
   with `0001-dsi…`, `0001-kmsfence…`, `0001-plane…`, `0004-kmsfbdev…` applied
   and those files present in `drivers/gpu/drm/exynos7580/`; CRTC stubbed in
   the earlier run): `make … -j8 drivers/gpu/drm/` → exit 0, 0 errors,
   `LD drivers/gpu/drm/exynos7580/exynos-drm7580.o`.

   This run found and `DsiEncoder` fixed (`36372ed`) the one contract leftover
   `exynos_drm7580_dsi.c:300: drm = &ex->drm;` (`struct drm_device **` after
   `8048d08`); the DSI patch now also creates its file under
   `drivers/gpu/drm/exynos7580/`.

   It also exposed the assembly mismatch this patch had flagged: the
   fence/plane/fbdev patches still create their sources in
   `drivers/gpu/drm/exynos/`, and `0004-kmsfbdev…` wires the fbdev file into
   `exynosdrm.o` through `CONFIG_DRM_EXYNOS7580_FBDEV`, which then fails with
   `fatal error: exynos_drm7580.h: No such file or directory` (the contract
   header lives in `drivers/gpu/drm/exynos7580/`). With
   `CONFIG_DRM_EXYNOS7580_FBDEV` off (or after re-pathing those files into
   `drivers/gpu/drm/exynos7580/` and dropping that `exynosdrm-*` Makefile line)
   the build is clean. `KmsAssemble` must pick one directory for the whole
   slice set; this patch's Makefile builds all seven objects from
   `drivers/gpu/drm/exynos7580/`.
5. **Full slice set, no stubs** (same fresh tree, the complete series of item 1
   applied including the shim `0100`, CRTC `0101` and dmabuf, and the
   fence/plane/fbdev sources copied into `drivers/gpu/drm/exynos7580/`):

   ```
   make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 \
        drivers/gpu/drm/ drivers/video/exynos/decon_7580/
   → exit 0, 0 errors
   LD  drivers/video/exynos/decon_7580/decon.o        (vendor + export shim)
   LD  drivers/gpu/drm/exynos7580/exynos-drm7580.o    (all seven objects)
   ```

   No warning in `drivers/gpu/drm/exynos7580/` nor in
   `decon_kms_api.c` in this configuration (the `exynos_drm7580_dsi.c:300`
   warning of item 4 is fixed by `DsiEncoder`'s `36372ed`). Re-run after that
   commit: 15 patches + `0200` all `git apply` OK, build exit 0, 0 errors,
   0 slice warnings.

Not verified (out of scope for this seat): boot, `card0` node creation on the
device, `modetest`/`kmscube`, actual atomic commits and flip events. Those are
`KmsBoot`'s int50 slot. No flash, reboot or device write was performed.
