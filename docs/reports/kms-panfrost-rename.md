# Render driver rename: `panfrost7580` → `panfrost`

Status 2026-11-25. Owner slice: `PanfrostName`. Follows
`docs/reports/kms-mesa-pairing.md` §5 item 1 (the name is the only thing that
blocks the kmsro join; every other gate is a version/ABI gate).

Search-key evidence for the whole ticket (all paths relative to the repo root):

```sh
grep -n '\.name = "panfrost' code/gpu31/driver/0001-panfrost7580-driver-sources.patch
# 5856:  .name = "panfrost7580"   <- drm_driver.name   (changed, this ticket)
# 6301:  .name = "panfrost7580"   <- platform_driver.driver.name (kept)
```

---

## 1. Change

One line in `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`, inside
the `panfrost7580_main.c` new-file hunk (patch line 5856):

```diff
 	.fops = &p7580_drv_fops,
-	.name = "panfrost7580",
+	.name = "panfrost",
 	.desc = "Mali-T720 render-only (Exynos 7580)",
 	.date = "20261119",
```

* driver 0001 sha256 before: `e74746706e2320a7e4dfff8d978473bec5bc3cadbfa7e48068abd586bfc4be48`
* driver 0001 sha256 after:  `aa710ebb23d0d57b289259504f65a30f346b134ec6c87b38d428b61f6f0dab15`

Keeping the change inside driver 0001 (instead of a follow-up patch under
`patches/drm-kms/`) is deliberate: the KMS series does not apply the GPU
driver, so a `patches/drm-kms/` follow-up would never reach the render node;
driver 0001 is the single artifact every GPU image and any future combined
image applies.

The `+1,760` hunk count is unchanged (a `+` line changed its text, not the
count), so the int4x "new-file hunk count" gate still passes — verified with
that gate's own awk (§5).

### Deliberately unchanged (string is *not* the DRM driver name)

| Symbol / string | Location | Why kept |
|---|---|---|
| module/object name `panfrost7580.ko`, `panfrost7580-y` | 0002 build wiring | module name; = m/`insmod` naming and the int4x gates |
| `CONFIG_DRM_PANFROST7580` | 0002 Kconfig | config symbol; every build script and fragment uses it |
| `platform_driver.driver.name = "panfrost7580"` | 0001 main.c:6301 | platform bus name; `dev_driver_string()` log prefix, `/sys/bus/platform/drivers/panfrost7580`, DT/platform match, `MODULE_ALIAS("platform:panfrost7580")` |
| `.compatible = "arm,mali-t720"` / `"arm,mali"` | 0001 main.c:5991 | DT matching |
| debugfs `p7580/` (jobs/perf slices), `dri/128/` | job slice / DRM core | debugfs paths |
| IRQ action labels `panfrost7580-job/-mmu/-gpu` | 0001 main.c:6169-6177 | `/proc/interrupts` labels |
| `get_driver_name()` `"panfrost7580"`, timelines `"panfrost7580-js-N"` | 0001 job.c / `code/gpu31/jobs/panfrost7580_job.c:226` | dma_fence debug labels; no kernel or Mesa consumer |
| `drm_sched_init(..., "panfrost7580")` | 0001 main.c:8738 | workqueue/thread label only |
| message tags `"panfrost7580: MMU fault …"` | 0001 mmu/IRQ paths | log text matched by `docs/reports/gpu31-jobtrace.md`; not the driver name |

---

## 2. Audit — what keys on `drm_driver.name`

The value is consumed only through `DRM_IOCTL_VERSION` in this tree
(`drivers/gpu/drm/drm_drv.c:349-350` copies `dev->driver->name`). No in-tree
kernel code compares it; the only other readers are the platform-bus init print
(`drm_platform.c:95`, patched by Mode-B) and `drm_irq.c:355` (unused: the driver
installs its own IRQs with explicit labels).

| # | Consumer | Mechanism | Before | Needs | After rename |
|---|---|---|---|---|---|
| 1 | Mesa 20.3.5 kmsro | `drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER)` → exact `drmGetVersion()->name` match on every render node | `panfrost7580` | `panfrost` | **fixed** (gate 2 of the pairing report) |
| 2 | Mesa 26.3.0 platform probe (kmsro) | render nodes whose `drmGetVersion()->name` ∈ {`panfrost`, `panthor`, `lima`, … } | `panfrost7580` | `panfrost` | **fixed** (gate 2′) |
| 3 | Mesa 26 panfrost screen | `pan_kmod_drm_drv_match(fd, "panfrost")` **and** version ≥ 1.1 | name wrong, 1.0 | name + 1.1 | name gate **fixed**; version 1.0 and the 6.x `PANFROST_CREATE_BO` ABI still block Mesa 26 (unchanged, separate tickets) |
| 4 | Mesa/libdrm DRI loader | render node → `<name>_dri.so` | `panfrost7580_dri.so` (absent) → llvmpipe | `panfrost_dri.so` | **fixed**; `MESA_LOADER_DRIVER_OVERRIDE=panfrost` in `code/gpu31/kms/tools/env.sh` becomes redundant (harmless) |
| 5 | DRM core init print | `DRM_INFO("Initialized %s %d.%d.%d %s on minor %d (render-only)")` (Mode-B `drm_platform.c` hunk) | see §4 | — | **dmesg text change** (§4) |
| 6 | `debugfs dri/128/name`, `/proc/dri` | `drm_name_info` prints `bus->get_name()` = `platformdev->name` (`11400000.mali`) + `dev_name`, never `driver->name`; 3.10 creates proc nodes only for the LEGACY minor | unchanged | — | **no change** (debugfs path and content both stable) |
| 7 | every `dev_err`/`dev_warn`/`dev_info` | `dev_printk` prefix `dev_driver_string()` = platform driver name | `panfrost7580 11400000.mali: …` | — | **no change** (platform driver name kept) |
| 8 | sysfs/platform/module autoload | `/sys/bus/platform/drivers/panfrost7580`, uevent `DRIVER=`, `MODULE_ALIAS("platform:…")` | — | — | **no change** |
| 9 | `/proc/interrupts` | explicit `devm_request_irq` names | `panfrost7580-job/-mmu/-gpu` | — | **no change** |
| 10 | `mesaprobe do_opens` | probes both literal names | `panfrost7580` → 4, `panfrost` → -1 | — | row flips: `panfrost7580` → -1, `panfrost` → 4. **Tool still passes**; the `### render-opens` capture in `kms-mesa-pairing.md` §8 is now historical |
| 11 | `drmshim` / `run-matrix.sh` shim cells | `DRM_NAME_SHIM=panfrost7580=panfrost` | rewrote the name | — | map becomes a **no-op** (source name no longer exists); `run-matrix.sh` filters on `panfrost` and still matches. No cell breaks |
| 12 | `kmsflip` output | `KMSFLIP: … driver=<drmGetVersion name>` | `driver=panfrost7580` | — | prints `driver=panfrost`; no script greps the literal (checked `kms/tools/`, `series/`) |
| 13 | docs asserting the old name | `kms-mesa-pairing.md:22,36,141,253`, `kwin-bringup-plan.md:32,61,116,307`, `kms-flip-tool.md:72-73`, `tools/env.sh:13` | — | — | **stale text**, not a failure. Owners (MesaPair/KwinPrep) should update; this ticket's commit is patch + report only |
| 14 | kernel-side name checks | grep of pristine `drivers/gpu/drm/*.c` | none compare the name | — | **nothing breaks kernel-side** |

Net: nothing that worked stops working. The rename removes the name gate for
both Mesa generations and removes the need for the LD_PRELOAD shim on Mesa 20.

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

### Integrator addition (patches) and the three merge points

The combined image needs, on top of the int83 KMS series:
`patches/drm-backport/0001-0004`, `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch`,
`code/gpu31/driver/0001` + `0002`. Applied in that order on top of the KMS
series, everything applies **except** three known overlaps (verified, §5):

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

`code/gpu31/driver/0003-…-fault-head-vertexjob-decode.patch` is not part of any
build script and does not apply to the current driver 0001 (pre-existing, see
§5); it is out of scope here and must not be added to the combined image until
it is regenerated.

---

## 4. dmesg delta

One line changes at probe (Mode-B render-only print, `drm_platform.c`):

```diff
-[drm] Initialized panfrost7580 1.0.0 20261119 on minor 128 (render-only)
+[drm] Initialized panfrost 1.0.0 20261119 on minor 128 (render-only)
```

(`%d` is `dev->render->index` = 128, the render-minor idr base; `date` is
`drm_driver.date = "20261119"`. `DRM_INFO` adds the `[drm] ` prefix.)

Everything else in the boot log is byte-identical, in particular:

* `panfrost7580 11400000.mali: pf7580: …` (clocks/power/reset probe lines) —
  prefix comes from `dev_driver_string()`, i.e. the platform driver name.
* `panfrost7580 11400000.mali: panfrost7580: MMU fault …` /
  `… GPU MULTIPLE_FAULT …` — message tags, not the driver name.
* `/proc/interrupts` labels `panfrost7580-job/-mmu/-gpu`.
* No `drm` release/teardown line contains the name.

So `dmesg | grep panfrost` still finds all prior evidence lines; only the
`Initialized …` line changes wording.

---

## 5. Verification performed (patch application only)

Scratch trees are copies of the pristine 3.10.108 tree
(`.../j7k/android_kernel_samsung_universal7580-2a746c1b…`); nothing was built,
flashed or committed there.

GPU series order (int49 order), with the edited 0001:

```sh
cd <pristine-copy>
for p in patches/drm-backport/000[1-5]-*.patch; do git apply --check "$p" && git apply "$p"; done
git apply --check code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch && git apply …
git apply --check code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch && git apply …
git apply --check code/gpu31/driver/0001-panfrost7580-driver-sources.patch && git apply …
git apply --check code/gpu31/driver/0002-panfrost7580-build-wiring.patch && git apply …
```

Result: all `ok`, including edited 0001 and 0002. Post-apply:

```
drivers/gpu/drm/panfrost/panfrost7580_main.c:303:	.name = "panfrost",
drivers/gpu/drm/panfrost/panfrost7580_main.c:748:		.name = "panfrost7580",   # platform driver, kept
```

* `code/gpu31/perf/0001-panfrost7580-prof-overlay.patch` still
  `git apply --check`-clean on the edited 0001 (int49's pinned overlay is
  unaffected).
* The int4x new-file hunk-count gate (awk over every applied patch) reports no
  mismatch for edited 0001.
* KMS-first order (int83 series, then GPU patches) — 0001 and 0002 also apply
  cleanly; the three overlaps listed in §3 are the only conflicts.
* `code/gpu31/driver/0003-…-fault-head-vertexjob-decode.patch` fails
  `git apply --check` against the **unmodified** 0001 as well (job.c:856,
  job.h:113, submit.c:400) — pre-existing stale baseline, not caused by this
  change, and not applied by any build script.

---

## 6. Rollback

Reverting is the single line, no ABI changes involved:

```sh
# one line back to the old literal (patch line 5856), then rebuild the image
git revert --no-edit <this commit>      # or restore the file by hand / checkout
# resulting line:  +	.name = "panfrost7580",
```

Consequences of rollback: Mesa 20 kmsro pairing needs the LD_PRELOAD shim again
(`code/gpu31/kms/tools/mesa/drmshim.c`, `DRM_NAME_SHIM=panfrost7580=panfrost`);
Mesa 26 name gate closes again. No kernel state, module name, DT binding,
debugfs path, IRQ label or stored image is affected either way — the rename is
userspace-visible only through `DRM_IOCTL_VERSION` and the one-init-line dmesg
change.
