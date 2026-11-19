# GPU31 suspend — runtime PM + system sleep on vendor kbase (fbdev intact)

Date: 2026-11-19. Slice: `suspend` (G31_Suspend).
Device state per Main 2026-11-19: **phone IS UP**, kernel `3.10.108 #35
SMP PREEMPT Wed Nov 18 23:36:05 EST 2026` (aarch64, gcc 16.2.0).
Stale "link down" notes in older slice reports are disregarded.
No reboot, no flash, no kernel build performed in this slice
(Integrator builds; M1Verify flashes after Main's explicit go).
The shared kernel tree was never modified — read-only source study plus
a `git apply --dry-run` + `-fsyntax-only` check of the new patch file
from a private copy. Stock `dt.img` untouched. Live reads below are
short `flock /tmp/gpsdev.lock` SSH holds (<120 s), read-only sysfs.

## Deliverables

- `code/gpu31/suspend/0001-gpu31-kbase-dev_pm_ops-suspend-resume.patch`
  — one-file kbase patch (applies to the shared tree's
  `drivers/gpu/arm/t7xx/r15p0/mali_kbase_core_linux.c`).
- This report: `docs/reports/gpu31-suspend.md`.

## 1. Ground truth (tree file:line + live reads)

### 1.1 The defect: suspend/resume dummies in dev_pm_ops

`mali_kbase_core_linux.c:4055-4220` (3.10.108 r15p0 tree):

| Symbol | Lines | What it does |
|---|---|---|
| `kbase_device_suspend` (real body) | 4068-4083 | devfreq suspend + `kbase_pm_suspend(kbdev)`; returns 0 |
| `kbase_device_resume` (real body) | 4099-4111 | `kbase_pm_resume(kbdev)` + devfreq resume; returns 0 |
| `kbase_device_suspend_dummy` | 4062-4066 | `return 0` — wired to `.suspend` |
| `kbase_device_resume_dummy` | 4093-4097 | `return 0` — wired to `.resume` |
| `kbase_device_runtime_suspend/resume/idle` | 4123-4204 | real runtime legs (`-EBUSY` guard, `callback_power_runtime_on/off`) |
| `kbase_pm_ops` | 4211-4220 | `.suspend/.resume` = dummies; runtime legs real |

System sleep therefore never reaches the real bodies through the driver
model. All it reaches is the exynos PM notifier
(`platform/exynos/gpu_notifier.c:137-156`):

```c
case PM_SUSPEND_PREPARE:  kbase_device_suspend(kbdev);  break;
case PM_POST_SUSPEND:     kbase_device_resume(kbdev);   break;
```

Two consequences:

1. **genpd leg is a no-op for mali.** `drivers/base/power/main.c`
   `__device_suspend` (`:1123-1220`) prefers `dev->pm_domain->ops`
   (`pm_op`) when present, else falls to `dev->driver->pm`. On this
   tree the mali device is attached to pd-g3d by the vendor bus notifier
   (`arch/arm64/mach-exynos/pm_domains.c:401-430`,
   `exynos_read_domain_from_dt` on `BUS_NOTIFY_BIND_DRIVER`, verified in
   gpu31-power §1). With dummies in `kbase_pm_ops`, the domain suspend
   bookkeeping (`pm_genpd_suspend` → `genpd_suspend_dev` →
   `GENPD_DEV_CALLBACK(suspend)`, `drivers/base/power/domain.c:765-768`)
   calls a stub: pd-g3d can gate the rail while
   `kbdev->pm.backend.gpu_powered` still reads true. DPM ordering vs
   pd-disp (DECON fbdev) is likewise lost for the GPU device.
2. **Notifier errors cannot veto sleep.** `gpu_pm_notifier` returns
   `NOTIFY_OK` unconditionally; `blocking_notifier_call_chain` folds
   results and the pm core never aborts on it. A GPU drain failure
   (jobs still active) cannot stop the transition the way a dev_pm_ops
   error return from `dpm_suspend` can (`main.c:1260-1266` sets
   `async_error` and aborts).

### 1.2 What the real bodies already do (why wiring them is safe)

- `kbase_pm_suspend` (`mali_kbase_pm.c:232-270`): sets `pm.suspending`,
  `kbasep_js_suspend` (job scheduler releases PM refs),
  waits `pm.active_count == 0`, then `kbase_hwaccess_pm_suspend`
  (backend `mali_kbase_pm_backend.c:412-450` →
  `kbase_pm_do_poweroff(is_suspend=true)` → `kbase_pm_clock_off` →
  `callback_power_suspend` = `gpu_power_suspend` →
  `pm_schedule_suspend(delay)` + `gpu_control_disable_clock`).
- `kbase_hwaccess_pm_resume` (`:452-469`): clears suspending,
  `kbase_pm_do_poweron(is_resume=true)` → `kbase_pm_clock_on`
  (`mali_kbase_pm_driver.c:1032-1120`) → power_on/resume callback chain,
  **GPU reset when state was lost** (`power_on` return 1),
  MMU reprogram per bound address space, IRQ re-enable, JS resume
  (`kbase_pm_resume`, `mali_kbase_pm.c:272-302`). Context loss degrades
  to reset + re-fault, never stale mappings.
- Concurrent userspace syscalls are race-free: JD submit paths use
  `kbase_pm_context_active_handle_suspend` with explicit suspend
  handlers (`mali_kbase_pm.c:103-173`); a suspend in progress fails or
  defers new activations per handler code instead of powering off
  under a live job.
- devfreq legs are NULL-safe: `drivers/devfreq/devfreq.c:551-561`
  returns `-EINVAL` on NULL devfreq and 0 on NULL governor.
  `CONFIG_MALI_DEVFREQ` is unset here (pmaports config has only
  `CONFIG_MALI_DVFS=y`), so `kbdev->devfreq` is NULL — legitimately
  skipped, verified by the fact that the notifier path already calls
  the same functions unconditionally today.
- Runtime-PM backend callbacks in `platform/exynos/gpu_notifier.c`:
  `pm_callback_runtime_on` (clock enable + DVFS env + set target volt,
  `:276-299`), `pm_callback_runtime_off` (clock disable + env stop,
  `:301-327`), `gpu_power_on/off/suspend` (`:167-225`, runtime
  get/put + autosuspend `runtime_pm_delay_time` = 50 ms default,
  `mali_kbase_platform.c:347`). All untouched by this patch.

### 1.3 Runtime-PM leg is already healthy on live #35 (read-only)

Locked SSH reads, 2026-11-19 (no writes, no reboot):

| Node | Value | Meaning |
|---|---|---|
| `/sys/devices/11400000.mali/power/runtime_status` | `suspended` | genpd + runtime PM already gate the block at idle |
| `.../power/control` | `auto` | runtime PM enabled, kbase-owned sequencing |
| `/sys/kernel/gpu/gpu_clock` | `0` | aclk_g3d gated |
| `/sys/kernel/gpu/gpu_busy` | `0%` | no GPU work (llvmpipe session) |
| `/sys/kernel/gpu/gpu_freq_table` | `266 350 440 534 668` | DVFS ladder intact |
| `/dev/dri` | absent | expected: #35 predates M1 |
| `/sys/class/graphics/fb0/virtual_size` | `720,1280` | vendor fbdev intact |
| `DISPLAY=:0 xdpyinfo dimensions` | `720x1280` | X on fbdev intact |
| `Mali-T72x` (`/sys/kernel/gpu/gpu_model`), `/dev/mali0` per gpu31-master | present | kbase intact |

So the "device power off, clocks gated" half of the acceptance already
holds at runtime; this slice wires the missing system-sleep half to the
same tested poweroff path.

## 2. The patch (one file, no Kconfig/DT change)

`code/gpu31/suspend/0001-gpu31-kbase-dev_pm_ops-suspend-resume.patch`:

1. Delete both dummies (`kbase_device_suspend_dummy`,
   `kbase_device_resume_dummy`).
2. Add two thin wrappers after the real bodies:
   `kbase_device_suspend_pm(dev)` / `kbase_device_resume_pm(dev)`
   (`to_kbase_device(dev)` → `-ENODEV` on NULL → delegate to the real
   `kbase_device_suspend/resume`). Wrappers exist so the exported
   `kbase_device_suspend(kbdev)` / `kbase_device_resume(kbdev)` symbols
   keep their signatures for the exynos notifier and any out-of-tree
   callers.
3. `kbase_pm_ops` becomes:
   `.suspend/.freeze/.poweroff = suspend_pm`,
   `.resume/.thaw/.restore = resume_pm`, runtime legs unchanged.

What is deliberately NOT in the patch:

- No Kconfig change (`CONFIG_PM_SLEEP=y`, `CONFIG_PM_RUNTIME=y`,
  `CONFIG_PM_SLEEP_DEBUG=y`, `CONFIG_MALI_RT_PM` default y already).
- No DT change; no clock/regulator/genpd logic change.
- The exynos `gpu_pm_notifier` is kept as-is (MALI_SEC hotplug/TMU
  sequencing relies on `PM_SUSPEND_PREPARE/PM_POST_SUSPEND` broadcast
  timing). After this patch it is a redundant second call; harmless
  because both bodies are idempotent across `pm.suspending` and the
  `active_count == 0` drain (second suspend finds count already 0,
  second resume finds suspending already false, both return 0).
- No `poweroff_pending`/`suspending_wait` surgery; no fbdev/DECON change
  (DECON keeps its runtime-only `decon_pm_ops` clock gating
  `decon_core.c:2542-2592` + `.fb_blank` handler; pd_disp ordering
  unchanged).
- No second PM notifier, no `pm_suspend_ignore_children`
  (gpu31-power.md F8: genpd owns the domain, dev_pm_ops owns the
  device).
- No userspace ABI change: no lost-context counter/ioctl. Mesa
  re-faults on next submit after the resume reset. A generation
  counter is a follow-up slice, not this one.

## 3. Verification (host-side; integrator/M1Verify own the rest)

- `git apply --check`-equivalent: `patch -p1 --dry-run` against a
  pristine copy of `mali_kbase_core_linux.c` from the read-only shared
  tree → **PASS** (`checking file .../mali_kbase_core_linux.c`, no
  rejects). Full file text verified post-apply by inspection of the
  patched copy.
- `-fsyntax-only` with the tree's real build flags (extracted from the
  tree's own `.mali_kbase_core_linux.o.cmd`: `-nostdinc`, arm64 + kbase
  + exynos + backend/gpu + staging/android + thermal include set,
  `KBUILD_STR/BASENAME/MODNAME`, `MALI_CUSTOMER_RELEASE`,
  `MALI_RELEASE_NAME`, `MALI_SEC_CL_BOOST/UTILIZATION`, kconfig
  include): **patched file exits 0 with zero warnings/errors**, and the
  **unpatched baseline also exits 0** — the patch introduces no new
  diagnostics. (Not a kernel build: only G31_Integrator builds.)
- Live evidence is boot-time/runtime only (see §1.3): no system-sleep
  cycle was attempted from this slice (no reboot — cold boot is scarce,
  GPS/FM share the phone; no flash without Main's go).
- `git apply --check` of the final committed patch from the repo is
  left to the integrator at wire-in time (one-file, fuzz-free context
  on uniquely-anchored dummy/pm_ops blocks).

## 4. Review-scope statement (G31_Review 625ed18)

The 2026-11-19 review (`docs/reports/gpu31-review.md`) findings target
other slices' files and require **no change in this slice**:

- Sched (0004 entity-list/dependency/finish_cb/ERR_PTR/timeout/
  push_job), dma-resv locking/pinning, dma-fence init/lock/wait —
  `patches/drm-backport/*`, owned by G31_Sched. This slice adds no
  fence/resv/sched code.
- DrmCore display driver (iovmm/clk_put/IRQ/LVDS-vs-DSI), DT
  single-compatible, config fragment, drm-m1 alternative-path note —
  `patches/drm/*`, owned by G31_DrmCore. This slice touches no display
  driver, DT, or Kconfig.
- DmaBuf import-by-copy/vmap pinning — `patches/iommu-dma/*`, owned by
  G31_DmaBuf. This slice touches no PRIME path.
- Mesa `n_slots` short-by-one — `patches/gpu-mesa-kbase/*`, owned by
  G31_Mesa. This slice touches no Mesa code.
- KbaseShim r7p0 48/56 stride — `patches/gpu-kbase-compat/*`. This slice
  does not add a compat shim and preserves the 64-byte stride contract
  by not touching submission at all.

No review item names a file under `code/gpu31/suspend/`; the §18
`drm-kms-coexist/DESIGN.md` PASS (spec, no code) is consistent with this
patch's fbdev-intact claim.

## 5. Integrator / M1Verify handoff

- Apply: `git apply
  code/gpu31/suspend/0001-gpu31-kbase-dev_pm_ops-suspend-resume.patch`
  in the shared tree (fuzz-free on #35-era `mali_kbase_core_linux.c`;
  re-verify with `--check` first — sibling slices may shift line
  numbers, the anchor strings are unique).
- Build: normal `code/kernel/build.sh` flow (no new Kconfig symbol).
- Boot asserts (M1Verify, after Main's go, no extra reboot beyond the
  flash gate): `/dev/mali0` present; `runtime_status=suspended` at
  idle; `echo mem > /sys/power/state` sleep/wake cycle with a kbase
  job in flight (winsys soft job) then `dmesg | grep -iE 'mali|kbase|
  suspend|resume'` shows drain + poweroff + resume reset with no hang;
  `/dev/fb0` + `xdpyinfo 720x1280` unchanged after resume (DECON
  untouched).
- Open / not claimed: measured sleep-cycle trace on hardware; lost-
  context generation counter for Mesa (follow-up slice).
