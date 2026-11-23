# DRM/KMS coexistence design — exynos7580 (j7elte, Nura 3.10.108)

> **Status 2026-11-23:** this is the earlier coexistence study. The M3
> decision, the applicable Kconfig patch and the rollback runbook are in
> `docs/reports/decon-kms-coexist.md` (slice `KmsCoexist`). Its key
> correction: the KMS driver must keep the existing compatibles and must not
> remove the vendor fbdev driver from the image; a boot parameter cannot
> switch modes on this device because `CONFIG_CMDLINE_FORCE=y`.

Scope: this directory ONLY. No change to `patches/drm/`, no DTS, no config,
no DTB payload. The S-Boot DT-payload rule is absolute: M1 must boot with the
stock DT and the vendor fbdev stack driving DECON/DSIM.

## 1. What the integrator's DRM driver does today

### 1.1 Probe path (patch `patches/drm/0001-...`, driver `exynos7580_drm.h/.c`)

New platform driver in `drivers/gpu/drm/exynos7580/`:

- `exynos7580_probe()` (patch offset ≈ line 1882 of the 0001 diff) is a
  one-liner: `return drm_platform_init(&exynos7580_drm_driver, pdev);`
- `exynos7580_remove()` calls `drm_platform_exit(&exynos7580_drm_driver, pdev)`.
- Match table (`exynos7580_drm_dt_match`, ≈ patch line 1893):
  `{ .compatible = "samsung,exynos7580-decon-drm" }`.
- Driver struct (`exynos7580_drm_driver`, ≈ patch line 1656):
  `.driver_features = DRIVER_MODESET | DRIVER_GEM`,
  `.load = exynos7580_load`, `.unload = exynos7580_unload`.
- `exynos7580_load()` (patch ≈ line 1680) powers the DISP domain, ioremaps
  DECON (`0x14830000`) and DSIM (`0x14800000`) resources from the bound
  platform device, programs DECON/DSIM/panel, registers CRTC/encoder/
  connector + CMA dumb buffers + fbdev emulation.

Key consequence: `drm_platform_init()` → `drm_get_platform_dev()` runs the
full modeset bring-up and `->load()` touches real DECON/DSIM registers. There
is no "probe without hardware" path — binding this driver means owning the
display pipe.

### 1.2 DT patch (patch `patches/drm/0002-...`)

For each of `exynos7580-j7elte_rev{00,04,06}.dts` the patch inserts, inside
the existing display nodes (no new node, no new reg/interrupt/clock data):

- `decon_fb@14830000`: `compatible = "samsung,exynos7580-decon-drm";`
- `dsim_0: dsim@14800000`: `compatible = "samsung,exynos7580-dsim";`

Stock tree (`arch/arm64/boot/dts/exynos7580.dtsi:1078-1079`) has

    decon_0: decon_fb@14830000 {
            compatible = "samsung,exynos5-decon_driver";

and the DSIM driver's match tables are
`drivers/video/exynos/decon_7580/dsim_drv.c:66`
(`samsung,exynos5-dsim`) and `:1572` (`samsung,exynos5-mipi-dsi`).
The vendor DECON table is
`drivers/video/exynos/decon_7580/decon_core.c:56`
(`samsung,exynos5-decon_driver`), driver `decon_driver` at `:4867`,
registered by `late_initcall(exynos_decon_register)` at `:4890`.

So the 0002 patch STEALS both devices from the vendor fbdev: after the patch
`of_match` binds `decon_fb` to `exynos7580_platform_driver` and `dsim_0` to
whatever claims `samsung,exynos7580-dsim`, and the vendor `decon_driver`
never probes them. Kconfig fragment `patches/drm/config-drm-exynos7580.fragment`
completes the flip by commenting out
`CONFIG_EXYNOS_DECON_EXYNOS7580 / CONFIG_EXYNOS_MIPI_DSI /
CONFIG_EXYNOS_DECON_LCD / CONFIG_PANEL_EA8061_DYNAMIC`.

### 1.3 3.10 DRM core: what minors exist

`drivers/gpu/drm/drm_platform.c:42-110` — `drm_get_platform_dev()`:

    :66-70  if (DRIVER_MODESET) drm_get_minor(dev, &dev->control, DRM_MINOR_CONTROL);
    :72-74  drm_get_minor(dev, &dev->primary, DRM_MINOR_LEGACY);
    :76-80  if (dev->driver->load) dev->driver->load(dev, 0);
    :83-88  if (DRIVER_MODESET) drm_mode_group_init_legacy_group(...);
    err_g3/g2 unwind primary then control.

There is NO `DRM_MINOR_RENDER` allocation anywhere in this file. Nothing in
3.10 `drm_platform.c`, `drm_pci.c`, or `drm_usb.c` calls
`drm_get_minor(..., DRM_MINOR_RENDER)`.

`drivers/gpu/drm/drm_stub.c`:

- `drm_minor_get_id()` (`:110-128`) already carves the id space:
  legacy 0–63, control 64–191 (`base 64, limit base+127`), render 128–383
  (`base 128, limit base+255`). The allocator supports render minors.
- `drm_get_minor()` (`:328-~400`) is fully type-generic: `kzalloc`,
  `new_minor->type = type`, `idr_replace`, sysfs add; legacy additionally gets
  `/proc/dri`. Any caller CAN allocate a render minor today.
- `drm_put_minor()` (`:405+`) handles all types.

`drivers/gpu/drm/drm_sysfs.c:499-531` — `drm_sysfs_device_add()`:

    :510-515  CONTROL -> "controlD%d", RENDER -> "renderD%d", else "card%d".

The `renderD%d` naming is already wired; only the allocation call is missing.

`include/drm/drmP.h`:

- `:308-312` permission bits: `DRM_AUTH 0x1`, `DRM_MASTER 0x2`,
  `DRM_ROOT_ONLY 0x4`, `DRM_CONTROL_ALLOW 0x8`, `DRM_UNLOCKED 0x10`.
  There is NO `DRM_RENDER_ALLOW` on 3.10 (upstream added it with the render-node
  rework in 3.12).
- `:983-986` minor types: `LEGACY 1, CONTROL 2, RENDER 3`.
- `struct drm_device` (`drmP.h` ≈ `:1209,131-132`): only two minor pointers
  exist — `struct drm_minor *primary;` (comment: "render type primary screen
  head") and `struct drm_minor *control;`. There is NO `dev->render` field.
  A render-node holder must live in driver-private data.
- Dispatcher `drivers/gpu/drm/drm_drv.c:436-439`:

      (ROOT_ONLY && !CAP_SYS_ADMIN) ||
      (AUTH && !authenticated) ||
      (MASTER && !is_master) ||
      (!CONTROL_ALLOW && minor->type == DRM_MINOR_CONTROL)  →  -EACCES

  i.e. control nodes may ONLY run ioctls flagged `DRM_CONTROL_ALLOW`;
  everything else is `-EACCES` on `controlD*`. There is no render-node
  exemption test — a `DRM_MINOR_RENDER` open falls through to the normal
  AUTH/MASTER checks.

ioctl flags that matter (`drm_drv.c:63-168`):
GЕМ close `DRM_UNLOCKED`; PRIME `HANDLE_TO_FD`/`FD_TO_HANDLE`
`DRM_AUTH|DRM_UNLOCKED` (`:140-141`, NO `DRM_CONTROL_ALLOW` → control-node
inaccessible); KMS read ioctls (`GETRESOURCES/GETCRTC/GETCONNECTOR/GETFB/...`)
carry `DRM_CONTROL_ALLOW|DRM_UNLOCKED`; mutating KMS (`SETCRTC/SETPLANE/
PAGE_FLIP/...`) carry `DRM_MASTER|DRM_CONTROL_ALLOW`; dumb-buffer
`CREATE/MAP/DESTROY_DUMB` carry `DRM_CONTROL_ALLOW|DRM_UNLOCKED`
(`:164-166`, so they work from EITHER node); GEM `FLINK/OPEN` need
`DRM_AUTH`.

`drm_fops.c:249-330` (`drm_open_helper`): every open gets `priv->minor` from
the idr, `authenticated = capable(CAP_SYS_ADMIN)`, first opener on a minor
becomes master (`:299-314`). Mastership is per-minor (`minor->master`), so a
render node has its own master domain — a render client never needs, and
never steals, the modeset master on `card0`.

## 2. Mode A — M1-safe: vendor fbdev keeps DECON/DSIM, DRM core only

Goal: `/dev/dri/card0` (+`controlD64`) exists for dumb-buffer/PRIME interop
tests while the panel stays on vendor fbdev and X keeps working.

Construction (code sketch only in §5; no existing file touched):

- A NEW, separate tiny platform driver in a NEW file (e.g. `coexist_stub.c`,
  its own Kconfig symbol, NOT `DRM_EXYNOS7580`) that calls
  `drm_platform_init()` with a driver struct whose `.driver_features` includes
  `DRIVER_MODESET | DRIVER_GEM` (so `drm_get_platform_dev()` allocates both
  control and legacy minors) but whose `.load` does NOT ioremap or program
  DECON/DSIM — it only sets up a CMA/GEM + dumb-buffer + PRIME surface with
  no CRTC, or defers CRTC registration entirely.
- It must bind to a NEW compatible that matches NO existing node (added only
  via a device-tree OVERLAY or `platform_device_register_simple()` from the
  stub's `module_init`, never by editing a shipped `.dts`), or to a disabled
  dummy node. It must NEVER match `samsung,exynos7580-decon-drm`,
  `samsung,exynos5-decon_driver`, `samsung,exynos5-dsim`, or
  `samsung,exynos5-mipi-dsi`.

Why it is safe:

- `drm_get_platform_dev()` with a no-HW `.load` performs no MMIO, takes no
  clocks/regulators/power domains, installs no IRQ handler — it cannot
  disturb the vendor DECON that is actively scanning out.
- `drm_sysfs_device_add()` creates `card0`/`controlD64`; X (fbdev backend)
  keeps `/dev/fb0`; no driver unbinds, no DT node is stolen.
- Dumb-buffer ioctls are `DRM_CONTROL_ALLOW`, GEM/PRIME ioctls usable from
  `card0` with normal AUTH semantics; PRIME `HANDLE_TO_FD` export lets a
  Mali/kbase userspace import a CMA buffer without the display pipe.

Limit: no modeset (no CRTC/encoder claims hardware), so compositors can
allocate/scan-import but cannot flip through DRM. That is exactly the M1
contract: `/dev/dri` exists, fbdev X still works.

## 3. Mode B — render node on 3.10

Goal: a `renderD128` node for unprivileged GPU clients (Mali/kbase shim or
later panfrost) decoupled from modeset mastership.

### 3.1 Exact insertion point

`drivers/gpu/drm/drm_platform.c`, inside `drm_get_platform_dev()`, after the
primary-minor allocation (`:72-74`) and — decision below — before or after
`->load()`. Concretely, after line 74 (`ret = drm_get_minor(dev,
&dev->primary, DRM_MINOR_LEGACY); if (ret) goto err_g2;`) insert:

    /* render node: no master semantics needed, see §3.2 */
    ret = drm_get_minor(dev, &dev->render /* driver-private ptr, NOT dev */,
                        DRM_MINOR_RENDER);
    if (ret)
            goto err_g2;   /* unwinds primary; control unwound at err_g2 */

with matching `drm_put_minor()` calls added on the `err_g3` path and in
`drm_platform_exit()` alongside the existing `drm_put_minor(&dev->primary)`
(`:100-104`). Because `struct drm_device` has no `->render` member, the
`struct drm_minor *` must be stored in the driver's private struct (and
looked up for open via the idr, which `drm_open_helper` already does by
minor id — no open-path change needed). `drm_minor_get_id()` (`drm_stub.c
:118-120`) will hand out 128..383 and `drm_sysfs_device_add()`
(`drm_sysfs.c:512-513`) will name it `renderD128`.

Simplest correct ordering: allocate render AFTER a successful `->load()` and
`drm_mode_group_init_legacy_group()` (`:83-88`), so a failed display bring-up
does not leave a render node pointing at a dead device; unwind it first on
`err_g3`. Either order works; what matters is that every error leg releases
exactly the minors already allocated.

### 3.2 Permission-flag analysis (why `DRM_UNLOCKED` and NOT a new bit)

Constraint: 3.10 has no `DRM_RENDER_ALLOW`. The ONLY node-type gate in the
dispatcher is drm_drv.c:439:

    (!(flags & DRM_CONTROL_ALLOW) && minor->type == DRM_MINOR_CONTROL)

A render minor is neither legacy nor control, so this clause never fires for
it: EVERY ioctl is reachable from `renderD*` subject only to its AUTH/MASTER/
ROOT_ONLY bits. The design question is therefore purely which flags the
GPU-buffer ioctls carry:

- Mark render-reachable ioctls `DRM_AUTH | DRM_UNLOCKED` (mirror the existing
  PRIME entries `:140-141`). `DRM_AUTH` keeps the authenticated-client
  requirement (root-authenticated on open, or via `AUTH_MAGIC`); `DRM_UNLOCKED`
  avoids the global-mutex serialization for buffer submission. Do NOT set
  `DRM_MASTER` (would force render clients to be modeset master — defeats the
  purpose) and do NOT set `DRM_CONTROL_ALLOW` (irrelevant off the control
  node; harmless but misleading — leave it off so a later `DRM_RENDER_ALLOW`
  backport has a clean grep surface).
- KMS ioctls already carry `DRM_MASTER` for mutating paths; a render client
  calling `SETCRTC`/`PAGE_FLIP` fails with `-EACCES` via the MASTER check,
  which is the desired "render cannot modeset" property WITHOUT any new flag.
  Read-only KMS (`GETRESOURCES` etc., `DRM_CONTROL_ALLOW`) remains callable
  from a render node — harmless (read-only) and matches upstream render-node
  behavior of that era.
- `DRM_ROOT_ONLY` paths (SET_UNIQUE, ADD_MAP, AGP, SG) stay root-only
  regardless of node; no change.

What NOT to do: do not invent a local `DRM_RENDER_ALLOW 0x20` bit and gate the
dispatcher on it. Upstream's bit (3.12) inverts the test (`!RENDER_ALLOW &&
minor->type == RENDER → -EACCES`); bolting an inverted test onto 3.10's
`drm_ioctl()` changes the ABI contract of every existing ioctl table entry
and risks locking non-GPU ioctls out of `card0`. The 3.10-safe equivalent is
achieved with existing bits alone, as above.

Caveat (must be in the backport): `drm_gem_open`/`drm_gem_flink` carry
`DRM_AUTH`; an unauthenticated render client gets `-EACCES` until it does the
`AUTH_MAGIC` dance. That is acceptable and matches upstream render semantics
(authenticated, just not master).

## 4. Mode C — full KMS later (NOT M1)

The flip happens in two coupled places, and ONLY together:

1. DT: apply the 0002 change — `decon_fb@14830000` gains
   `compatible = "samsung,exynos7580-decon-drm"` (and `dsim_0` gains
   `samsung,exynos7580-dsim`), replacing the vendor compatibles
   (`samsung,exynos5-decon_driver` on `exynos7580.dtsi:1079`, etc.).
2. Kconfig: apply `config-drm-exynos7580.fragment` — `CONFIG_DRM_EXYNOS7580=y`
   plus `# CONFIG_EXYNOS_DECON_EXYNOS7580 is not set` (and DSI/LCD/panel off).

Only then does `exynos7580_probe → drm_platform_init → exynos7580_load` own
DECON/DSIM, and the vendor `decon_driver` (`late_initcall`) finds no node to
bind.

Why M1 must NOT flip it:

- The DRM `->load()` path programs the panel/LCD timings from its own copy of
  the ea8061 sequences; any deviation from the vendor sequence = black panel
  with no fbdev fallback, and X loses `/dev/fb0` the moment the vendor driver
  is compiled out.
- S-Boot consumes the shipped DT payload: a rebuilt `dt.img` (which the 0002
  patch would require) is a brick risk, hence the absolute DT rule. Mode C
  waits until the panel path is validated on an un-flashed bring-up rig, with
  a recovery path (Mode A kernel) in hand.

## 5. M1 recommendation and optional sketch

RECOMMENDED M1: Mode A. Vendor fbdev stays (no DT change, no fragment
`is not set` lines applied — only the `CONFIG_DRM=y` core plus the new stub
symbol), `/dev/dri/card0`+`controlD64` appear for dumb-buffer/PRIME interop,
X on fbdev keeps working, GPS-at-stock-parity untouched (no shared power
domain touched), and the phone stays flashable/bootable either way because no
shipped file changes.

OPTIONAL SKETCH (new files only, touches no existing-file semantics): a stub
platform driver `patches/drm-kms-coexist/coexist_stub.c` (plus its own
`Kconfig.coexist` fragment consumed by the integrator's build glue, NOT by
editing `drivers/gpu/drm/Kconfig`) that:

- defines `coexist_drm_driver` with `.driver_features = DRIVER_MODESET |
  DRIVER_GEM`, `.load = coexist_load` where `coexist_load()` registers dumb/
  PRIME helpers and returns 0 WITHOUT ioremap/clk/IRQ;
- registers via `platform_device_register_simple("drm-coexist-stub", -1, NULL,
  0)` + `drm_platform_init()` in `module_init`, undone in `module_exit`;
- binds to NOTHING in any shipped DT (the platform_device is created
  in-code), so `samsung,exynos7580-decon-drm` and all vendor compatibles are
  unaffected and no `dt.img` is produced.

This sketch is illustrative; the integrator decides whether to productize it
as an M1 fragment or keep M1 to core-DRM-only closure in a private `/tmp`
output dir. Either way, Mode B's `drm_get_minor(..., DRM_MINOR_RENDER)` edit
lands in Mode B/C work, never in M1.

## 6. Verification (integrator-owned device/flash; worker ran none)

- M1: `ls /dev/dri` shows `card0 controlD64`, no `renderD*`; `modetest`/`drmtest`
  dumb-buffer create+map succeeds; X starts on fbdev; `dmesg` shows no DECON
  probe conflict (only vendor `decon_driver` bound).
- Mode B: `renderD128` appears; unprivileged PRIME import + GEM map works while
  a modeset client holds `card0` master; `SETCRTC` from the render fd returns
  `-EACCES`.
- Mode C (later rig only): vendor DECON absent from `dmesg`, DRM `exynos7580`
  bound, panel lights with fbdev emulation, then X on modesetting driver.
