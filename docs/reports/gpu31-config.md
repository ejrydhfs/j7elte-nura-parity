# gpu31 config matrix — kernel config fragments with closure proofs + size estimates

Date: 2026-11-19. Base: `code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`
(stock, `# CONFIG_MODULES is not set`, `# CONFIG_DRM is not set`, `CMA_SIZE_MBYTES=0`,
vendor `EXYNOS_DECON_FB=y`, panel `EA8061_DYNAMIC=y`, kbase `MALI_MIDGARD/T7XX/R15P0=y`).
Tree: `android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07` (3.10.108,
read-only for this slice). Phone is UP (kernel #35, Main 2026-11-19) — this slice is
host-side only: no device contact, no tree writes, no Image build (hard rules).

Rule compliance: this slice produces **patch files under `code/gpu31/config/` only**
(fragments + checker). Only `G31_Integrator` builds/flashes; only `G31_M1Verify`
flashes after Main's go. No reboot involved.

## 1. Fragments (7)

| File | Symbols | Role |
|---|---|---|
| `code/gpu31/config/fragments/c0-modules-only.fragment` | `MODULES=y` | M2 kbase-shim path groundwork, no DRM |
| `code/gpu31/config/fragments/d0-m1-minimal.fragment` | M1 6-symbol set | **RECOMMENDED boot** — bit-identical set to `patches/drm-m1/m1-kconfig.fragment` |
| `code/gpu31/config/fragments/d1-plus-vidi.fragment` | D0 + `DRM_EXYNOS_VIDI=y` | probe-dead subdriver add-on experiment |
| `code/gpu31/config/fragments/d2-full-display.fragment` | 16 symbols (FIMD/HDMI/VIDI/IPP/FIMC/ROTATOR/GSC requests + MFD_SYSCON/FB_MODE_HELPERS/VIDEOMODE_HELPERS) | opt-in modeset experiments ONLY, integrator-gated |
| `code/gpu31/config/fragments/d3-nomodules-drm.fragment` | D0 minus MODULES | isolates loader cost / answers "does card0 need modules?" (no) |
| `code/gpu31/config/fragments/m0-mali-off.fragment` | D0 + 3× `# … is not set` (MIDGARD/T7XX/R15P0) | panfrost-cutover SHAPE proof (Kconfig only — real cutover needs M2/M3 driver+DT work) |
| `code/gpu31/config/fragments/m1-mali-on.fragment` | D0 + `MALI_MIDGARD/T7XX/R15P0=y` | stock kbase restated (DDK r15p0 / UK 10.6, `/dev/mali0`) |
| `code/gpu31/config/fragments/fbcon-keep-off.annotation` | `# CONFIG_FRAMEBUFFER_CONSOLE is not set` (annotated) | guard: fbcon steals `/dev/fb0` from X — stays OFF for all boot variants |

D0 is intentionally the same 6 symbols as the proven M1 fragment
(`patches/drm-m1/m1-kconfig.fragment`): `MODULES/D RM/KMS_HELPER/EXYNOS/G2D/DMABUF`.
Reviewed together with `patches/drm-m1/*` (review §9 PASS): the M1 node-only path is
an **ALTERNATIVE to the display-swap path (§6–§8), not a complement** — do not apply
both Kconfig fragments (the PRIME patch targets `exynos7580_drm.c`, i.e. the swap path).

## 2. Review-response (commit 625ed18, items touching this slice)

Two review notes touch config surface; both are already satisfied by the fragments above —
no fragment change was needed, verified by re-running the closure proofs below:

- **§8 `config-drm-exynos7580.fragment` CONDITIONAL — `ARCH_EXYNOS` reality + 32 MB CMA.**
  Verified: `config ARCH_EXYNOS` exists in BOTH `arch/arm/Kconfig:796` and
  `arch/arm64/Kconfig:191` (this tree builds arm64, so the symbol is real), and the
  stock config already carries `CONFIG_ARCH_EXYNOS=y` + `CONFIG_ARCH_EXYNOS7=y` +
  `SOC_EXYNOS7580=y`. So `DRM_EXYNOS7580`-style swap fragments select on a live arch
  symbol. CMA: this slice keeps `CMA_SIZE_MBYTES=0` for every variant (M1 needs no
  scanout; fb helpers early-return on `num_connector==0`). The swap path's 32 MB
  reservation is its own fragment's business (`patches/drm/config-drm-exynos7580.fragment`);
  headroom math (§5) shows 32 MB CMA costs nothing in BOOT (RAM reservation, not image
  bytes). No action in this slice.
- **§7 DT single-compatible + §9 alternative-path note.** D2 requests every in-tree
  subdriver, but the DT rule (never rebuild stock `dt.img`) means FIMD/HDMI/G2D/VIDI
  probes resolve against the STOCK j7elte DT: no `samsung,exynos5250-g2d`,
  `exynos5-hdmi`, `exynos4210/5250-fimd`, or `exynos-drm-vidi` device exists there
  (verified by grep over `arch/arm64/boot/dts/exynos7580-j7elte_rev*.dts` and
  mach-exynos board sources — zero hits). So on stock DT the extra D1/D2 subdrivers
  are compile-in-but-never-probe, exactly like G2D on M1. The one-compatible-per-node
  requirement bites only when the swap-path DT patch (0002) is applied — out of scope
  here. No action in this slice.

Untouched by this slice (other owners): sched/resv/fence shims (G31_Sched),
display-driver hunks (G31_DrmCore), PRIME import-by-copy (G31_DmaBuf), Mesa
`n_slots`/r7p0 stride (G31_Mesa/G31_KbaseShim). No `.c`/`.h`/Makefile content in
this slice — nothing to fix here.

## 3. Dependency closure proofs (all 7 variants, `olddefconfig`, read-only tree)

Runner: `code/gpu31/config/check-matrix.sh` (extends `code/kernel/m1/m1-kconfig-check.sh`;
private out-dir `$OUT` defaults to `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/mconfig`
because host `/tmp` is a small tmpfs — never `/tmp`). Result 2026-11-19: **ALLPASS**.

| Variant | Resolved (post-`olddefconfig`) |
|---|---|
| c0 | `MODULES=y`, `# DRM is not set`; FORCE_LOAD/UNLOAD/MODVERSIONS/SIG stay `not set` |
| d0 (= M1) | `MODULES/DRM/KMS_HELPER/EXYNOS/G2D/DMABUF=y`; `FIMD/HDMI/VIDI/IPP`=not set; `CFB_FILLRECT/COPYAREA/IMAGEBLIT=y` (unchanged); `EXYNOS_DECON_FB=y`; `FRAMEBUFFER_CONSOLE`=not set; `CMA_SIZE_MBYTES=0` |
| d1 | d0 + `DRM_EXYNOS_VIDI=y` (no DT/board device registers `exynos-drm-vidi` → probe never fires) |
| d2 | `FIMD/HDMI/VIDI/G2D/IPP/FIMC/ROTATOR=y`, `MFD_SYSCON/FB_MODE_HELPERS/VIDEOMODE_HELPERS=y`; `GSC` **silently dropped** (symbol vanishes — needs `ARCH_EXYNOS5`, which is `not set` on this arm64 tree); `CMA_SIZE_MBYTES=0` kept; `FRAMEBUFFER_CONSOLE` still `not set` |
| d3 | `# MODULES is not set` + d0 DRM set verbatim (loader not needed for card0) |
| m0 | d0 + `# MALI_MIDGARD/T7XX is not set` (T7XX gate is `menuconfig`, follows MIDGARD off) |
| m1 | d0 + `MALI_MIDGARD/T7XX/R15P0=y` (stock) |

Dependency evidence (Kconfig file:line in the shared tree):

- `DRM`: `depends on (AGP||AGP=n)&&!EMULATED_CMPXCHG&&MMU` (`drivers/gpu/drm/Kconfig:10`);
  stock: AGP line absent (=n, OK), EMULATED_CMPXCHG absent (off), `MMU=y` → satisfiable.
  Selects `HDMI/I2C/I2C_ALGOBIT/DMA_SHARED_BUFFER` — all already `=y` except HDMI,
  which closes to `CONFIG_HDMI=y` (`drivers/video/hdmi.c`, 8787 B src, built-in 1823 B text).
- `DRM_KMS_HELPER`: `depends on DRM`, selects `FB` (=y stock) + `FRAMEBUFFER_CONSOLE`
  **iff `!EXPERT`** (`Kconfig:31-35`); stock `EXPERT=y` → fbcon NOT pulled (verified:
  stays `not set` in c0/d0/d1/d2/d3/m0/m1).
- `DRM_EXYNOS`: `depends on DRM && (PLAT_SAMSUNG||ARCH_MULTIPLATFORM)`
  (`exynos/Kconfig:3`); stock `PLAT_SAMSUNG=y` → OK. Selects `DRM_KMS_HELPER` +
  `FB_CFB_{FILLRECT,COPYAREA,IMAGEBLIT}` (all `=y` stock, zero churn) +
  `VT_HW_CONSOLE_BINDING if FRAMEBUFFER_CONSOLE` (off).
- Subdrivers: `G2D` needs `!VIDEO_SAMSUNG_S5P_G2D` (`not set` stock → OK);
  `HDMI` needs `!VIDEO_SAMSUNG_S5P_TV` (`not set` stock, §M1 tree confirms);
  `FIMD` needs `OF && !FB_S3C && !ARCH_MULTIPLATFORM` (OF=y, FB_S3C not set, MULTIPLATFORM
  unset → OK); `GSC` needs `IPP && ARCH_EXYNOS5` — **fails** (ARCH_EXYNOS5 not set) →
  request dropped silently; `FIMC` needs `IPP && MFD_SYSCON && OF` (MFD_SYSCON shipped
  explicitly in D2); `ROTATOR` needs `IPP` only. `VIDI` needs `DRM_EXYNOS` only.
- `MALI_T7XX` is a `menuconfig` gated by `(SOC_EXYNOS7420||SOC_EXYNOS5433||SOC_EXYNOS7580)`
  + `DDK_VERSION_OS="n"` (`drivers/gpu/arm/Kconfig:23-30`); stock `SOC_EXYNOS7580=y` →
  visible; MIDGARD off cascades T7XX/R15P0 off (m0 verified).
- CMA helpers are **select-only**: `DRM_GEM_CMA_HELPER`/`DRM_KMS_CMA_HELPER`
  (`drivers/gpu/drm/Kconfig:58-71`) have no `depends` trigger a fragment can pull —
  only `select` from tilcdc/shmobile/imx (`depends on ARM/SUPERH/...`, all dead on
  arm64: tilcdc needs `ARM`, shmobile needs `SUPERH||ARCH_SHMOBILE`). Requesting
  `DRM_KMS_CMA_HELPER=y` in a fragment is silently reverted by `olddefconfig`
  (verified: present pre-closure, gone post-closure, `FB_SYS_*` stay `not set`),
  and `drivers/gpu/drm/exynos/*.c/*.h` contain **zero** `gem_cma/fb_cma` references
  (exynos carries its own `dma_alloc_attrs` GEM backend: `exynos_drm_gem.c`/`buf.c`).
  Hence no variant ships CMA helpers — deliberate, not an omission.

## 4. `/dev/dri/card0` with no modeset driver (why D0/D1/D3 work)

Unchanged M1 mechanism (`drivers/gpu/drm/drm_platform.c:42-110`):
`drm_get_platform_dev` runs `drm_fill_in_dev` + `drm_get_minor(PRIMARY)` **before**
`load()`; with only G2D bound (`manager==NULL` → `fine_cnt++`,
`exynos_drm_core.c:131-135`; G2D probe never fires — no `samsung,exynos5250-g2d`
node in stock DT, no board-device source on arm64) `load()` returns 0,
`exynos_drm_fbdev_init` early-returns 0 on `num_connector==0`, and the already-created
primary node survives. Expected: `card0` + `controlD64`, zero connectors/CRTCs,
`/dev/fb0` + X untouched, no DT change. D1 adds VIDI the same way ( registers
`exynos-drm-vidi` platform driver, `exynos_drm_vidi.c:660`; no device source in tree
→ `vidi_probe` never fires).

## 5. Size estimates vs the 32 MiB BOOT limit (`mmcblk0p10` = 33 554 432 B)

Anchors (measured, not modelled): stock `Image` 17 234 120 B → M1 `Image`
18 094 328 B (**+876 592 B, +5.1 %**, both from the #35-era tree state);
stock boot img 26 374 144 B (25.15 MiB) → M1 boot img 27 250 688 B (25.99 MiB),
headroom **6 303 744 B (5.99 MiB)**. The boot img ≈ `Image` + fixed
ramdisk (17 234 120 + ~9.1 MB) + `dt.img` (313 344 B) + page padding, so Image
deltas translate ~1:1 into boot-img deltas (M1 observed: +876 592 Image →
+876 544 boot).

Method: per-object `size -B` (M1 tree) for the always-on parts + source-byte
scaling for objects not built in the M1 tree, cross-checked against the M1
Image delta (the DRM core total `dec` 935 667 B vs +876 592 B Image delta —
link/padding absorbs ~60 KB; estimates below are therefore conservative-upper).

| Variant | Image Δ vs stock (est.) | Boot img (est.) | Headroom | Verdict |
|---|---|---|---|---|
| c0 (MODULES only) | `kernel/module.o` dec **28 655 B** → ~**+29 KB** | ~26 403 399 B (25.18 MiB) | ~7.15 MB | fits |
| d0 = M1 (measured) | **+876 592 B** (Image 18 094 328 B) | **27 250 688 B** (25.99 MiB) | **6.30 MB** | fits (built) |
| d1 (+VIDI) | d0 + `vidi.c` 668 lines ≈ ½× g2d (10 612 B dec) → ~**+5 KB** | ~27.00 MiB | ~6.30 MB | fits |
| d2 (full) | d0 + fimd+hdmi+mixer+ddc+hdmiphy+ipp+rotator+fimc src ≈ 278 KB ≈ ~**+150–200 KB** Image + `fbmon/video` helpers (fbcon OFF, so no `fbcon.o` 90 KB / fonts 147 KB) → ~**+1.1 MB** | ~28.3 MB (27.1 MiB) | ~4.9 MB | fits (boot only; **display-safety gate still applies** — FIMD double-drives DECON) |
| d3 (no MODULES) | d0 − module.o → ~**+848 KB** (Image ~18 065 673 B) | ~27 222 033 B | ~6.33 MB | fits |
| m0 (Mali off) | d0 − kbase `built-in.o` dec **2 229 532 B** → ~**−2.1 MB** (Image ~15.9 MB) | ~25.2 MB (24.0 MiB) | ~8.4 MB | fits (Kconfig only; functional cutover needs M2/M3) |
| m1 (= d0 + stock kbase) | same as d0 | 25.99 MiB | 6.30 MB | fits (stock kbase kept) |
| fbcon (rejected ref.) | +`fbcon.c` 90 KB + fonts 147 KB + `font.o`/tileblit ≈ **+250 KB** if ever enabled | — | — | NOT shipped (steals `/dev/fb0`) |

All variants clear the 32 MiB BOOT limit with ≥4.9 MB headroom, including the
maximal D2. CMA 32 MB (swap path) costs zero BOOT bytes (RAM reservation only).
No variant approaches the limit; sizes are informational, not gating.

## 6. Closure checker

`code/gpu31/config/check-matrix.sh` — merges each `*.fragment` (both `CONFIG_X=y`
and `# CONFIG_X is not set` lines; stock `.config` + fragment → `ARCH=arm64
olddefconfig`, read-only tree, private `$OUT`) and asserts per-variant invariants:

- c0: MODULES=y, DRM stays off. d0/m1: G2D=y, fbcon stays off.
- d1: VIDI=y on top of d0. d2: FIMD=y, **GSC absence** (proves the ARCH_EXYNOS5
  drop is caught, not silently assumed), fbcon still off.
- d3: MODULES stays off. m0: MIDGARD + T7XX both off.

Failure mode it guards: a fragment requesting an unsatisfiable symbol (GSC without
ARCH_EXYNOS5, CMA helpers without a selecting driver) would previously have passed
silently; the checker now asserts the *resolved* post-`olddefconfig` state, so drops
are explicit (d2/GSC) and reverts are documented (CMA helpers, §3).

## 7. Handoff

- Integrator: D0 (= proven M1 set) is the boot candidate; D2 needs the display-safety
  gate (FIMD vs vendor DECON double-drive) + the §7 single-`compatible` DT check when
  (and only when) the swap-path DT patch is in play. Never apply D0 and the
  swap-path fragment together (§9 alternative-path rule).
- M1Verify (after Main's go only): `card0` + `controlD64` present, zero connectors,
  `dmesg` exynos init, X 720×1280 — per `code/kernel/m1/m1-verify.sh`.
- M2/M3 owners: c0 proves the loader half of the kbase-shim path; m0 proves the
  Kconfig half of the cutover shape. Neither substitutes for the render-node patch
  (Mode B), the panfrost backport series fixes (review §2–§5), or the PRIME
  import-by-copy real fix (review §10).
