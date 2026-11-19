# G31_Thermal — GPU thermal zones, G3D OPP cooling (j7elte / Exynos 7580)

Date: 2026-11-19. Phone: kernel `3.10.108 #35` (up, RNDIS SSH works).
Tree examined read-only: `.../android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`.
No kernel `.config`/source touched; no build/flash by this slice (integrator-only).
No reboot performed (shared-phone rule). Big files: none (patches are ~11 KiB total).

## 1. Live-device ground truth (all read live 2026-11-19, kernel #35)

| Item | Value |
|---|---|
| Zones | `thermal_zone0/1` (CPU, `temp` 47/44 °C at idle) + `thermal_zone2` (GPU) |
| GPU cdev | `cooling_device2` = `thermal-gpufreq-0`, `max_state=5`, `cur_state=0` |
| GPU trips | 76/81/86/91/96/101/106/115 °C (from `EXYNOS7580_TMU_DATA_GPU` freq_tab + HW trip), all `active` type, governor `step_wise` |
| GPU zone2 `temp` | **empty read**; `curr_temp` = 5–6 (i.e. ~5 °C — sensor not tracking silicon; zones 0/1 read 44–65 °C under the same load) |
| kbase sysfs | `tmu=1`, `dvfs=1`, `dvfs_table = 668 534 440 350 266` (5-level ASV variant), `clock=0` (power-collapsed at idle), `dvfs_max_lock=-1`, all lock slots 0 |
| Config (build tree) | `CONFIG_THERMAL=y`, `CONFIG_GPU_THERMAL=y`, `CONFIG_EXYNOS_THERMAL=y`, `CONFIG_EXYNOS_THERMAL_CORE=y`, `CONFIG_MALI_DVFS=y`, `CONFIG_MALI_RT_PM=y`, `CONFIG_MALI_DEVFREQ` **not set**, `CONFIG_DEVFREQ_THERMAL` **not set** |

So the actuator path is the vendor one: TMU → `thermal-gpufreq-0` → `gpu_notifier` → `gpu_dvfs_clock_lock(TMU_LOCK)`.
There is no devfreq/OPP cooling device (`of_devfreq_cooling_register` needs
`CONFIG_DEVFREQ_THERMAL`, absent) — "devfreq max" for this slice means the
kbase `max_lock` clamp on the OPP table, which is what the TMU path drives.

## 2. Defect 1 (found + patched): step_wise cdev state never reaches the GPU

`drivers/thermal/exynos7580/gpu_cooling.c:gpufreq_apply_cooling()` emits only

```c
blocking_notifier_call_chain(&gpu_notifier, GPU_THROTTLING, &cooling_state);
```

with `GPU_THROTTLING = 7`. But `gpu_notifier.c:gpu_tmu_notifier()` handles
`GPU_COLD / GPU_NORMAL / GPU_THROTTLING{1..4} / GPU_TRIPPING` and has no
`GPU_THROTTLING` arm — the event falls in `default: "wrong event"` and does
nothing. The graded `gpu_tmu_hot_check_and_work()` is dead code on this board
(the numbered events are only sent by the legacy `exynos_thermal.c` /
`exynos7420_thermal.c`, which are not compiled for 7580).

Effect: zone trips move `cur_state`, the GPU max clock never moves.

Fix (`0001-gpu-cdev-state-to-tmu-maxlock.patch`): handle `GPU_THROTTLING` in
the kbase notifier by translating the attached cdev state into the existing
graded events (`0→NORMAL`, `1..3→THROTTLING1..3`, `>=4→THROTTLING4`, saturating;
TRIPPING never synthesized) and reusing `gpu_tmu_hot_check_and_work()` /
`gpu_tmu_normal_work()` unchanged. Monotonic by construction (verified by a
host-side C89 harness over states 0..9 + 99). No change for COLD/NORMAL/
TRIPPING, no change when `CONFIG_MALI_DVFS` is off or `tmu=0`.

## 3. Defect 2 (found + patched): TMU clamp table vs OPP table mismatch

`mali_kbase_platform.c` fills `tmu_lock_clk[]` from attributes, but:

- non-v1: slots 1..4 are written, then slots 5..8/`TRIPPING` overwrite the
  switch scope while `GPU_TEMP_TRIPPING` is read twice (aliased into both
  `THROTTLING8` and `TRIPPING` in the v1 fall-through ordering).
- v1 branch copies `THROTTLING{3..8}` into slots `{1..6}` — 800/734 vanish.
- Live table here is `{668,534,440,350,266}` while clamp entries include
  800/734 (rejected by `gpu_dvfs_get_level()` → "max lock error") and the
  thermal `freq_tab` pins trip 6 at 160 MHz, which is in neither table.

Fix (`0002-gpu-tmu-table-vs-opp-mismatch.patch`): each slot reads its own
attribute (v1: slots 1..6 ← `THROTTLING{1..6}`), `TRIPPING` reads only
`GPU_TEMP_TRIPPING`. Values unchanged — this is a slot-renumbering fix, not
a policy change. Residual mismatch (clamp entries above the live 668 table)
is left to fail closed as today and flagged as integrator follow-up below.

## 4. Verification on hardware (no reboot, short flock holds only)

- New `code/gpu31/thermal/thermal-verify.sh` (also staged at `~/g31/thermal-verify.sh`
  on the phone): read-only checks + optional `--inject` actuation via rootshell.
- Read-only: **3/3 PASS** (zone2↔cdev binding, `tmu=1`, `max_state=5` over 8 trips).
- Sustained-load soak (30 s `glmark2-es2 --off-screen`, llvmpipe): CPU zones rose
  44→65 °C; GPU `zone2/temp` stayed empty and `curr_temp` stayed 5–6 °C, `gpu clock`
  stayed 0 — consistent with §1 (GPU sensor not tracking; software GL, kbase idle).
- Injection (pre-patch baseline, restored after): `echo 2 > .../cur_state` sticks
  (`cur_state` reads back 2) but `dvfs_max_lock` stays `-1` and all TMU lock slots
  stay 0 — **reproduces Defect 1 on stock #35**. Post-patch expectation: state 2 →
  `THROTTLING2` → `dvfs_max_lock=734` on the 8-level table (or a rejected-lock
  dmesg on the live 668 table — either proves the notifier arm now runs).
- Mapping harness (host, gcc C89): PASS. Full Kbuild compile deliberately not run
  here (only G31_Integrator builds); patches are confined to two platform files.

## 5. Deliverables (this slice only, repo-committed next)

- `code/gpu31/thermal/patches/0001-gpu-cdev-state-to-tmu-maxlock.patch`
- `code/gpu31/thermal/patches/0002-gpu-tmu-table-vs-opp-mismatch.patch`
- `code/gpu31/thermal/thermal-verify.sh`
- this report

## 6. Integrator follow-up (not this slice)

1. Build + flash both patches, re-run `thermal-verify.sh --inject` via rootshell;
   expect `dvfs_max_lock` to follow cdev state; then emulate a trip
   (`/sys/class/thermal/thermal_zone2/emul_temp`, `CONFIG_THERMAL_EMULATION=y`)
   and confirm step_wise drives the clamp down and releases it.
2. Align TMU clamp entries with the live ASV table (`{668,…}` on this unit) or
   extend the DVFS table — `freq_tab[6]=160 MHz` and clamp entries 800/734
   currently resolve to no OPP level.
3. Investigate GPU `zone2/temp` empty read (TMU valid-bit path `exynos_tmu_read`
   returns `-EINVAL` → `exynos_get_temp` returns 1; CPU zones unaffected).
   Until fixed, TMU trips for the GPU zone are driven by a stale/low reading.
4. Optional: `CONFIG_DEVFREQ_THERMAL` + OPP-table cooling (panfrost path) stays
   out of scope for the kbase tree — revisit only under the M3 render-node work.

## 7. Stale-report correction (per parent notice)

Earlier gpu31 slices that claim "device unreachable / link down" are stale:
the phone is UP on kernel #35 with working RNDIS SSH as of 2026-11-19. This
report's hardware table supersedes those claims for the thermal subsystem.
