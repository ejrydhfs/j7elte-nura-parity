# GPU31 jobs slice — job submission (JM, slots, atom dependency)

Slice: `jobs`. Status: **code + host test + kernel compile check done;
no hardware submit yet** (needs the M1 flash gate + GEM/MMU/sched
slices; see §7).

No kernel tree edits, no flash/reboot, no device writes in this slice.
One read-only live check was used for grounding (phone UP per Main
2026-11-19, kernel `3.10.108 #35`): `gpuinfo` = `Mali-T72x MP2 r1p0
0x0720`, `utilization` = `0`, `/dev/dri` absent, 1.1 GB free RAM.
`js_present`/`shader_present` sysfs nodes do not exist (only
`gpuinfo`/`core_mask`/power nodes), so slot count comes from the
verified `gpu-verify.md` capture (`js_present 0x00000007`, 2 shader
cores, 33 VA bits, coherency NONE) — consistent with
`BASE_JM_MAX_NR_SLOTS 3`.

Prior art: `gpu-panfrost.md` (register/MMU audit), `gpu-drm.md`
(M1–M3), `gpu31-target.md` (v5.2 UAPI lock), `gpu31-dt.md`
(zero-DT-change rule), `code/gpu31/iommu/` (MMU programming),
`docs/reports/gpu31-review.md` (this slice answers its jobs-relevant
findings in §6).

Delivered under `code/gpu31/jobs/` (produce patch files; only the
Integrator applies/builds):

| File | Lines | What it is |
|---|---|---|
| `panfrost7580_jm_hw.h` | ~230 | Pure-C JM hardware interface: JS/COMMAND/STATUS/CONFIG offsets, IRQ demux, JS_COMMAND/JS_CONFIG values, GPU reset regs, timeouts, FS→JS0/else→JS1 routing, T720 `p7580_js_config()` (START_MMU always, no FLUSH_REDUCTION/JOBCHAIN/PROTECTED), 17-entry fault table, 32-byte descriptor + v5.2 SUBMIT layout |
| `panfrost7580_job.h` | ~130 | Submit-path state: per-slot job/file-ctx/device structs (real locks in-tree, opaque placeholders host-side), 6 entry points |
| `panfrost7580_job.c` | ~640 | Slot manager + SUBMIT + IRQ + timeout + recovery (see §3–§5) |
| `job_test.c` | ~110 | Host unit test (60 checks, native PASS) |
| `Makefile` | ~50 | `make check` (host test) + `stage-shims`/`kcheck` (kernel `-fsyntax-only` + `-c` vs real 3.10.108 headers with staged 0001-0004; read-only tree) |

## 1. Hardware model (T720 MP2, verified)

- **3 slots.** `BASE_JM_MAX_NR_SLOTS 3`
  (`mali_kbase_defs.h:134`); live `js_present 0x7` (`gpu-verify.md`
  §4). Slot base `0x1800 + n*0x80` — kbase `JOB_SLOT_REG` and
  mainline `JS_BASE` agree byte for byte (`gpu-panfrost.md` §3.1).
- **Routing.** v5.2 `panfrost_job_get_slot`: `PANFROST_JD_REQ_FS`
  (bit 0) → JS0 (fragment); everything else → JS1
  (vertex/tiler). JS2 (compute) exists in hardware but the v5.2 UAPI
  has no compute-only bit — wired, idle. Matches kbase atom-type
  flags (`BASE_JD_REQ_FS/CS/T` in `mali_base_kernel.h`).
- **T720 quirk deltas** (from KT `mali_base_hwconfig_*.h` + [KB]/[V5]
  submit paths):
  - `base_hw_features_t72x` (features.h:72-84) has **no**
    `FLUSH_REDUCTION`, **no** `JOBCHAIN_DISAMBIGUATION`, **no**
    `PROTECTED_MODE` → the `JS_CONFIG` built here omits all three
    (v5.2 sets FLUSH_REDUCTION/flush_id conditionally; kbase toggles
    JOBCHAIN per-submit — both dropped).
  - `base_hw_issues_model_t72x` (issues.h:566-581) **has 10649** →
    `JS_CONFIG_START_MMU` always set (same as kbase
    `mali_kbase_jm_hw.c:117-118` and v5.2 `panfrost_job_hw_submit`).

## 2. Job header / cfg descriptors (what the GPU consumes)

- **Descriptor** (`struct p7580_job_desc`, 32 B): `mali_kbase_defs.h`
  1689-1712 "identical for all GPU archs" — exception_status,
  first_incomplete_task, fault_pointer, size/type/barrier bitfields,
  job_index, dep indices 1/2, next_job. The `jc` in SUBMIT points at
  this; the MMU slice validates the mapping before HW sees it.
- **CONFIG_NEXT** (`p7580_js_config(as_nr)`): `as_nr` low bits (kbase
  style: `cfg = kctx->as_nr`, jm_hw.c:102) `|` START/END
  CLEAN_INVALIDATE `|` START_MMU `|` THREAD_PRI(8). No
  conditional bits — every T720 submit uses the same cfg.
- **Affinity**: `JS_AFFINITY_NEXT_{LO,HI}` = full `shader_present`
  mask (live: 2 cores). v5.2 and kbase agree; the SEC HWCNT_VERT
  `0x3F` special case in kbase (`gpu_exynos7580.c` path) is
  Exynos-kbase-only and not carried over.
- **Submit sequence** (NEXT-view, then GO): HEAD_NEXT_LO/HI ← `jc`,
  AFFINITY_NEXT ← mask, CONFIG_NEXT ← cfg, `COMMAND_NEXT = START`
  (`0x01`). Guard: `COMMAND_NEXT != 0` means the slot is stuck →
  `WARN_ON` + recovery, never a second START on top (mirrors kbase's
  `KBASE_DEBUG_ASSERT(kbasep_jm_is_js_free)`).

## 3. Submit path (ioctl → slot → fence)

Mirrors v5.2 `panfrost_job_push` + `panfrost_job_run`, adapted to the
0001-0004 shim ops (run/timedout/free only):

1. **Validate** (`p7580_job_submit_args`): `jc` 8-byte aligned and
   nonzero; `requirements` ⊆ {FS, ONLY_COMPUTE}; `in_sync_count` ≤
   16, `bo_handle_count` ≤ 64; slot legal **and** present in
   `js_present` (else `-ENODEV`).
2. **Explicit deps**: `in_syncs` handles → fences via the DrmCore
   syncobj callback (0003 `find_fence`). Owned by the job; freed in
   `p7580_job_free`. One-in-fence contract is explicit end to end
   (see §6 F3).
3. **BO reservation**: handles copied in; GEM callback resolves each
   to its resv (0002 locking discipline per review: resv->lock held
   by the GEM side; this file never touches resv lists directly).
   The sched-side `sched_submit` callback attaches them as shim
   dependencies (`add_dependency`) and retires them as exclusive on
   completion — same shape as v5.2
   acquire/attach_object_fences.
4. **Dispatch**: `sched_submit(slot, job)` → per-slot entity FIFO in
   the 0004 shim → `p7580_job_run` creates the per-slot ordered
   fence (context per slot, monotonic `emit_seqno`), programs the
   slot, returns the in-flight fence.
5. **Out-sync**: `render_done_fence` installed into `out_sync` via
   the DrmCore callback (0003 `replace_fence`).
6. **Completion**: JOB_INT DONE(j) → `dma_fence_signal(done_fence)`
   (wakes sched retirement + any WAIT_BO waiter). ERR(j) →
   COMMAND_NEXT=NOP + sticky CLEAR; the fault is read + recovered in
   process context (§5), never in the IRQ.

UAPI is v5.2 byte-identical (`SUBMIT` 0x00 … `GET_BO_OFFSET` 0x05,
`GET_PARAM` = `GPU_PROD_ID` only, `CREATE_BO.flags` reserved-zero)
per `gpu31-target.md` — stock Mesa `panfrost_dri.so` is the
unmodified userspace.

## 4. Timeouts

- `P7580_JOB_TIMEOUT_MS = 5000`: **intentionally equal** to the 0004
  shim's hardcoded dispatch timeout (`pump:
  msecs_to_jiffies(5000)`). The shim's `sched->timeout` is currently
  unreachable (review §4 F5: no entity→sched back-pointer), so this
  slice pins both to 5 s and documents the one-line wiring
  (`entity->sched` + `pump` uses `sched->timeout`) for G31_Sched.
- `P7580_RESET_TIMEOUT_MS = 3000`: slot-drain + `RESET_COMPLETED`
  poll bound; matches kbase `DEFAULT_JS_SOFT_JOB_TIMEOUT` and
  `DEFAULT_RESET_TIMEOUT_MS` (config_defaults.h:185,210).
- Spurious-timeout guard: `timedout_job` bails if `done_fence`
  signaled between dispatch and the timeout work (same as v5.2).

## 5. Fault handling + resume/recovery design (acceptance item)

State machine per slot: `IDLE → SUBMITTED (in_flight set,
COMMAND_NEXT=START) → DONE (IRQ, fence signaled)` or `→ FAULT
(ERR bit, STATUS latched) → RECOVERING (reset_lock held) → IDLE`.

**IRQ (no sleeps, no mutexes):** read `JOB_INT_STAT`; for each set
slot bit: `JOB_INT_CLEAR(mask)`; on ERR: `COMMAND_NEXT=NOP` (freeze
the chain, keep STATUS/HEAD/TAIL for the dump) and leave recovery
to process context; on DONE: pull `in_flight[j]` under `hw_lock`,
`dma_fence_signal(done_fence)`.

**Recovery (`p7580_job_recover`, process context only — timeout
workqueue or submit-error path):**

1. Take `reset_lock` (serialises concurrent slot faults; the IRQ
   never takes it).
2. `HARD_STOP` the faulting slot; also any other present slot with
   `COMMAND_NEXT != 0`. (`MULTIPLE_FAULT` = GPU IRQ bit 7, read by
   the caller from `GPU_INT_STAT` — all busy slots stop.)
3. Poll `COMMAND_NEXT == 0` on every slot up to 3 s (`-ETIMEDOUT`
   if a slot never drains).
4. `GPU_CMD = SOFT_RESET`; poll `RESET_COMPLETED` up to 3 s;
   fall back to `HARD_RESET` on timeout (kbase + v5.2 sequence;
   `M0` already proves the poll shape on this tree).
5. Re-arm: `JOB_INT_CLEAR` all slot bits, `JOB_INT_MASK =
   present DONE+ERR`. The MMU slice re-programs `AS_TRANSTAB`
   between reset and re-arm (integrator wires the call — the AS
   program is owned by G31_Mmu/G31_Iommu, not duplicated here).
6. Fence accounting: the faulting job keeps its `done_fence`
   unsignaled until the retry policy decides; the fence is put in
   `p7580_job_free`, never leaked on the ERR path (contrast review
   §4 F4 — this slice's run path has no ERR_PTR-drop: `p7580_job_run`
   returns the fence or the ERR_PTR itself, and `p7580_fence_create`
   frees nothing on the single OOM path because nothing was linked).

**Fault decode** for the log path: full `JS_STATUS` table
(`p7580_js_status_name`, KT regmap :418-451): benign
`INTERRUPTED/STOPPED/TERMINATED` vs fault `CONFIG/POWER/READ/WRITE/
AFFINITY/BUS/INSTR_*/DATA_INVALID/TILE_RANGE/ADDRESS_RANGE/
OUT_OF_MEMORY/UNKNOWN`. `p7580_js_status_is_fault` gates the
recovery path (`status >= 0x40`).

**What recovery does NOT do** (shim limits, explicit): no
`drm_sched_stop/resubmit_jobs/karma` (the 0004 shim has no
stop/resubmit — review §4) — slots restart directly after reset;
no per-context guilty tracking; no cross-slot dependency replay
(single address space, FIFO per slot, Mesa re-submits the batch).

## 6. Review findings affecting this slice (gpu31-review, 625ed18)

G31_Jobs owns none of the 0001-0004 patches, but this slice's code
touches all four shims — fixes/callouts:

- **0001 dma-fence [CONDITIONAL] F1/F2/F3 — worked around in this
  slice, fix belongs to G31_Sched.** The landed 0001 has no
  `dma_fence_init` and keeps the dangling `lock` pointer + wrong
  `default_wait` return. `panfrost7580_job.c` does **not** call the
  missing helper: `p7580_fence_create` inits `refcount/completion/
  cb_list/cb_lock/flags/ops/context/seqno` by hand exactly as the
  fixed helper will, and nothing dereferences `fence->lock`.
  `default_wait`'s overlong-budget bug does not affect the submit
  path (no chained resv waits here — one fence per job). State the
  fix explicitly: G31_Sched adds `dma_fence_init` (kref, completion,
  list, lock-init-to-NULL, flags-clear), deletes `lock`/
  `default_work`/`status`, returns the `wait_for_completion_*`
  remainder. This slice compiles against either version.
- **0002 dma-resv [NEEDS FIXES] F1/F2 — contract, not code, in this
  slice.** This file never touches resv lists directly: BO reserve /
  resv-lock / fence-pin-across-wait live behind the GEM callback
  (`bo_reserve`) and the Sched `sched_submit` callback. State the
  fix explicitly: G31_Gem/G31_Sched take `resv->lock` in every
  mutating helper + snapshot refs under lock in
  test_signaled/wait_timeout (`dma_fence_get` under lock, wait after
  unlock), per review §2. The SUBMIT path remains correct under
  either version because it holds no resv references itself.
- **0004 drm_sched [NEEDS FIXES] F1–F5 + push_job — adapted to, not
  repeated.** This slice uses only the three ops that survive the
  fix (`run_job`/`timedout_job`/`free_job`) plus alloc/submit; it
  does not rely on the broken entity list, does not share
  `finish_cb` (one `in_fence` array drained in `free`, explicit
  one-in-fence contract end to end matching the Mesa backend), has
  no ERR_PTR-drop (see §5), pins the timeout to the shim's 5 s
  until `entity->sched` lands, and never calls the undefined
  `entity_push_job` (the `sched_submit` callback is the seam).
  State the fix explicitly: G31_Sched separates membership list
  from job FIFO, gates `pump` on dep readiness, splits dep vs
  completion callbacks (or documents one-dep), frees/requeues on
  ERR_PTR, threads `sched->timeout` through, implements or deletes
  `push_job`.
- **G31_DrmCore display/DT/CMA (§6–§8):** no impact — this slice
  programs no clocks, no DT, no CMA; PM is the vendor pd-g3d
  notifier (M0-resolved `aclk_g3d`/`vdd_g3d`), power sequencing is
  out of scope.
- **G31_DmaBuf import-by-copy / vmap-unpinned (§10):** no impact —
  this slice copies no buffers; GEM/MMU slices own the import path.
- **G31_Mesa `n_slots` off-by-one (§12 F1):** no impact — this
  slice's slots are the 3 JM slots, not Mesa atom slots; noted so
  the Mesa fix (`n_slots = 2 + wait`) is not confused with JM
  slot accounting.
- **G31_KbaseShim 48/56 stride (§11 F1):** no impact — this slice
  submits native v5.2 descriptors, never translated r7p0 atoms.
  Noted so the stride fix (64 + per-atom expansion) stays in the
  shim slice.

## 7. Verification (exercised, not claimed)

- Host: `make check` → `job_test: all checks passed` (60 checks:
  register offsets/IRQ bits, slot routing, JS_CONFIG construction +
  T720 exclusions, 6 fault names + fault-vs-benign split, timeouts,
  32-B descriptor + SUBMIT layout sizes, caps).
- Kernel: `make stage-shims` stages 0001-0004 from
  `code/gpu31/series/` into a private dir (shared tree untouched);
  `aarch64-linux-gnu-gcc -fsyntax-only -std=gnu89` → **0 errors**;
  `-c` object → **0 errors**, `panfrost7580_job.o` 16 232 B.
  Only tree-header warnings (unused params in `pgtable.h` etc.),
  none from this slice. (Host `cc` == gcc 16.2.0 per day-plan env.)
- Live device (read-only, short lock): #35, `Mali-T72x MP2 r1p0`,
  idle (`utilization 0`), no `/dev/dri` — confirms the driver has
  no slot competition yet (kbase owns the block until cutover
  `CONFIG_MALI_MIDGARD=n`), and the T720 feature/issue tables used
  for the cfg (§1) match this exact silicon.
- NOT verified (honest): no Image link (Integrator serialises the
  one build), no boot, no GPU submit, no fault injection — those
  need M1 (`/dev/dri`) + GEM (CREATE_BO/MMAP_BO) + MMU (AS program)
  + sched (per-entity FIFO) landed first. First-HW-submit recipe
  for the integrator: soft-job jc in a mapped BO → SUBMIT with no
  in-syncs → expect DONE(1) + `done_fence` signaled, HEAD/TAIL
  advance; then `ADDRESS_RANGE_FAULT` injection (unmapped jc) →
  expect ERR(1)+STATUS 0x5A → `p7580_job_recover` → slot IDLE.

## 8. Integrator notes

- Files: `code/gpu31/jobs/{panfrost7580_jm_hw.h,panfrost7580_job.h,
  panfrost7580_job.c,job_test.c,Makefile}`. No Kconfig/Makefile
  wiring in this slice (driver `drivers/gpu/drm/panfrost/` owns
  that at M3 assembly).
- Deps: 0001-0004 headers (`dma-fence.h`, `dma-resv.h`,
  `drm_syncobj.h`, `drm_sched.h`); GEM (handle→resv + WAIT_BO),
  MMU (`jc_validate` + AS re-program after reset), Sched
  (per-slot entity + deps), DrmCore (syncobj table + ioctl numbers
  from `DRM_COMMAND_BASE` clear of exynos GEM ioctls — review §3
  F-note), Power (devfreq transition hook where the `pm_runtime`
  calls were in v5.2).
- Do NOT apply with the §6–§8 display-swap path and the drm-m1
  node-only path at once (review §9: alternatives, not
  complements).
