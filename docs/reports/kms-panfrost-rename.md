# Render driver rename: `panfrost7580` → `panfrost` (DRM name + platform driver)

Status 2026-11-25. Owner slice: `PanfrostName`. Follows
`docs/reports/kms-mesa-pairing.md` §5 item 1 (the name is the only thing that
blocks the kmsro join; every other gate is a version/ABI gate) and the KWin 6.8
uevent finding (Main: `DRIVER=panfrost7580` in
`/sys/dev/char/226:128/device/uevent` → unknown-name NULL lookup → SIGSEGV).

Search-key evidence for the whole ticket (all paths relative to the repo root):

```sh
grep -n '\.name = "panfrost' code/gpu31/driver/0001-panfrost7580-driver-sources.patch
# 5856:  .name = "panfrost"   <- drm_driver.name   (Mesa/kmsro + libdrm loader)
# 6301:  .name = "panfrost"   <- platform_driver.driver.name (uevent DRIVER=, KWin)
grep -n 'MODULE_ALIAS' code/gpu31/driver/0001-panfrost7580-driver-sources.patch
# 6313:  MODULE_ALIAS("platform:panfrost")   (must track .driver.name)
```

---

## 1. Change

Three lines in `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`,
inside the `panfrost7580_main.c` new-file hunk:

```diff
 	.fops = &p7580_drv_fops,
-	.name = "panfrost7580",
+	.name = "panfrost",
 	.desc = "Mali-T720 render-only (Exynos 7580)",
 	.date = "20261119",
 ...
 	.driver = {
-		.name = "panfrost7580",
+		.name = "panfrost",
 		.owner = THIS_MODULE,
 ...
-MODULE_ALIAS("platform:panfrost7580");
+MODULE_ALIAS("platform:panfrost");
```

Two namespaces, two consumers:

* `drm_driver.name` (patch line 5856) — what `DRM_IOCTL_VERSION` returns:
  Mesa 20/26 kmsro pairing and the libdrm/Mesa DRI-module lookup.
* `platform_driver.driver.name` (line 6301) — what `drivers/base/core.c:308`
  writes as `DRIVER=` in the device uevent and what names
  `/sys/bus/platform/drivers/…`: KWin 6.8's DRM backend reads
  `/sys/dev/char/226:128/device/uevent`, and an unknown driver string is a NULL
  table lookup. `MODULE_ALIAS("platform:…")` must match `.driver.name` or
  modalias autoload stops matching (module file name stays `panfrost7580.ko`).

* driver 0001 sha256 before this ticket: `e74746706e2320a7e4dfff8d978473bec5bc3cadbfa7e48068abd586bfc4be48`
* sha256 after the `drm_driver.name` change (`2d19f91`): `aa710ebb23d0d57b289259504f65a30f346b134ec6c87b38d428b61f6f0dab15`
* sha256 after the platform-driver change: `082dea4a002bdcbdb1d5f489bda869ab4e8b0adad659eeb4768f85dbe834ab58`

Keeping the change inside driver 0001 (instead of a follow-up patch under
`patches/drm-kms/`) is deliberate: the KMS series does not apply the GPU
driver, so a `patches/drm-kms/` follow-up would never reach the render node;
driver 0001 is the single artifact every GPU image and any future combined
image applies.

The `+1,760` hunk count is unchanged (`+` lines changed text, not the count),
so the int4x "new-file hunk count" gate still passes — verified with that
gate's own awk (§5).

### Deliberately unchanged

| Symbol / string | Location | Why kept |
|---|---|---|
| module/object name `panfrost7580.ko`, `panfrost7580-y` | 0002 build wiring | module file name; `insmod`/ramdisk naming and the int4x gates |
| `CONFIG_DRM_PANFROST7580` | 0002 Kconfig | config symbol; every build script and fragment uses it |
| `.compatible = "arm,mali-t720"` / `"arm,mali"`, `MODULE_DEVICE_TABLE(of)` | 0001 main.c:439-446 | DT matching; OF match is what binds the node, independent of the driver name |
| device name `11400000.mali` | DT/of_device_make_bus_id | no DT edit; `dev_name()` and `/sys/devices/platform/11400000.mali` unchanged |
| `MODULE_DESCRIPTION("panfrost7580 render-only DRM driver …")` | 0001 main.c:758 | module metadata only; not in uevent/sysfs driver matching |
| debugfs `p7580/` (jobs/perf slices), `dri/128/` | job slice / DRM core | debugfs paths |
| IRQ action labels `panfrost7580-job/-mmu/-gpu` | 0001 main.c:6169-6177 | `/proc/interrupts` labels; docs quote them |
| `get_driver_name()` `"panfrost7580"`, timelines `"panfrost7580-js-N"` | 0001 job.c / `code/gpu31/jobs/panfrost7580_job.c:226` | dma_fence debug labels; no kernel, Mesa or KWin consumer |
| `drm_sched_init(..., "panfrost7580")` | 0001 main.c:8738 | workqueue/thread label only |
| message tags `"panfrost7580: MMU fault …"` | 0001 mmu/IRQ paths | log text matched by `docs/reports/gpu31-jobtrace.md`; not a driver name |

---

## 2. Audit — what keys on each name

`drm_driver.name` is consumed only through `DRM_IOCTL_VERSION` in this tree
(`drivers/gpu/drm/drm_drv.c:349-350` copies `dev->driver->name`); no in-tree
kernel code compares it, and the only other reader is the platform-bus init
print (`drm_platform.c:95`, patched by Mode-B). `drm_irq.c:355` also reads it,
but this driver installs its own IRQs with explicit labels.

`platform_driver.driver.name` is the *device-model* name: `drivers/base/core.c:308`
writes it as `DRIVER=` into the device uevent, it names `/sys/bus/platform/drivers/<name>`
and the `<device>/driver` symlink, and `dev_driver_string()` (`core.c:73-86`)
uses it as the prefix of every `dev_*` print.

| # | Consumer | Mechanism | Before | Needs | After rename |
|---|---|---|---|---|---|
| 1 | Mesa 20.3.5 kmsro | `drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER)` → exact `drmGetVersion()->name` match on every render node | `panfrost7580` | `panfrost` | **fixed** (gate 2 of the pairing report) |
| 2 | Mesa 26.3.0 platform probe (kmsro) | render nodes whose `drmGetVersion()->name` ∈ {`panfrost`, `panthor`, `lima`, … } | `panfrost7580` | `panfrost` | **fixed** (gate 2′) |
| 3 | Mesa 26 panfrost screen | `pan_kmod_drm_drv_match(fd, "panfrost")` **and** version ≥ 1.1 | name wrong, 1.0 | name + 1.1 | name gate **fixed**; version 1.0 and the 6.x `PANFROST_CREATE_BO` ABI still block Mesa 26 (unchanged, separate tickets) |
| 4 | Mesa/libdrm DRI loader | render node → `<name>_dri.so` | `panfrost7580_dri.so` (absent) → llvmpipe | `panfrost_dri.so` | **fixed**; `MESA_LOADER_DRIVER_OVERRIDE=panfrost` in `code/gpu31/kms/tools/env.sh` becomes redundant (harmless) |
| 5 | DRM core init print | `DRM_INFO("Initialized %s %d.%d.%d %s on minor %d (render-only)")` (Mode-B `drm_platform.c` hunk) | see §4 | — | **dmesg text change** (§4) |
| 6 | `debugfs dri/128/name`, `/proc/dri` | `drm_name_info` prints `bus->get_name()` = `platformdev->name` + `dev_name`, never `driver->name`; 3.10 creates proc nodes only for the LEGACY minor | `11400000.mali 11400000.mali …` | — | **no change** (prints the device/bus name, not a driver name) |
| 7 | every `dev_err`/`dev_warn`/`dev_info` | `dev_printk` prefix `dev_driver_string()` = platform driver name | `panfrost7580 11400000.mali: …` | `panfrost` | **dmesg prefix changes** to `panfrost 11400000.mali: …` (§4) |
| 8 | uevent `DRIVER=`, `/sys/bus/platform/drivers/<name>`, `<dev>/driver` symlink, `MODULE_ALIAS` | `drivers/base/core.c:308`; driver-core device dir name; modalias autoload | `DRIVER=panfrost7580`, `…/drivers/panfrost7580` | `panfrost` | **fixed**: `DRIVER=panfrost`, `…/drivers/panfrost`, alias `platform:panfrost` (§4) |
| 8b | **KWin 6.8 DRM backend** | reads `/sys/dev/char/226:128/device/uevent`, then a driver-name lookup keyed on `DRIVER=`; unknown string → NULL → SIGSEGV at si_addr=0 (Main's strace) | `panfrost7580` → segfault | `panfrost` | **fixed** by row 8; Mesa/kmsro never saw this because it uses `drmGetVersion()` |
| 9 | `/proc/interrupts` | explicit `devm_request_irq` names | `panfrost7580-job/-mmu/-gpu` | — | **no change** (action labels, not the driver name) |
| 10 | `mesaprobe do_opens` | probes both literal names | `panfrost7580` → 4, `panfrost` → -1 | — | row flips: `panfrost7580` → -1, `panfrost` → 4. **Tool still passes**; the `### render-opens` capture in `kms-mesa-pairing.md` §8 is now historical |
| 11 | `drmshim` / `run-matrix.sh` shim cells | `DRM_NAME_SHIM=panfrost7580=panfrost` | rewrote the name | — | map becomes a **no-op** (source name no longer exists); `run-matrix.sh` filters on `panfrost` and still matches. No cell breaks |
| 12 | `kmsflip` output | `KMSFLIP: … driver=<drmGetVersion name>` | `driver=panfrost7580` | — | prints `driver=panfrost`; no script greps the literal (checked `kms/tools/`, `series/`) |
| 13 | docs asserting the old name | `kms-mesa-pairing.md:22,36,141,253`, `kwin-bringup-plan.md:32,61,116,307`, `kms-flip-tool.md:72-73`, `tools/env.sh:13` | — | — | **stale text**, not a failure. Owners (MesaPair/KwinPrep) should update; this ticket's commit is patch + report only |
| 14 | kernel-side name checks | grep of pristine `drivers/gpu/drm/*.c` and `drivers/base/*.c` | none compare either name; uevent/dev_printk only print it | — | **nothing breaks kernel-side** |
| 15 | captured dmesg in docs | `gpu31-watchdog.md:122`, `gpu31-readback.md:180-182` quote `panfrost7580 11400000.mali:` | — | — | those captures become historical; the *message* text (`pf7580: …`) is unchanged apart from the prefix |

Collision audit for the new platform name:

* pristine 3.10 has **no** platform driver, module alias or `drivers/gpu/drm/panfrost`
  named `panfrost` (`grep -rn 'platform:panfrost\|\.name *= *"panfrost"' drivers include`
  → empty; `ls drivers/gpu/drm/panfrost` → absent; it is created by 0001/0002).
* The KMS side answers to DRM name `exynos` and platform driver
  `exynos7580-decon-drm`; different device, and the platform driver namespace is
  distinct from the DRM name namespace, so no overlap.
* `platform_driver` registration is by name in the platform bus only;
  registering `panfrost` cannot alias the DRM driver of the same string —
  nothing in the tree dereferences one from the other.
* DT binding is unaffected: `platform_match()` tries
  `of_driver_match_device()` first (`drivers/base/platform.c`), so `arm,mali`
  still binds regardless of the driver name.

Net: nothing that worked stops working. The rename removes the name gate for
both Mesa generations, removes the need for the LD_PRELOAD shim on Mesa 20, and
gives KWin the `DRIVER=panfrost` string it maps.

---

## 3. KMS image config: the GPU driver is **absent** — required addition

Evidence (checked 2026-11-25):

```sh
grep -c PANFROST7580 code/gpu31/series/build-int{50..83}.sh      # 0 in every KMS image script
grep -n PANFROST patches/drm-kms-coexist/display-owner-kms.fragment   # (none)
grep -n PANFROST patches/drm-kms/diag/shim-only.fragment              # (none)
grep -n DRM_PANFROST code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64  # (none)
```

`code/gpu31/series/build-int83.sh` (the current KMS image) applies only
`patches/drm-atomic/0001-0005` + 14 `patches/drm-kms/*` patches and configures
`config-samsung-j7elte.aarch64` + `display-owner-kms.fragment` +
`shim-only.fragment`. It applies **none** of `code/gpu31/driver/0001,0002`,
`patches/drm-backport/0001-0005`, `mode-b`, or `dmabuf/0001`, and sets no
`CONFIG_DRM_PANFROST7580`.

Consequence: a KMS image built as int83 has `/dev/dri/card0` but **no
`/dev/dri/renderD128`**; kmsro therefore cannot pair on it even with this
rename. Conversely the GPU images (int10…int49) have the render node but no
`card0`. The render-node rows in `kms-mesa-pairing.md` ran on the GPU kernel
(`#37`); the card0 rows are blocked. A **combined** image is the missing piece.

### Integrator addition (config)

**Status: implemented.** `patches/drm-kms-coexist/gpu-pairing.fragment` exists
and `build-int84..87.sh` apply it, so the combined images already carry the
render node. The analysis below is kept as the rationale.

Add to the KMS image config (either place works; a dedicated fragment keeps
`display-owner-kms.fragment` shared with the non-GPU coexist series clean):

```
CONFIG_DRM_PANFROST7580=y
```

Recommended placement: a new `patches/drm-kms-coexist/gpu-pairing.fragment`
containing that one line, applied by the image script after
`display-owner-kms.fragment`, **and** add `CONFIG_DRM_PANFROST7580=y` to the
script's `CONFIG_EXPECT` closure list so a silent loss fails the gate.

`=y`, not `=m`: the fragment already pins `CONFIG_DRM=y` (dependency
`DRM && ARM64 && MMU` is closed; the symbol selects `DRM_GEM_CMA_HELPER`), and a
built-in render node avoids adding a third `.ko` to the KMS module pair, whose
vermagic pairing is already the fragile part of int82/int83.

### Integrator addition (patches) and the four merge points

**Status: implemented in `build-int87.sh`** (items 1-2 as script hand edits,
item 3 skipped, item 4 wired through `patches/drm-kms/0136`, `0137` and
drm-backport `0006`/`0007`). Kept as the rationale for a from-scratch series.

The combined image needs, on top of the int83 KMS series:
`patches/drm-backport/0001-0004`, `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch`,
`code/gpu31/driver/0001` + `0002`. Applied in that order on top of the KMS
series, everything applies **except** three known overlaps plus one KMS-side
gap (verified, §5):

1. `patches/drm-backport/0005-build-wiring-for-shims.patch` — its
   `drivers/gpu/drm/Makefile` hunk no longer matches because maxwell-atomic
   0003 already extended `drm-y`. Hand-apply its two lines:
   `drm-y += drm_syncobj.o drm_sched.o` (before `drm-$(CONFIG_COMPAT)`) and the
   `drivers/base/Makefile` line `obj-$(CONFIG_DMA_SHARED_BUFFER) += dma-fence.o dma-resv.o`.
2. `mode-b` `include/drm/drmP.h` hunk — drm-atomic 0001 took `0x8000` for
   `DRIVER_ATOMIC`. Add `#define DRIVER_RENDER_ONLY 0x10000` (next free bit)
   after `DRIVER_ATOMIC`, and add the `struct drm_minor *render;` member in the
   `struct drm_device` hunk by hand; the rest of mode-b applies as-is.
3. `code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch` —
   **do not apply**: `patches/drm-kms/0003-core-drm-gem-cma-helper-arm64-dma-attrs.patch`
   already carries the identical `DMA_ATTR_WRITE_COMBINE` conversion (plus the
   arm64 format-specifier fixes). The two overlap at
   `drm_gem_cma_helper.c:35` and cannot both apply.
4. **PRIME ioctls are not wired on card0** (measured by `MesaPair` on the
   int83 boot: `DRM_CAP_PRIME=0x0`). `patches/drm-kms/0200` sets
   `DRIVER_PRIME` in `driver_features` and `.gem_prime_export/get_sg_table/
   vmap/vunmap/import`, but never `.prime_handle_to_fd`/`.prime_fd_to_handle`,
   and 3.10 derives `DRM_CAP_PRIME` from exactly those two pointers
   (`drm_ioctl.c:287-288`: `0x1` = import, `0x2` = export). renderonly's first
   allocation step is `drmPrimeHandleToFD(card0)`, so kmsro fails before it
   renders. Add to the `drm_driver` initializer in 0200
   (`exynos_drm7580_main.c`, next to `.gem_prime_export`):

   ```c
   	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
   	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
   ```

   With those two, `DRM_CAP_PRIME` reads `0x3`, and
   `drm_gem_prime_handle_to_fd()` reaches the generic
   `drm_gem_prime_export()` (3.10 has no dispatcher: `.gem_prime_export` *is*
   the exporter, `drm_prime.c:185-195`). The two existing candidates
   `code/gpu31/dmabuf/0002-drm-exynos7580-gem-prime-export.patch` and
   `patches/iommu-dma/0001-drm-exynos7580-wire-gem-prime-export-for-kbase-umm.patch`
   carry this wiring (plus their own `drm_driver` rewrite) but target the
   pre-0200 filename `drivers/gpu/drm/exynos7580/exynos7580_drm.c`, so neither
   applies to the int83 tree — port the two lines into 0200 instead of
   resurrecting those patches.

`code/gpu31/driver/0003-…-fault-head-vertexjob-decode.patch` is not part of any
build script and does not apply to the current driver 0001 (pre-existing, see
§5); it is out of scope here and must not be added to the combined image until
it is regenerated.

---

## 4. uevent / sysfs / dmesg delta

### 4.1 uevent (the KWin gate)

`/sys/dev/char/226:128/device/uevent` (render minor 128 → platform device
`11400000.mali`), before → after:

```diff
 DRIVER=panfrost7580
+DRIVER=panfrost
 MODALIAS=of:NmaliT<NULL>Carm,mali
 OF_NAME=mali
 OF_FULLNAME=/mali@11400000
 OF_COMPATIBLE_0=arm,mali
 OF_COMPATIBLE_N=1
```

Only the `DRIVER=` value changes. `MODALIAS`, `OF_NAME`, `OF_FULLNAME` and
`OF_COMPATIBLE_*` come from the DT node
(`drivers/of/device.c:132-169`, 3.10 has no `OF_TYPE` here because the node
type is `<NULL>`) and are untouched. Correspondingly:

```diff
-/sys/bus/platform/drivers/panfrost7580        (directory)
+/sys/bus/platform/drivers/panfrost            (directory)
-/sys/devices/platform/11400000.mali/driver -> .../drivers/panfrost7580
+/sys/devices/platform/11400000.mali/driver -> .../drivers/panfrost
```

`/sys/devices/platform/11400000.mali` (device name) is unchanged.

### 4.2 dmesg

Two classes of line change:

```diff
-[drm] Initialized panfrost7580 1.0.0 20261119 on minor 128 (render-only)
+[drm] Initialized panfrost 1.0.0 20261119 on minor 128 (render-only)

-panfrost7580 11400000.mali: pf7580: power cycle with busy cores
+panfrost 11400000.mali: pf7580: power cycle with busy cores
```

(`%d` is `dev->render->index` = 128, the render-minor idr base; `date` is
`drm_driver.date = "20261119"`. `DRM_INFO` adds the `[drm] ` prefix. The
device-message prefix is `dev_driver_string()` + `dev_name()`.)

Unchanged in the boot log:

* message tags inside the format strings, e.g.
  `panfrost 11400000.mali: panfrost7580: MMU fault …` /
  `… panfrost7580: GPU MULTIPLE_FAULT …` — the `panfrost7580:` tag is text, and
  is kept so `docs/reports/gpu31-jobtrace.md` greps stay valid.
* `/proc/interrupts` labels `panfrost7580-job/-mmu/-gpu`.
* `MODULE_DESCRIPTION` (module metadata, not a log line).
* No `drm` release/teardown line contains a driver name.

So `dmesg | grep panfrost` still finds all prior evidence lines; lines that
grep `panfrost7580 11400000.mali:` (two captured samples in
`gpu31-watchdog.md` / `gpu31-readback.md`) become historical.

### 4.3 Runtime gate for int88

```sh
grep -qx 'DRIVER=panfrost' /sys/dev/char/226:128/device/uevent   # uevent
[ -d /sys/bus/platform/drivers/panfrost ]                        # driver dir
[ ! -e /sys/bus/platform/drivers/panfrost7580 ]                  # no stale dir
grep -q 'Initialized panfrost 1\.0\.0 20261119 on minor 128 (render-only)' /dev/kmsg
```

---

## 5. Verification performed (patch application only)

Scratch trees are copies of the pristine 3.10.108 tree
(`.../j7k/android_kernel_samsung_universal7580-2a746c1b…`); nothing was built,
flashed or committed there.

**Current check — the exact int88 predecessor series.** `build-int87.sh`'s
ordered `addp` list was extracted verbatim (62 patches: drm-atomic 0001-0005,
the KMS core/slice series up to `0137-kms-core-prime-allow-rdwr`, drm-backport
0001-0004, Mode-B, driver 0001/0002/0137, drm-backport 0006/0007, 0136) and
applied with int87's own exceptions and hand edits
(`0200 --exclude=decon_core.c`, Mode-B `--exclude=include/drm/drmP.h`, then the
`DRIVER_RENDER_ONLY 0x10000` + `struct drm_minor *render;` + Makefile lines).
Result: **all 62 apply, no conflict**, including the edited 0001, 0002 and the
perf/mmap patch 0137. Post-apply:

```
drivers/gpu/drm/panfrost/panfrost7580_main.c:303:	.name = "panfrost",   # drm_driver
drivers/gpu/drm/panfrost/panfrost7580_main.c:748:		.name = "panfrost",   # platform_driver
drivers/gpu/drm/panfrost/panfrost7580_main.c:760:MODULE_ALIAS("platform:panfrost");
drivers/gpu/drm/panfrost/panfrost7580_main.c:439:	{ .compatible = "arm,mali-t720" },
drivers/gpu/drm/panfrost/panfrost7580_main.c:446:MODULE_DEVICE_TABLE(of, p7580_of_match);
```

* earlier GPU-only order (int49): drm-backport 0001-0005, Mode-B, dmabuf, then
  0001+0002 — all `ok` as well.
* `code/gpu31/perf/0001-panfrost7580-prof-overlay.patch` still
  `git apply --check`-clean on the edited 0001 (int49's pinned overlay is
  unaffected).
* driver `0137-panfrost7580-mmap-instrument.patch` touches only
  `panfrost7580_gem.c` / `_ioctl.c`, so the main.c edit cannot disturb it
  (confirmed by the clean 62-patch run above).
* The int4x/int8x new-file hunk-count gate (awk over every applied patch)
  reports no mismatch for edited 0001.
* `code/gpu31/driver/0003-…-fault-head-vertexjob-decode.patch` fails
  `git apply --check` against the **unmodified** 0001 as well (job.c:856,
  job.h:113, submit.c:400) — pre-existing stale baseline, not caused by this
  change, and not applied by any build script.

Not verified here (assigned to int88 at build/boot time): compilation and the
runtime gates in §4.3.

---

## 6. Rollback

Three lines, no ABI changes involved:

```sh
git revert --no-edit <commit>      # or restore driver 0001 and rebuild
# back to:  drm_driver.name = "panfrost7580"
#           platform_driver.driver.name = "panfrost7580"
#           MODULE_ALIAS("platform:panfrost7580")
```

Consequences of rollback: Mesa 20 kmsro pairing needs the LD_PRELOAD shim again
(`code/gpu31/kms/tools/mesa/drmshim.c`, `DRM_NAME_SHIM=panfrost7580=panfrost`);
the Mesa 26 name gate closes again; KWin 6.8 reads `DRIVER=panfrost7580` in
uevent again and segfaults on the unknown name. No kernel state, module name,
DT binding, debugfs path, IRQ label or stored image is affected either way — the
rename is userspace-visible only through `DRM_IOCTL_VERSION`, the device uevent
`DRIVER=` / platform driver directory, and the dmesg prefix.
