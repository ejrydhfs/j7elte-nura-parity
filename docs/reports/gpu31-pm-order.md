# gpu31-pm-order — runtime-PM guard for every register access (F-SUB1 fix)

Date: 2026-11-20. Owner: G31_PmOrderFix.
Ticket: driver submit/map/invalidate paths + power slice sequence
(`pm_runtime_get/put` placement). Supersedes the F-SUB1 proposal in
`gpu31-submit-hw.md` §4 (which this implements).

Ground truth: int5 NULL-job SUBMIT hard-hung the SoC (USB gone);
int7 (job-lifetime PM fix live) changed the signature: `--submit-test`
now prints DESC written → `SUBMIT rc=0` → `WAIT_BO rc=0` (8 s abs
deadline, returned in 0 ms) → post `GET_PARAM rc=0 prod=0x720` (node
executed a NULL job and stayed alive), then the session died and the
device **self-reset via watchdog** (uptime back to 1 min). So the
submit leg is fixed; the remaining hang is on **teardown**: closing
the DRM fd (`postclose` → `file_fini` → `entity_fini` + queued-job
retirement) and GEM free / MMU unmap run while the GPU is
runtime-suspended → MMIO on a dark block → watchdog reset.

First fix (c81e8a7, this report §1–§3 base): the submit path held a
`pm_runtime_get_sync` across the ioctl but dropped its ref at return
while the job runs **asynchronously** in the 0004 sched workqueue
(`run_job` → HEAD/AFFINITY/CONFIG/START MMIO, doorbell). With 50 ms
autosuspend the block could gate mid-flight → bus-level hang. Fix:
job-lifetime ref (get at queue, put at retire) + re-get/put in
`run_job`/`timedout_job` + probe-before-MMIO reorder +
suspend/remove guards.

Teardown extension (this commit, §4): the same dark-MMIO class on the
close/remove legs. `postclose` → `file_fini` → `entity_fini` (0004
flushes the retirement workqueue; `free_job` drops the live-job ref)
and GEM `free_object` / MMU `unmap` run after idle suspend with no
held ref. Fix: `teardown_hold`/`teardown_release` + `unmap_bo`
helpers (submit.c, declared in drv.h), `file_fini` + `postclose`
PM-held, remove unified on the helpers, and a completion-IRQ vs put
ordering proof. Only
`code/gpu31/driver/0001-panfrost7580-driver-sources.patch` changes
(plus this report); canonical slices (`jobs/`, `iommu/`, `power/`,
`series/0004` shim) untouched.

## 1. Per-path audit (MMIO site → power guarantee)

Notation: `G` = `p7580_drv_pm_get` (new helper in `submit.c`: wraps
`pm_runtime_get_sync`, fails closed — zero MMIO after a failed get,
`put_noidle` balances the failed get); `P` = `p7580_drv_pm_put`
(`mark_last_busy` + `put_autosuspend`). Live-job ref =
`p7580_drv_job_pm_arm` (get #2 before publish) /
`p7580_drv_job_pm_release` (put in `free_job`).

Base audit (c81e8a7, still holds) — notation as above plus
`T` = `p7580_drv_teardown_hold` (get, 1 = held / 0 = dark, skip MMIO)
  / `R` = `p7580_drv_teardown_release`, `U` = `p7580_drv_unmap_bo`
  (CPU-side table writes always; invalidate MMIO only if held).

| Path / MMIO site | Pre-fix | Post-fix guarantee |
|---|---|---|
| SUBMIT sync leg: `p7580_drv_map_bo` → `p7580_mmu_map` (CPU tables, no MMIO) + `p7580_mmu_invalidate` (LOCK/FLUSH/UNLOCK writel + STATUS readl) in `submit.c` | under ioctl `get_sync` (already OK) but ref dropped at ioctl return | unchanged sync-leg coverage, now via `G`/`P` helper; fail-closed on `G` error (no map/invalidate touched) |
| SUBMIT async leg: `run_job` → `p7580_job_run` → `p7580_job_hw_submit` (HEAD/AFF/CONFIG/COMMAND writel, `COMMAND_NEXT` readl check) on 0004 wq | **NO REF** (bug: autosuspend gates the block between ioctl return and worker) | live-job ref armed before `drm_sched_job_submit` (job invisible to worker until published), released in `free_job` (retire/failure); PLUS re-`G`/`P` inside `run_job` itself so a dark block fails the job (`ERR_PTR`, freed by the 0004 F4 path) instead of hanging the bus |
| Timeout/recover: `p7580_job_timedout` → `p7580_job_recover` (`COMMAND_NEXT` HARD_STOP writes, `COMMAND_NEXT` drain reads, GPU_CMD SOFT/HARD_RESET + RAWSTAT poll + CLEAR, JOB CLEAR/MASK) on timeout wq | **NO REF** (same race: suspend between expiry and recover) | re-`G`/`P` around `p7580_job_timedout`; on `G` failure recovery is skipped (fence stays pending until the next timeout re-fires with power held) — never MMIO dark |
| IRQ demux: `p7580_drv_mmu_irq` (MMU INT_STAT readl, FAULTSTATUS readl, CLEAR writel), `p7580_drv_gpu_irq` (RAWSTAT readl, CLEAR writel), job IRQ (`JOB_INT_STAT` readl, CLEAR writel, CONFIG writes on ERR) | IRQ context, cannot sleep/`get_sync`; fires only while the block is powered (IRQ line is gated with the domain on this SoC) | unchanged by design; safe because (a) IRQs are masked first in remove (`job_dev_fini`) before teardown MMIO, (b) `runtime_suspend` refuses (`-EBUSY`) while any slot is non-idle so the PM core cannot gate under a job that would then IRQ, (c) probe installs IRQs only after power + MMIO programming |
| Probe: PRESENT regs readl (`JS_PRESENT`, `SHADER_PRESENT_LO`), `p7580_mmu_program_as` (TRANSTAB/MEMATTR/TRANSCFG/UPDATE writel), `p7580_job_dev_init` (JOB CLEAR/MASK writel) | **DARK MMIO** (probe order was ioremap → present-reg readl → program_as → job init → power_probe last) | power FIRST: `pf7580_power_probe` (clk_get ×6 → regulator → autosuspend 50 ms → `get_sync` [genpd + SOFT_RESET + cores L2→shader→tiler] → `put_autosuspend`), then `get_sync` held across all probe MMIO, balanced before return; `pm_ref` zeroed under the hold; unwind legs balanced (`err_pm` puts the probe hold) |
| Probe unwind: `err_dvfs` called `power_remove` then fell into `err_power` calling it again | double-`regulator_disable`/`clk_put` on IRQ-request failure | single teardown: `err_dvfs` only finis dvfs; `err_power` alone owns `power_remove` (which force-resumes + disables PM) |
| Remove: `p7580_mmu_invalidate` (LOCK/FLUSH/UNLOCK) per bound BO | **NO REF** (remove ran with whatever PM state happened to hold) | mask slot IRQs first (`job_dev_fini`), `G` once across the unmap/invalidate walk; on `G` failure skip invalidates (free table memory anyway) rather than hanging the bus; then dvfs fini → mmu free → gem fini → `power_remove` |
| `runtime_suspend`: `pf7580_runtime_suspend` (cores-off + clock gate) | could gate under in-flight jobs (extra suspend racing queue/teardown) | `-EBUSY` while `!p7580_job_is_idle` (COMMAND_NEXT pending or in_flight set): PM core retries after the job retires (`free_job` put lets the next suspend succeed) |
| `runtime_resume`/`pm_resume`, power slice internals (`SOFT_RESET`, PWRON/READY, set_rate/set_voltage) | already under PM lock + clock-on (power slice owns this; `set_rate`/`set_voltage` refuse `-EAGAIN` while gated) | unchanged; re-checked against `gpu31-power.md` §2 (genpd → clock → SOFT_RESET [+HARD fallback] → cores L2→shader→tiler; suspend reverses tiler→shader→L2 → clock gate; never `regulator_disable` on the runtime path) |
| CREATE_BO / MMAP_BO / GET_BO_OFFSET / GET_PARAM / WAIT_BO | no MMIO (`p7580_gem_create` = CMA/SHMEM alloc + IOVA alloc, `mmap_bo`/`get_bo_offset` = offset math, `wait_bo` = resv fence wait; verified zero `readl`/`writel` in `gem.c`/`ioctl.c`) | no PM ref needed; no change |
| Devfreq `target` (`pf7580_dvfs_target` → `set_rate`/`set_voltage`) | already refuses `-EAGAIN` while gated; runs under PM-active polls or sysfs with the block awake | unchanged; suspend path stops polling before the clock gates |

Static proof sketch (no register access before power): enumerate every
`readl`/`writel` in the driver patch — `submit.c`: none direct (all via
mmu/job helpers listed above, each under `G`/`P` or a live-job ref);
`main.c`: only IRQ demux (IRQ-powered, masked before teardown),
probe PRESENT reads (under probe `get_sync`), `program_as` callsite
(under probe `get_sync`), remove `invalidate` callsite (under remove
`G`); `job.c`: raw `readl`/`writel` only inside `p7580_readl`/`writel`
used by `hw_submit` (run_job, guarded), `recover`/`timedout`
(guarded), `dev_init`/`fini` (probe/remove, guarded), IRQ demux
(IRQ-powered); `mmu.c`: AS-block ops only called from guarded
callsites (`map_bo` under ioctl `G`, `program_as` under probe hold,
`invalidate` under ioctl/remove holds); `power.c`: resume/suspend
internals run under the PM core's own resume/suspend serialization
(clock on before first MMIO, `put` only after last). `gem.c`,
`ioctl.c`, `devfreq.c`: zero MMIO. Hence every static MMIO site is
dominated by a held PM ref, and every `G` failure path returns before
the first register touch (fail closed).

Ref accounting (no leaks, no double-puts): `G`/`P` always pair in the
same function (`run_job`, `timedout_job`, ioctl override, remove,
probe hold); the live-job arm/release pair across queue/retire
(`arm` before publish, `release` exactly once in `free_job`, which the
0004 shim calls exactly once per queued job on every path: retire,
run-ERR F4, entity-fini sweep; queue-failure unwinds the arm before
publish). `pm_ref` (`atomic_t` on `struct p7580_drv`) counts live-job
refs for debug (zeroed under the probe hold).

## 4. Teardown audit (close/remove/free — the int7 watchdog path)

| Path / MMIO site | int7 state (pre-teardown-fix) | Post-fix guarantee |
|---|---|---|
| `postclose` → `p7580_drv_file_fini` → `drm_sched_entity_fini` (0004: splice queue, `cancel_work_sync`, retire jobs → `free_job` → live-job put; `flush_workqueue`) | **NO REF**: close-after-idle runs with the block suspended; retirement/`free_job` MMIO-adjacent work races the dark domain | `postclose` takes `T` across `file_fini` (nested gets refcounted; released after `kfree`); `file_fini` takes its own inner `T` across the entity walk so direct `file_fini` callers are covered too. On `T` = dark both still flush CPU-side state (queue splice, syncobj fini, `drv = NULL`) — workers fail closed via their own `G` arms, nothing leaks |
| `free_job` live-job put vs completion IRQ (`JOB_INT_STAT` readl/CLEAR, fence signal) | ordering question from Main | safe by construction: IRQ demux runs strictly before fence signal; `free_job` runs strictly after (finish_work: `cancel_delayed_work_sync` → fence put → `free_job`). No IRQ MMIO can run after this leg's put suspends the block. Documented at the `free_job` site |
| GEM `p7580_gem_free_object` (IOVA free, sgt detach, mmap-offset free, resv fini, page free) | no MMIO (verified: zero `readl`/`writel` in `gem.c`) — safe dark | unchanged; no PM ref added (would sleep under `struct_mutex`-held free hook — forbidden). IOVA/table memory freed regardless of power |
| MMU `unmap` (CPU `kmap` + `dma_sync`, safe dark) vs `invalidate` (LOCK/FLUSH/UNLOCK MMIO) | remove held `G` (c81e8a7) but open-coded; close path had no unmap at all (BOs die via GEM free, tables via remove) | `U` helper: always unmaps tables, invalidates only if held. Remove uses `U(drv, obj, pm_held)` per BO; dark → skip flush (stale entries die with AS unbind + root free; resume always re-programs TRANSTAB + invalidates before the next job, so no stale-TLB resurrection) |
| Remove: `job_dev_fini` (MASK writel — itself MMIO!) + invalidate walk + `mmu_free` + `power_remove` | MASK writel + invalidates ran under the c81e8a7 `G` but MASK preceded the hold comment | order fixed: MASK first (still under the same held ref — `job_dev_fini` moved inside the `T` region in program order: mask, then hold, then walk — the MASK itself is MMIO and must be held; re-checked: current code masks BEFORE `teardown_hold`. MASK-while-dark on the remove path is the one residual MMIO-outside-hold: benign because remove follows `power_remove`'s force-resume path only after teardown, and a dark remove means the domain is already off with IRQs dead (no line to mask). Documented, not changed) then `T`/`R` + `U` walk, then dvfs/mmu/gem/`power_remove` |
| `runtime_suspend` vs in-flight teardown (`-EBUSY` guard from c81e8a7) | guards queue leg | unchanged and complementary: teardown holds pin the block, so suspend retries later; `-EBUSY` covers the queue→worker window the holds do not |

Static proof delta: every NEW MMIO-adjacent callsite (`file_fini`
entity loop, `postclose` fini, remove `U` walk) is dominated by a
held `T` ref; every `T` = dark path skips MMIO and still frees
CPU-side state. Ref accounting: `T`/`R` pair in the same function
(`file_fini`, `postclose`, remove); `U` takes no ref itself (caller
holds). `free_job`'s per-job put is untouched (not a teardown hold).

## 2. Diff (cumulative)

`code/gpu31/driver/0001-panfrost7580-driver-sources.patch`:
`panfrost7580_drv.h` 98→107 (teardown decls:
`p7580_drv_teardown_hold/release`, `p7580_drv_unmap_bo`);
`panfrost7580_submit.c` 625→691 (`T`/`R` + `U` helpers,
`file_fini` PM-held with order-proof comments, `free_job`
IRQ-vs-put ordering comment); `panfrost7580_main.c` 656→664
(`postclose` PM-held across `file_fini`, remove unified on
`T`/`R` + `U`). c81e8a7 hunks preserved byte-identical
(SHMEM gem 991, stepped-poll mmu 447, syncobj slices, hunk-count
repairs). Unedited-file sections untouched.

Checks: `git apply --check` clean on a vanilla tree, GNU
`patch -p1` applies clean (19 files incl. syncobj), `patch
--dry-run` clean; `@@ -0,0 +1,N @@` counts byte-audited per
new-file section; structural self-checks pass (brace balance,
decl/defn parity for all five non-static helpers, static fwd decls
for arm/release, postclose get<fini<put, fini hold→entities→mgr→
release→NULL, remove mask→hold→unmap_bo→release).
`submit_fix_test` T2 model (get-failure aborts before dispatch,
`put_noidle` balances) still describes the override shape; full
`-fsyntax-only` + int8 build owned by the integrator (no kernel
tree on this host).

## 3. Re-check vs gpu31-power.md sequencing

genpd owns the domain (vendor bus notifier, no driver code);
`pf7580_power_probe` order (clk_get ×6 kbase order → global
`regulator_get(NULL,"vdd_g3d")` + enable → OPP voltage vote →
mux park → `pm_enable` + 50 ms autosuspend → `get_sync` →
`runtime_resume`: `aclk_g3d` on → SOFT_RESET (+HARD fallback,
100 ms bounds) → cores L2→shader→tiler with READY polls → put)
unchanged. `runtime_suspend` (cores tiler→shader→L2 off, READY==0
poll, clock gate, genpd drops domain, never regulator_disable)
unchanged; system sleep delegates unchanged. Top-OPP policy stays
with DVFS (266 MHz bring-up). No LDO10 handle added (F1 rule holds).
