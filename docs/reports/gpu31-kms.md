# gpu31-kms — Mode B render-only minor (M2), no KMS takeover

Date: 2026-11-19. Owner: G31_Kms. Scope: exynos DRM/KMS on the 3.10 vendor
tree; Mode A/B/C design per `patches/drm-kms-coexist/DESIGN.md`.
Deliverable: patch `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch`
+ this report. Live device state (locked SSH, 2026-11-19): phone **UP**,
kernel `3.10.108 #35 SMP PREEMPT`, `/dev/mali0` present, `/dev/dri`
**absent** (running kernel predates M1 — expected). Reports claiming
"link down" are stale (that outage predates this slice).

## 1. What Mode B is (and is not)

- **Mode A (M1, already built):** vendor DECON fbdev keeps the panel;
  `card0`+`controlD64` via exynos+G2D; no display-driver swap.
- **Mode B (this slice):** ADD a `renderD128` render node next to Mode A
  for unprivileged GPU clients (Mali/kbase shim now, panfrost later).
  Decoupled from modeset mastership. No KMS takeover.
- **Mode C (full KMS, later):** separate exynos7580 DECON/DSIM + panel
  patches (`code/gpu31/series/0001-...-kms`) PLUS the DT compatible flip
  PLUS the vendor-fbdev Kconfig off-switches, applied together. None of
  that is in this patch.

## 2. Patch map (5 files, tree untouched)

`code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch`
(+37/−2; `git apply`-style, `patch -p1 --dry-run` clean, §4):

| # | File / hunk | What changes |
|---|---|---|
| 1 | `include/drm/drmP.h` (`struct drm_device`) | Add `struct drm_minor *render` holder + comment. 3.10 has no `dev->render` field (only `primary`/`control`, `drmP.h:1208-1209`); the minor pointer must live somewhere, and the device struct keeps the platform/stub unwind paths symmetric with primary/control (no exynos driver patch needed). |
| 2 | `drivers/gpu/drm/drm_platform.c` (`drm_get_platform_dev`) | `drm_get_minor(dev, &dev->render, DRM_MINOR_RENDER)` after primary, BEFORE `->load()`; new `err_g4` releases render, `err_g3` primary, `err_g2` control. pci (`drm_pci.c`) / usb (`drm_usb.c`) intentionally untouched: M1/M2 bind only the exynos platform device; mirror there only if a future x86/qemu bring-up needs it. |
| 3 | `drivers/gpu/drm/drm_stub.c` (`drm_put_dev` / `drm_unplug_dev`) | NULL-guarded `drm_put_minor(&dev->render)` / `drm_unplug_minor(dev->render)` alongside primary/control (dev is kzalloc'd; `drm_get_minor` NULLs on failure). |
| 4 | `drivers/gpu/drm/drm_fops.c` (`drm_open_helper`) | Render opens are always authenticated (upstream render semantics of the era: authenticated, just never master). Card0 unchanged (AUTH_MAGIC dance for non-root). |
| 5 | `drivers/gpu/drm/drm_drv.c` (`drm_ioctl` dispatcher) | Explicit deny: any `DRM_MASTER`-flagged ioctl on a render node → `-EACCES`. REQUIRED, not belt-and-braces: mastership is per-minor (`drm_open_helper` creates `minor->master` on first open of EACH node, `drm_fops.c:299-314`), so the first render opener *is* master of the render minor and would pass the plain `DRM_MASTER && !is_master` test. With this clause, SETCRTC/SETPLANE/CURSOR/ATTACH/DETACH/SETPROPERTY/PAGE_FLIP/DIRTYFB/OBJ_SETPROPERTY all fail on renderD128: render cannot modeset, with no new flag. The existing `!CONTROL_ALLOW && type == CONTROL` gate is untouched. |

## 3. Design decisions (why this shape)

- **Device-field holder, not driver-private:** keeps `drm_platform_exit`
  / `drm_put_dev` / `drm_unplug_dev` symmetric; no `exynos_drm_drv.c`
  change; G31_DrmCore confirmed sole-writer status (their shims are
  new-files-only: `drm_compat_310.h`, `panfrost_ioctls_310.h`).
- **Render alloc BEFORE `->load()`:** a failed display bring-up unwinds
  the node too; every error leg releases exactly the minors allocated.
- **Auth flags for 3.10 (`DRM_AUTH|DRM_UNLOCKED`, no new bit):** 3.10 has
  no `DRM_RENDER_ALLOW` (`drmP.h:308-312` has only AUTH/MASTER/ROOT_ONLY/
  CONTROL_ALLOW/UNLOCKED). Buffer ioctls (GEM CREATE/MAP, PRIME
  HANDLE_TO_FD/FD_TO_HANDLE: `DRM_AUTH|DRM_UNLOCKED`) are therefore
  render-reachable; mutating KMS stays `DRM_MASTER`-gated; read-only KMS
  (`DRM_CONTROL_ALLOW|DRM_UNLOCKED`) stays callable from the render node
  (harmless, matches era render semantics). No dispatcher ABI change for
  card0/controlD64 paths. In-tree exynos ioctls (`EXYNOS_GEM_*`,
  `EXYNOS_G2D_*`, `EXYNOS_IPP_*`: all `DRM_UNLOCKED|DRM_AUTH`,
  `exynos_drm_drv.c`) are likewise render-reachable once a consumer binds
  the node — the intended M2 path.
- **No fbdev conflict (by construction):** no DT/dts edit, no compatible
  matched (`samsung,exynos7580-decon-drm/-dsim`, `samsung,exynos5-*`
  untouched), no DECON/DSIM MMIO, no clocks/regulators/power domains, no
  IRQ, no CRTC/encoder/connector claim. Vendor `decon_driver`
  (late_initcall) keeps its nodes; `/dev/fb0` + fbdev X unaffected.
- **Minor-number source:** existing 3.10 carve-out
  (`drm_minor_get_id`: control 64–191, render 128–383) → first render node
  is minor 128 → `renderD128` via the already-wired `drm_sysfs.c`
  `renderD%d` naming. Only the allocation call was missing.

## 4. Compile check (host-side, read-only tree — no `make` in shared tree)

- `patch -p1 --dry-run`: clean on scratch copies of all 5 files
  (fuzz ≤2 on context; integrator re-runs `git apply --check`).
- Recorded-kbuild-flags `gcc -fsyntax-only` + real `-c` object compile vs
  actual 3.10.108 headers: **all 4 patched `.c` files pass with zero
  warnings** (drmP.h is covered through inclusion):

| File | syntax-only | `-c` object |
|---|---|---|
| `drm_platform.c` | OK | OK (1676 B text+data+bss delta domain) |
| `drm_stub.c` | OK | OK |
| `drm_drv.c` | OK | OK |
| `drm_fops.c` | OK | OK |

  Flags:[kbuild `.cmd` files, `KBUILD_BASENAME`/`KBUILD_MODNAME` defines
  included; first attempt without them failed on `MODULE_PARAM_PREFIX`
  (pre-existing tree property, unrelated to this patch), re-ran with the
  recorded defines — documented so the integrator reproduces exactly.
- Note: baseline (unpatched) `-c` compiles were also run to validate the
  flag set before testing patched sources.

## 5. On-device verification (M1Verify-owned, post-M1-flash)

Expected (asserts for the M1Verify runbook, NOT run in this slice — no
flash/reboot per slice rules; GPS/FM share the phone):

1. `ls /dev/dri` → `card0 controlD64 renderD128` (minor 128).
2. Unprivileged `PRIME HANDLE_TO_FD` + GEM map from the render fd while a
   modeset client holds `card0` master → success, no master stolen
   (per-minor master domains).
3. `SETCRTC` / `PAGE_FLIP` from the render fd → `-EACCES`.
4. X on fbdev still 720×1280; `/dev/fb0` present; no DECON probe conflict
   in `dmesg` (only vendor `decon_driver` bound).
5. `render-node-probe /dev/dri/renderD128` → H1/H2 classification decides
   the Mesa path (`gpu-drm.md` M2).

## 6. What remains for KMS (explicit)

1. **M1 gate (blocking):** Integrator builds; M1Verify flashes only after
   Main's explicit go (`gpu31-master.md` §6), then `m1-verify.sh` /
   `run-m1.sh` (card0+controlD64, dmesg exynos init, X 720×1280).
2. **This patch lands** (integrator applies + builds; M1Verify asserts §5).
3. **Mode B consumers:** kbase-shim render-fd adoption (H2) or panfrost
   UAPI on the node (H1) per `gpu-drm.md` M2 + `ADAPT.md`.
4. **Mode C (NOT this slice):** DECON/DSIM/panel driver + DT compatible
   flip + vendor-fbdev Kconfig off-switches + `dt.img` question (stock
   DT rule still absolute) + panel-light validation with fbdev fallback.
5. **M3:** backport series → panfrost GEM/job-slot driver (the honest gap)
   → Mesa build → glmark harness (`run-m3.sh`, THRESHOLDS.md).

## 7. Coordination

- G31_DrmCore: sole-writer handshake done (no competing hunks; their
  headers `#include` the render allocator after this field lands).
- Slice rules honored: shared tree untouched (scratch copies only for
  `--dry-run`/`-c`); no build/flash; no reboot; short `flock` holds only;
  big files under `/tmp`+`/data`, none added here.
