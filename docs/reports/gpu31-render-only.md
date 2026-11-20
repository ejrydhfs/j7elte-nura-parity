# gpu31 render-only — no card0, only renderD128 (X-probe fix)

Date: 2026-11-20. Owner: G31_RenderOnlyFix.
Target: Mode-B patch + driver patches (`drm_get_platform_dev` path,
`driver_features`, minor allocation).
Deliverable: v2 core patch
`code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch` (rewrite) +
driver 0001 companion change (`DRIVER_RENDER_ONLY` in `driver_features` +
header comments) + this report. No shared-tree edits; patches only.
Device state per Main (ground truth, not re-probed): int4 image booted
with BOTH `/dev/dri/card0` and `renderD128`; Xorg auto-probes card0 and
deadlocks in D state (blank screen, cursor dot); phone reverted to M1
(working UI, no `/dev/dri`).

## 1. Problem (why v1 is unshippable)

v1 of the Mode-B patch allocated renderD128 IN ADDITION TO the primary
minor on the same `drm_device`: `drm_get_platform_dev` ran the legacy
`drm_get_minor(dev, &dev->primary, DRM_MINOR_LEGACY)` unconditionally
(pristine `drm_platform.c:72`) and then added
`drm_get_minor(dev, &dev->render, DRM_MINOR_RENDER)`. The panfrost7580
device therefore registered `/dev/dri/card0` (plus exynos/G2D's own
card0 on another device). Xorg's platform probe enumerates
`/dev/dri/cardN` only (never render nodes); with a panfrost card0
present it binds, issues modeset ioctls against a node with no
CRTC/encoder/connector, and wedges in D. The fix is render-ONLY
registration: the panfrost7580 device must create NO primary/control
minor at all — no `/dev/dri/card0`, no `controlD64` — only
`/dev/dri/renderD128`.

## 2. Allocation point (where the primary minor comes from)

Vendor 3.10 stub path, `drivers/gpu/drm/drm_platform.c`,
`drm_get_platform_dev()` — the ONLY attach path the driver uses
(`panfrost7580_main.c` probe calls `drm_platform_init()`, which calls
it; confirmed in int4 `vmlinux` symbols per `gpu31-integration4.md` §5):

- pristine `drm_platform.c:66-70`: `drm_get_minor(dev, &dev->control,
  DRM_MINOR_CONTROL)` iff `DRIVER_MODESET`.
- pristine `drm_platform.c:72-74`: `drm_get_minor(dev, &dev->primary,
  DRM_MINOR_LEGACY)` UNCONDITIONALLY — this is the line that creates
  card0 for every platform DRM device, including panfrost7580 (which
  sets no `DRIVER_MODESET` but still gets a primary minor).
- `drm_stub.c drm_minor_get_id`: control carve-out 64..191, legacy
  0..63, render 128..383. The render carve-out is wired end to end
  (`drm_sysfs.c` `renderD%d` naming); only the allocation call was
  missing — v1 added it, v2 makes it the ONLY allocation.

`drm_get_minor` (`drm_stub.c:328`) takes an explicit `type`: the core
decides which minors exist, not the driver. So the correct gate is a
driver-features bit checked in `drm_get_platform_dev`, not driver-side
tricks (the driver never touches `dev->primary`).

## 3. Diff (v2 core patch + driver companion)

New bit: `DRIVER_RENDER_ONLY 0x8000` (`include/drm/drmP.h`). Bit survey
of pristine `drmP.h:144-158`: used bits are `0x1..0x4000`
(`DRIVER_PRIME` is the top at `0x4000`); `0x8000` is free, no other
`DRIVER_*` bit uses it.

Core patch (5 files, `git apply`-style, `patch -p1 --dry-run` clean
from a tree root, §5):

| # | File / hunk | Change |
|---|---|---|
| 1 | `include/drm/drmP.h` | `DRIVER_RENDER_ONLY 0x8000` + comment; `struct drm_minor *render` holder + comment (3.10 has no `dev->render`; keeps unwind symmetric, no exynos patch). |
| 2 | `drivers/gpu/drm/drm_platform.c` (`drm_get_platform_dev`) | `DRIVER_RENDER_ONLY` → allocate ONLY `dev->render` via `DRM_MINOR_RENDER` BEFORE `->load()`; legacy path (control iff MODESET, then primary) byte-identical otherwise. MODESET group setup + primary-based `DRM_INFO` skipped on render-only (render-index `DRM_INFO` instead). Unwind `err_g3` releases render/primary respectively; `err_g2` releases control only on non-render-only MODESET. |
| 3 | `drivers/gpu/drm/drm_stub.c` (`drm_put_dev` / `drm_unplug_dev`) | Render-only: release/unplug `->render` (NULL-guarded; `dev` is kzalloc'd, `drm_get_minor` NULLs on failure). Legacy put/unplug of primary/control untouched, with the MODESET-gated control release additionally gated on `!RENDER_ONLY`. |
| 4 | `drivers/gpu/drm/drm_fops.c` (`drm_open_helper`) | Render opens always authenticated (upstream render semantics of the era: authenticated, just never master). Card0 unchanged (AUTH_MAGIC dance). |
| 5 | `drivers/gpu/drm/drm_drv.c` (`drm_ioctl` dispatcher) | Explicit deny of any `DRM_MASTER`-flagged ioctl on a render node (`-EACCES`), even from the render-minor master. REQUIRED: mastership is per-minor (`drm_open_helper` creates `minor->master` on first open of EACH node), so the first render opener IS master of the render minor and would pass the plain `DRM_MASTER && !is_master` test. |

Driver companion (`code/gpu31/driver/0001-*.patch`, 3 lines + comments):

```c
.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY,
```

(no `DRIVER_MODESET`, as before). The two header comments
(`panfrost7580_main.c` file banner, `panfrost7580_drv.c` banner) now say
"no primary minor / ONLY renderD128 / no cardN". No other driver change:
the core branches on the bit; `dev->primary` is never dereferenced by
the driver. Legacy 3.10 users that assume `dev->primary != NULL`
(`drm_bufs.c` addmap paths, `drm_sysfs.c` connector/hotplug helpers)
are unreachable for this driver: no `DRM_IOCTL_ADD_MAP`/legacy-map
ioctl is registered, no connector is created, `drm_sysfs_hotplug_event`
is never called.

Diff vs v1: v1 = render IN ADDITION TO primary (both nodes, X wedges).
v2 = render INSTEAD OF primary for `DRIVER_RENDER_ONLY` devices only;
all other devices (exynos/G2D MODESET path, pci/usb untouched) keep the
byte-identical legacy path. Hunks 1/4/5 kept from v1 in intent;
hunk 2 rewritten as a branch; hunk 3 hardened for the missing primary.

## 4. Why card0 is absent (and why X cannot find the device)

With `DRIVER_RENDER_ONLY` set, `drm_get_platform_dev` never calls
`drm_get_minor(..., DRM_MINOR_LEGACY, ...)` for this device, so no
primary idr slot is consumed and `drm_sysfs_device_add` never creates a
`card%d` node — there is no `/dev/dri/cardN` belonging to panfrost7580
at all. `drm_sysfs.c` names nodes by minor type (`controlD%d` /
`renderD%d` / `card%d`), so the only node is `renderD128` (first free
render slot, 128). Xorg's platform probe scans `cardN` nodes only; it
can never open `renderD128` by probing. Result: the probe finds only the
M1-shape exynos/G2D card0 (or nothing), Xorg falls back to the pinned
fbdev driver (`j7elte-gpu-soft/10-fbdev.conf`, `Driver "fbdev"` on
`/dev/fb0`) — the proven M1 UI path. Permission gating is unchanged
from v1: render opens are authenticated but can never be modeset master
(MASTER ioctls → `-EACCES`), so even a direct render open cannot
modeset.

## 5. Apply-check + compile evidence (host-side, read-only tree)

- `patch -p1 --dry-run` from a tree root holding pristine copies of the
  5 files: v2 core patch **clean, 5/5 files, rc=0** (v1 convention,
  `a/`-rooted, same as the file it replaces); driver 0001 with the
  companion edit **clean, 17/17 files, rc=0**.
- Recorded-kbuild-flags `gcc -fsyntax-only` + real `-c` object compile
  vs actual 3.10.108 headers (pristine tree + patched `drmP.h` overlay,
  `-nostdinc`, `KBUILD_BASENAME`/`KBUILD_MODNAME` defines from the
  int4 `.cmd` files): **all 4 patched `.c` files pass with zero
  warnings** (`drmP.h` covered through inclusion):

| File | syntax-only | `-c` object |
|---|---|---|
| `drm_platform.c` | OK | OK (167040 B) |
| `drm_stub.c` | OK | OK (204960 B) |
| `drm_drv.c` | OK | OK (205296 B) |
| `drm_fops.c` | OK | OK (208640 B) |

  Note: the overlay puts the patched `drmP.h` first in `-I` order, so
  `DRIVER_RENDER_ONLY` resolves while every other header is pristine.

## 6. Boot-path render-node audit (nothing else opens it)

- Xorg is pinned to fbdev (`10-fbdev.conf`: `Driver "fbdev"`, `fbdev
  "/dev/fb0"`); it never names `/dev/dri/renderD128`. Its only DRI
  contact is the cardN platform probe, which cannot match a device with
  no card node.
- No udev rule, init script, or in-tree userspace component references
  `renderD128` (repo-wide grep: only probe/selftest tools
  — `code/gpu31/probe/*`, `code/gpu31/client/*`,
  `code/userspace/gpu/mesa-render/*` — name it, and all open it
  explicitly, off-boot).
- Expected steady state after boot: `/dev/dri/renderD128` exists,
  unopened, until the first GPU client (Mesa / kbase-shim / selftest)
  opens it. No boot service holds it.

## 7. On-device asserts (M1Verify-owned, post-next-build; NOT run here)

1. `ls /dev/dri` → `renderD128` present, NO `card0`/`card1` from
   panfrost (exynos/G2D card0 may be present per M1 shape — that is the
   M1 node, unaffected).
2. Xorg up at 720x1280 on fbdev; no D-state Xorg; `/dev/fb0` present.
3. Unprivileged `PRIME HANDLE_TO_FD` + GEM map from the render fd works.
4. `SETCRTC` / `PAGE_FLIP` from the render fd → `-EACCES`.
5. `render-node-probe --probe-only /dev/dri/renderD128` classifies the
   node (H1/H2 path per `gpu-drm.md` M2).

## 8. Coordination

- Peer regions (no overlap, confirmed over IRC): G31_SubmitFix2 owns
  driver-0001 submit/job/mmu hunks + `submit_fix_test` + submit report
  (landed c553b8e → e4edb3a); G31_OpenFix owns driver-0001 submit
  `sched_ensure` + main.c probe warm-up + open report (landed b199c8f
  → 20ea233). This ticket owns ONLY the kms v2 rewrite + the driver-0001
  `.driver_features` line + header comments + this report. Rebased onto
  20ea233; the `.driver_features` anchor line is untouched by both
  peers (their hunks are in submit.c file-init and main.c probe).
- Slice rules honored: shared tree untouched (scratch copies under
  `/tmp` only); no build/flash/reboot; no device contact; short reads
  only against the external-disk pristine tree.
- Follow-ups (not this ticket): SUBMIT/open hangs (peers), int5
  integration build + flash request (Integrator/Main + M1Verify).
