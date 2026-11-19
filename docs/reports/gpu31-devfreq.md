# gpu31-devfreq — panfrost DVFS on Exynos 7580 (OPP 160–800 MHz, 350 seed)

Date: 2026-11-19. Tree (read-only, never modified):
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
(3.10.108 vendor). Phone state: UP on kernel #37 (SSH works); **no device
I/O in this slice** — host-side code + `kcheck` include-pass only. Only
`G31_Integrator` builds, only Main-gated flash.

J7elte stock config (`code/pmaports/device/archived/linux-samsung-j7elte/
config-samsung-j7elte.aarch64:445,3265,3270`):
`CONFIG_PM_OPP=y`, `CONFIG_PM_DEVFREQ=y`,
`CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND=y`. `CONFIG_DEVFREQ_THERMAL` is **not
set** (thermal report §1) — there is no `of_devfreq_cooling` on this tree.

## 1. Deliverables

| File | Role |
|---|---|
| `code/gpu31/devfreq/pf7580_devfreq.h` | OPP ladder, boot seed, governor/poll constants, host-testable helpers, DVFS API |
| `code/gpu31/devfreq/pf7580_devfreq.c` | init/target/status/cur/mark_busy/suspend/resume/set_max |
| `code/gpu31/devfreq/devfreq_test.c` | host unit test (pure-C surface, no kernel headers) |
| `code/gpu31/devfreq/Makefile` | `check` (host test) + `kcheck` (read-only `-fsyntax-only` vs the real 3.10 headers, never a build) |
| this report | `docs/reports/gpu31-devfreq.md` |

`devfreq_test` binary is a build artefact (untracked, rebuilt by `make
check`); sources + Makefile + report are the committed slice.

Relation to `code/gpu31/gem/panfrost7580_devfreq.[ch]` (GEM-slice draft):
that file is a GEM-internal hook sketch owned by G31_Gem. This header +
companion `.c` are the standalone DVFS slice. Same table, same order rule;
dedup at land time keeps one canonical copy (Main/Integrator call). This
slice never edits the GEM file.

## 2. Ground truth reused (read-only)

| Item | Source |
|---|---|
| OPP rows (freq MHz, volt uV) | `drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c:46-55` `gpu_dvfs_table_default[]`: `{800,734,668,534,440,350,266,160}` MHz at `1100/1100/1100/1000/1000/1000/900/900` mV |
| Voltage/clock order (up V→f, down f→V) | `platform/exynos/gpu_dvfs_api.c:27-38` `GPU_SET_CLK_VOL` (clone of that macro's shape, via the power-slice setters) |
| kbase target()/status()/init() shape | `t7xx/r15p0/backend/gpu/mali_kbase_devfreq.c:49-111` (flags forwarded to `devfreq_recommended_opp`, V-first-up / f-first-down, no-change skip, utilisation reset) and `:139-230` (`initial_freq`/`polling_ms=100`/`target`/`get_dev_status`/`get_cur_freq`/`exit`, `simple_ondemand` + NULL data, OPP notifier) |
| OPP API (Hz in, uV in) | `include/linux/opp.h:30-47`, `drivers/base/power/opp.c:396` `opp_add` / `:257-440` find/RCU |
| devfreq core (clamp→flags→target, max_freq sysfs, add/suspend/resume) | `include/linux/devfreq.h:179-192`, `drivers/devfreq/devfreq.c:155-209` `update_devfreq` (min/max clamp with GLB/LUB flags), `:853-877` `store_max_freq`, `:460-530` add, `:548-579` suspend/resume |
| Governor (stock 90/5 knobs, NULL-safe, zero-division guard) | `drivers/devfreq/governor_simpleondemand.c:37-48` (`DFSO_UPTHRESHOLD 90` / `DOWNDIFFERENCTIAL 5` / `WEIGHT 100`, NULL data legal), `:100-115` (`total_time==0` → MAX, `stat=0` default-keep) |
| Clock/voltage actuation + parking + `-EAGAIN` while gated | `code/gpu31/power/panfrost7580_power.h:187-195` (`pf7580_set_rate` / `pf7580_set_voltage`), report `gpu31-power.md` F2/F4 |
| Thermal max-lock path this plugs under | `code/gpu31/thermal/patches/0001-*` (cdev state → TMU max-lock) + `gpu31-thermal.md` §1 (live 5-level ASV `{668,534,440,350,266}`, `max_state 5`, no devfreq cooling device) |

## 3. Design (what the slice does)

- **Ladder**: 8 rows ascending in Hz, converted verbatim MHz→Hz / mV→uV.
  Ladder symbols carry a `dvfs_` infix (`struct pf7580_dvfs_opp`,
  `pf7580_dvfs_opps[]`, `PF7580_DVFS_OPP_COUNT`,
  `PF7580_DVFS_BOOT_HZ`): the power header already owns a TU-local
  `struct pf7580_opp { int mhz; int uv }` ladder and the firmware header
  owns its own `struct pf7580_opp` + extern table — reusing the bare tag
  would break any TU including both (probe needs both).
- **Boot seed 350 MHz** (`PF7580_DVFS_BOOT_HZ`): mid-ladder, kbase DVFS
  row; GEM draft uses 266 (its standalone stub minimum — no conflict, the
  slices merge at land time).
- **target()**: forwards the caller's `flags` to
  `devfreq_recommended_opp()` exactly like `kbase_devfreq_target`
  (plain polls arrive 0 → ceil/LUB; core sets
  `DEVFREQ_FLAG_LEAST_UPPER_BOUND` when `max_freq` clamps from above —
  forcing 0 instead would resolve a non-OPP ceiling UP past it).
  No-change skip, then `GPU_SET_CLK_VOL` order through the power slice.
  `-EAGAIN` (gated/suspended) propagates untouched; the monitor retries
  next poll.
- **Load accounting**: `mark_busy(busy_ns, idle_ns)` accumulates
  `u64` ns (wrap-safe for >7 min windows; `total_time==0` in the
  governor means MAX, never a divide-by-zero); `get_status` drains.
- **Wiring**: probe `power_probe → dvfs_init(dev, pwr, &dvfs)`; remove
  `dvfs_fini` then `power_remove`; `dev_pm_ops` suspend
  `dvfs_suspend` **before** the runtime-suspend leg, resume
  `dvfs_resume` **after** the runtime-resume leg
  (`DEVFREQ_GOV_SUSPEND/RESUME` → `devfreq_monitor_suspend/resume`,
  `governor_simpleondemand.c:200-205`, `devfreq.c:295-317`).
- **Idle/suspend**: no hook for runtime-idle (50 ms autosuspend parks at
  the boot OPP already power-gated; polling holds no PM ref, so a
  suspended target() just `-EAGAIN`s). System-sleep wrappers are
  NULL-safe (a missing handle returns 0, like kbase's unset path).
- **Thermal (`set_max`)**: no cooling device on this tree, so the
  panfrost equivalent of the kbase TMU max-lock is `df->max_freq` +
  `update_devfreq()` — the exact `store_max_freq` sequence (hold
  `devfreq->lock`, write clamp, re-evaluate; the core applies LUB when
  the clamp bites). `max_hz==0` releases. Non-OPP ceilings resolve by
  ceil. Sitting above the ceiling drops now through the same f-before-V
  down-step; `-EAGAIN` leaves the clamp armed for the next poll.
- **Lookup/concurrency**: never touches dev drvdata (the owning DRM
  driver's own struct lives there) — the single G3D block resolves
  through a file-static handle like kbase's global `pkbdev`
  (`gpu_notifier.c`); the slice mutex makes lookup + two-step actuation
  atomic against concurrent sysfs stores.

## 4. Verification

- `make check` (host unit test): **PASS** — 8-row ladder
  160–800 MHz ascending, verbatim voltages, 350 boot member, ceil
  semantics (exact/in-between/top-clamp/zero), `±1/0` step directions
  (up-across-corner V-first, down f-first, same-voltage/same-OPP no-op),
  `simple_ondemand` + 100 ms contract.
- `make kcheck` (read-only `-fsyntax-only` vs the real 3.10.108
  headers): exit **0**, no warnings/errors.
- Not exercised here (integrator/M1Verify after wiring into the
  panfrost probe): OPP step under load, suspend/resume cycle, TMU
  max-lock clamp following cdev state. No device I/O, no reboot in
  this slice.

## 5. Integrator follow-up (not this slice)

1. Wire `pf7580_dvfs_init/fini/suspend/resume/mark_busy` into the owning
   DRM driver probe/remove/`dev_pm_ops`/job-retire path; keep dev
   drvdata for the driver's own struct (this slice holds none).
2. Land-time dedup with `code/gpu31/gem/panfrost7580_devfreq.[ch]` (same
   table/order, different owners); reconcile boot seed (350 here vs 266
   there).
3. Align any TMU clamp table with the live 5-level ASV
   `{668,534,440,350,266}` (thermal report §6): ceilings above the live
   table fail closed in kbase and resolve to the top OPP here.
4. `code/gpu31/config/fragments/d4-panfrost7580.fragment` still says
   "devfreq hook is a stub; OPP table is static in C" — refresh that
   line when this slice lands (add `CONFIG_PM_DEVFREQ=y` /
   `CONFIG_DEVFREQ_GOV_SIMPLE_ONDEMAND=y`, already `y` in the stock
   j7elte config).
