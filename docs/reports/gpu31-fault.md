# GPU31 fault injection matrix — jobs fault path + MMU decode + reset recovery

Date: 2026-11-19. Slice: `G31_FaultTests`. No kernel tree edits, no flash,
no reboot, no device writes. One read-only live fact reused from the jobs
slice (phone UP per Main, kernel `3.10.108 #35`, `gpuinfo` = Mali-T720 MP2
r1p0 `0x0720`, idle, no `/dev/dri` pre-M1): slot count comes from the
verified `js_present 0x7` capture, so this matrix models 3 slots. Host-side
only; the full timeout matrix (5 s job timeout, 3 s reset bound) runs in
milliseconds of host CPU via a virtual clock stepped explicitly by the
matrix — no wall sleeps anywhere.

## Delivered (all new, under `code/gpu31/fault/`)

| File | Lines | Role |
|---|---|---|
| `pf7580_fault.h` | ~130 | Fault taxonomy + host MMU-decode helpers shared by model and matrix (no register truth duplicated; JM side comes from the jobs header, MMU side mirrors the two kernel decode views with per-case provenance). |
| `pf7580_mock_gpu.h` | ~140 | Register-file + job/BO/map/fence state for the T720 model (3 slots, AS0, 64-BO mock table). |
| `pf7580_mock_gpu.c` | ~430 | Deterministic register model: submit/IRQ-demux/timeout/recover with the exact driver sequence (HARD_STOP → drain → SOFT→HARD → CLEAR/re-arm/AS-reprogram → fence -EIO). Host-C89. |
| `fault_matrix.c` | ~430 | 12-case matrix F1–F12 + `wait_done` poll (the spurious-timeout-guard shape). Native + qemu-aarch64 clean, zero warnings. |
| `Makefile` | ~45 | `make` / `make check` (native) + `make check-aarch64` (static + qemu). Same C89 flags/style as the jobs slice. |

## The matrix (acceptance item)

| Case | Fault injected | Expected driver behaviour (asserted) |
|---|---|---|
| F1 valid submit | none (valid BO-backed `jc`) | DONE: fence signaled err 0, slot COMMAND_NEXT 0, no reset, BO frees clean |
| F2 MMU page fault | `jc` unmapped | ERR + `ADDRESS_RANGE_FAULT`/`TRANSLATION_FAULT`/`READ` (single LPAE MMU decoder, all four views); ISR leaves fence pending, COMMAND_NEXT=NOP; recover → SOFT, fence -EIO, slot idle, MASK rearmed, AS reprogrammed |
| F3 job timeout | slot hung (`complete_at=-1`) | 5 s virtual poll expires with no fence; recover → SOFT + fence -EIO (timeout parity `P7580_JOB_TIMEOUT_MS == 5000` asserted) |
| F4 slot hang | `stop_honor=0` (HARD_STOP never drains) | recover returns -ETIMEDOUT (reset bound parity `== 3000` asserted); reset proceeds anyway → SOFT (driver always runs the reset sequence after the drain loop — the errno only reports the stuck slot) |
| F5 soft-reset failure | `soft_ok=0` (COMPLETED never asserts) | SOFT issued first (`gpu_cmd_log&1`), then HARD fallback (`&2`), kind HARD |
| F6 BO lifetime | free while in-flight | free → -EBUSY (UAF flagged), free after completion → 0, free twice / unknown → -ENOENT |
| F7 submit races | misaligned/zero `jc`, unknown BO, 2nd START on stuck slot | -EINVAL / -ENOENT / -EBUSY (recovery refusal, never a 2nd START on top); slot reusable after recover |
| F8 multi-slot | fault slot 0 while slot 1 hung | both slots HARD_STOPped, both COMMAND_NEXT 0 after recover, MASK rearmed; faulting BO frees, hung sibling stays busy (its teardown belongs to the error path, asserted -EBUSY) |
| F9 spurious timeout | timeout check after DONE | signaled fence → no recovery taken (`reset_kind NONE`, no GPU_CMD) — mirrors `job.c:387-395` bail |
| F10 permission fault | WRITE to read-only mapping (`force_fault 0x43`) | `WRITE_FAULT` + `PERMISSION_FAULT`/`WRITE` (single LPAE decoder); recover → SOFT |
| F11 decode table | all 21 JS_STATUS names + reserved | full name table, fault-vs-benign split (`>= 0x40`), all 4 LPAE classes + 4 access kinds across all four MMU views (retired short-code ADDRESS_SIZE/MEMORY_ATTRIBUTES now UNKNOWN) |
| F12 register contract | probe/steady-state | MASK `0x10001/0x20002/0x40004` pairing, AS TRANSTAB `0x3F000007`/MEMATTR `0x884D4F48`/`0x00008D88`/TRANSCFG 0, MMU IRQ pairing bits, fault-VA round-trip via FAULTADDR_HI/LO, both timeout parities |

Two deliberate model-expectation corrections during development (both the
matrix was wrong, not the driver contract): F4 first expected a stuck
drain to force HARD — re-reading `job.c:610-634` shows the drain errno
does not gate the reset (SOFT always runs first, HARD only when
RESET_COMPLETED never asserts), so F4 now asserts SOFT + errno; F8 first
expected the hung sibling's BO to free — the mock correctly keeps it
busy since recovery only fails the faulting job's fence, so F8 now
asserts -EBUSY for the sibling. Both are documented in the matrix source.

## How it maps to the driver (code/gpu31/driver assembling)

- `mgpu_submit` ↔ `p7580_job_hw_submit` (+ `submit_args` validation):
  HEAD/CONFIG/START programming, stuck-slot EBUSY refusal (the WARN_ON
  shape), one-shot forced-STATUS injection for faults the mock cannot
  produce structurally (permission class).
- `mgpu_isr_demux` ↔ `p7580_job_irq`: CLEAR sticky bits, COMMAND_NEXT=NOP
  on ERR (freeze the chain, keep STATUS/HEAD/TAIL), signal + release
  in-flight only on DONE.
- `wait_done` ↔ `p7580_job_timedout` spurious guard (`job.c:387-395`):
  signaled → return without recovery (F9).
- `mgpu_recover` ↔ `p7580_job_recover` (`job.c:587-651`): HARD_STOP
  faulting + busy slots → drain poll → SOFT→HARD fallback → CLEAR +
  MASK re-arm + AS re-program → faulting fence -EIO. One intentional
  divergence, documented in the model header: the driver leaves the
  guilty fence's retry to userspace/Mesa and reports `-ETIMEDOUT` for a
  stuck drain while still running the reset; the mock signals -EIO
  directly so the matrix can assert waiter wake-up without a scheduler.
- MMU decode ↔ single LPAE decoder (F-MMU4): `p7580_mmu_fault_code/atype`
  (`iommu/panfrost7580_mmu.c`), `pf7580_fault_exc/access`
  (`mmu/pf7580_as.h:253-282`), `mali_fault_exc/access_name` (userspace
  `mali_mmu.h`), and `pf7580_fault_mmu/lpae_*` below — low byte
  0xC0-0xC7/0xC8/0xD0-0xD7/0xD8, bits 9:8 access, ATOMIC→UNKNOWN on
  LPAE. F11 asserts all four views agree byte-for-byte, so whichever
  view the IRQ log path prints, the code is right.

## Verification (exercised, not claimed)

```
$ cd code/gpu31/fault && make          # native gcc 16.2.1
fault_matrix: 12/12 PASS
$ make check-aarch64                   # static + qemu-aarch64
fault_matrix: 12/12 PASS
```

Zero warnings at `-Wall -Wextra -std=gnu89` on both toolchains (the
`/*`-in-comment `-Wcomment` and the one unused-helper warning were fixed
before hand-off; `job-manager` truth is `#include`d, never copied, so F11
fails loudly if the jobs header ever drifts).

NOT verified (honest): no Image link, no boot, no GPU submit — those need
M1 (`/dev/dri`) + GEM + MMU-AS program + sched landed first. The matrix is
the pre-silicon gate for exactly that: every behaviour it asserts is the
behaviour the first-HW-submit recipe (`gpu31-jobs.md` §7: unmapped-jc →
ERR/0x5A → recover → IDLE) will check on silicon.

## Integrator notes

- Files: `code/gpu31/fault/{pf7580_fault.h,pf7580_mock_gpu.h,pf7580_mock_gpu.c,fault_matrix.c,Makefile}`.
  No Kconfig wiring (host harness only, like the jobs `job_test`).
- Deps: `code/gpu31/jobs/panfrost7580_jm_hw.h` (included, not vendored).
  Re-run `make && make check-aarch64` after any jobs-header change —
  the matrix is the drift detector for the fault path.
- Mock limits (explicit, not silent): single address space (AS0), no
  TLB/invalidate modelling (MMU slices own that), no scheduler
  (run/timedout/free live in the driver), BO table 64 entries, one-shot
  `force_fault[]` per slot. Anything beyond that is a HW-test job, not a
  matrix job.
