# DECON KMS assembly (int50) — M0 atomic + core fixes + shim + six slices + main bind; 18-patch series, 50 gates, Image built

Date: 2026-11-23. Owner: G31_Integrator50 (build seat). Phone: on int49;
host-side assembly and build only. **No flash** — Main flashes for the M1
card0 test.

This is the `KmsAssemble` deliverable from `docs/reports/decon-kms-main.md`
§8, executed with Main's assembly rule: **all seven driver objects live in
`drivers/gpu/drm/exynos7580/`**, and 0200's Makefile is the single wiring
point.

## 0. TL;DR

- **Series applies cleanly, 18 patches, in order**, `git apply --check`
  then `git apply`, 0 fuzz/rejects; new-file hunk integrity checked for
  every patch before applying.
- **Path normalization done** in the four slices that were generated
  against `drivers/gpu/drm/exynos/`: fence, plane, CRTC (0101) and fbdev
  (0004). fbdev's `exynos/Kconfig` and `exynos/Makefile` hunks are
  **dropped** (0200 owns the choice and the object list), so no second
  wiring point exists.
- **Contract header folded in** as new patch `0201`: the frozen
  `code/gpu31/kms/exynos_drm7580.h` copied into the tree with the two
  prototypes 0200 declared locally (`exynos7580_atomic_check`,
  `exynos7580_atomic_commit`) added — semantics unchanged. The in-tree
  header is byte-identical to the repo source.
- **`patches/drm-kms-coexist/0001` and the old
  `patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch` are not
  applied** (subsumed / superseded). A gate proves the KMS choice appears
  only in the decon Kconfig, never a second time in `exynos/Kconfig`.
- **Config closure: 14/14 exact** after `olddefconfig` (KMS choice on,
  `DRM_EXYNOS7580=y`, `DRM_EXYNOS` off, fbcon off, `CMA_SIZE_MBYTES=32`,
  `EXYNOS_IOVMM_V6=y`, vendor display stack on).
- **Build green**: 5304-line log, 0 `error:`, 487 warnings (all ambient
  vendor-tree warnings; **0** in `drivers/gpu/drm/exynos7580/`), Image
  mtime gated, vmlinux freshness gated (the pristine tree ships a
  prebuilt vmlinux).
- **50 gate assertions, 0 failures**, including: 7 sources + header in
  `exynos7580/` and none left in `exynos/`; the 13 main-path slice
  symbols defined exactly once in vmlinux (the object table shows 16 T
  symbols, all unique); **single** `exynos7580_atomic_commit`;
  `DRIVER_ATOMIC` set by exactly one registration (ours); no
  `drm_gem_mmap(`/`drm_gem_create_mmap_offset` anywhere in our sources;
  `late_initcall(exynos7580_drm_init)` and the
  `__initcall_exynos7580_drm_init7` symbol; callback strings in the
  Image.
- Outputs: `Image-int50` `0ef948b7…` (17500384 B),
  `kernel-boot-int50.img` `36d1dbb8…` (26640384 B), stock `dt.img`
  (+313344) and the reference ramdisk (+8822392).

## 1. Series (exact order and bytes)

| # | patch | sha256 (first 16) |
|---|---|---|
| 1 | drm-atomic/0001 atomic headers/state/UAPI | `cdf1a9d17cba646e` |
| 2 | drm-atomic/0002 atomic core + locking + split-plane | `1f146398671ad5f5` |
| 3 | drm-atomic/0003 atomic/plane helpers + build wiring | `d5c1bed323ef656c` |
| 4 | drm-atomic/0004 DRM_IOCTL_MODE_ATOMIC + client cap | `1b6851f992b4b3ad` |
| 5 | drm-atomic/0005 4-arg atomic_set_property hooks | `31e1a141567fd544` |
| 6 | drm-kms/0001 core crtc-helper atomic-aware teardown | `67551cfb151e27f3` |
| 7 | drm-kms/0002 core fb-helper id/fb name fix | `10cb24c2a44e9ee7` |
| 8 | drm-kms/0003 core gem-cma arm64 dma-attrs | `3ce53274ecb1a9bb` |
| 9 | drm-kms/0005 core gem-cma import/free | `9b9ac29796815aec` |
| 10 | drm-kms/0100 DECON export shim | `af33f40716d5b608` |
| 11 | drm-kms/0101 CRTC atomic + link enable | `656ec1ed2984c5d8` |
| 12 | drm-kms/0201 contract header (assembly) | `899ff2d19d60284e` |
| 13 | drm-kms/0001 dsi encoder/connector (re-issued) | `cca57717cd4b8789` |
| 14 | drm-kms/0001 kmsfence vblank/flip events | `1b2d04a00db046e5` |
| 15 | drm-kms/0001 plane primary/overlays/formats | `9648b189b5653377` |
| 16 | drm-kms/0004 kmsfbdev emulation | `ed787ad6c142a4e4` |
| 17 | drm-kms/0001 kmsdmabuf import/framebuffer | `04040b3957f76b79` |
| 18 | drm-kms/0200 main bind (Kconfig/Makefile/main.c) | `ef2b88b394068a76` |

Full sha256s are in the evidence log (one pair per patch, recorded
before `git apply`).

## 2. Assembly changes made here

1. **Path normalization** (`drivers/gpu/drm/exynos/` →
   `drivers/gpu/drm/exynos7580/`) in:
   - `0001-kmsfence-vblank-flip-events.patch` (`_fence.c`),
   - `0001-plane-primary-overlays-and-formats.patch` (`_plane.c`),
   - `0101-crtc-decon-atomic-crtc-and-link-enable.patch` (`_crtc.c`),
   - `0004-kmsfbdev-decon-kms-fbdev-emulation.patch` (`_fbdev.c`).
   `dsi` and `dmabuf` were already re-issued against `exynos7580/`.
2. **fbdev wiring hunks dropped**: `0004` no longer edits
   `drivers/gpu/drm/exynos/Kconfig` or `.../Makefile`
   (`CONFIG_DRM_EXYNOS7580_FBDEV` + `exynosdrm-*` line). 0200's
   `exynos7580/Makefile` lists all seven objects and its Kconfig carries
   the choice; keeping both would have created a second wiring point and
   the `exynos_drm7580.h: No such file` failure recorded in
   `decon-kms-main.md` §8.4.
3. **Contract header as a patch** (`0201`): copies
   `code/gpu31/kms/exynos_drm7580.h` into
   `drivers/gpu/drm/exynos7580/exynos_drm7580.h`, byte-identical, with
   the two mode-config prototypes folded in:
   `int exynos7580_atomic_check(struct drm_device *, struct drm_atomic_state *);`
   `int exynos7580_atomic_commit(struct drm_device *, struct drm_atomic_state *, bool);`
   (0200 still declares them locally; identical signatures, no semantic
   change).
4. **Not applied, by instruction**: `drm-kms-coexist/0001` and the old
   `patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch`.

## 3. Config

Base `config-samsung-j7elte.aarch64` plus
`patches/drm-kms-coexist/display-owner-kms.fragment`. The fragment is
applied with a parser that treats `# CONFIG_X is not set` as a removal
plus a "not set" line (the older gpu31 fragment applier would silently
drop those comment lines and keep stale symbols).

After `olddefconfig` (no `recursive dependency`, no `error:`):

```
CONFIG_EXYNOS7580_DISPLAY_KMS=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_KMS_CMA_HELPER=y
CONFIG_DRM_EXYNOS7580=y
CONFIG_CMA_SIZE_MBYTES=32
CONFIG_EXYNOS_IOVMM_V6=y
CONFIG_EXYNOS_DECON_EXYNOS7580=y
CONFIG_EXYNOS_MIPI_DSI=y
CONFIG_EXYNOS_DECON_LCD=y
CONFIG_PANEL_EA8061_DYNAMIC=y
# CONFIG_DRM_EXYNOS is not set
# CONFIG_EXYNOS7580_DISPLAY_FBDEV is not set
# CONFIG_FRAMEBUFFER_CONSOLE is not set
```

## 4. Build and gates

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- `…/logs/int50-kms-build.log`: 5304 lines, 0 `error:`, 487 `warning:`
  (vendor ambient; `drivers/gpu/drm/exynos7580/` contributes **0**),
  `CC`/`LD` lines for all seven objects and `exynos-drm7580.o`.
- `…/logs/int50-kms-gates.log` (77 lines), `…/logs/int50-kms-evidence.log`
  (111 lines), `…/logs/int50-kms-config.log`.
- 50 assertions in five banner groups, all PASS:
  - **layout/single wiring**: 7 `.c` + header + Kconfig + Makefile under
    `exynos7580/`; zero `exynos_drm7580_*.c` left under `exynos/`;
    Makefile lists exactly the 7 objects and only
    `obj-$(CONFIG_DRM_EXYNOS7580)`; Kconfig keeps
    `depends on … EXYNOS_IOVMM_V6` and `select DRM_KMS_CMA_HELPER`;
    choice only in the decon Kconfig;
  - **atomic core**: `drm_atomic.h`, `drm_atomic_helper.h`,
    `drm_atomic.c`, `DRM_IOCTL_MODE_ATOMIC` in the ioctl table
    (`drm_drv.c`) with `drm_mode_atomic_ioctl` implemented;
    `DRIVER_ATOMIC` set by exactly one registration (ours);
    `late_initcall(exynos7580_drm_init)`, DECON compatible,
    `drm_platform_init`, `DRIVER_RENDER` absent;
  - **no core gem-mmap/offset** in our paths; CMA dumb_create and PRIME
    import wired;
  - **linked symbols**: the 13 main-path slice entry points defined
    exactly once in vmlinux (object table: 16 T, all unique),
    `__initcall_exynos7580_drm_init7` present,
    `exynos7580_drm_driver` present, **one**
    `exynos7580_atomic_commit`, and **no** `exynos_drm_driver` (in-tree
    EXYNOS core not linked);
  - **objects/Image clean**: `exynos7580/built-in.o` present, vmlinux
    newer than build start, callback strings
    (`exynos7580: %s bound, card0 ready (atomic)`,
    `samsung,exynos5-decon_driver`, `exynos7580-decon-drm`) in the Image.

Object offsets (exynos-drm7580.o): atomic_check 0xd80, atomic_commit
0xda0, commit_tail 0x2a20, crtc_init 0xc80, crtc_enable/disable_link
0x7e0/0xb28, planes_init 0x16c0, dsi_init 0x1a4c, dsi_get_modes 0x18c0,
fb_create 0x2710, gem_prime_import 0x2560, fb_scanout 0x21a0,
fbdev_init 0x2c6c, vblank_irq 0x2c08.

## 5. Outputs (sha256)

- `Image-int50` (17500384 B):
  `0ef948b730d5fb0c97a4d8b0032179065b0f73adf4a90964b35888ac2f56963b`
- `kernel-boot-int50.img` (26640384 B = kernel 17500384 + ramdisk
  8822392 + dt 313344 + 3280 header):
  `36d1dbb87d502f73dffa9a7d2bb8d05e1cacf521d1943305e6b104bbf1cd4a7b`
- build log:
  `a7222d5077bc726e4fb1af98a9d2e27f80b616a098c297b178acd9bdef1888a9`
- gate log:
  `ddd49e5b60896171a544569762a824169933026c54872261a11fa3ecead6e663`
- evidence log:
  `879e41b4d102840d4d9244ba027554a02009a5aa51e5ffb34132bee9e09bf3ac`
- config log:
  `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`
- script:
  `6d34aa892a48e3c6b93ac9ef123fdb2e670b55d84ba00169cea0417df6f1db4b`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.

## 6. Hand-off for the M1 card0 test (Main)

1. Flash `kernel-boot-int50.img` to `/dev/mmcblk0p10` (not
   `/dev/block/mmcblk0p10`; the latter writes into devtmpfs).
2. Boot and read dmesg: the bind line is
   `[drm] exynos7580: %s bound, card0 ready (atomic)`; the driver is
   `exynos7580-decon-drm` with `DRIVER_NAME` `exynos` (Mesa kmsro
   pairing depends on that name).
3. Expected nodes: `/dev/dri/card0` (KMS, atomic) and, through
   drm_fbdev_cma, `/dev/fb0`; no separate render node from this driver.
4. If the probe fails, the vendor `late_initcall_sync` fbdev driver must
   bind DECON instead and `/dev/fb0` must come back (that is the designed
   fallback; its absence would be a coexist regression, not a KMS
   success).
5. Modetest/kmscube with atomic (the M1 acceptance): connector DSI,
   720x1280 panel mode, one primary + overlay planes, flip events from
   the DECON FRAME interrupt. The first commit also arms the initial
   fbdev modeset (fbdev slice's `drm_fbdev_cma_restore_mode()` inside
   `exynos7580_fbdev_init()`), so a `crtc.state->enable &&
   crtc.primary->fb` failure is a probe failure by design.

## 7. Script notes

- **Two gate corrections during the run**, both found on the retained
  built tree before the final build: (a) `DRM_IOCTL_MODE_ATOMIC` lives in
  the ioctl table in `drivers/gpu/drm/drm_drv.c`, not in `drm_atomic.c`
  (the latter holds `drm_mode_atomic_ioctl`); (b) the KMS warning gate
  initially matched `drivers/thermal/exynos7580/` vendor warnings — it now
  matches `drivers/gpu/drm/exynos7580/` only.
- **Freshness is double-gated**: Image mtime ≥ build start *and* vmlinux
  mtime ≥ build start, because the pristine tree ships a prebuilt
  vmlinux/Image whose symbols (e.g. `exynos_drm_driver`) would otherwise
  fool a link-time gate.
- The fragment applier treats `# CONFIG_X is not set` lines as
  removals; the gpu31 applier (non-comment lines only) would have kept
  `CONFIG_DRM_EXYNOS` and `CONFIG_FRAMEBUFFER_CONSOLE` from the base
  config.
- Script `code/gpu31/series/build-int50.sh` (261 lines) is the one that
  produced §5; it deletes scratch and stage after packing, and keeps the
  per-patch sha256s in the evidence log.
