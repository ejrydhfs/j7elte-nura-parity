# gpu31-submit-hw — first real SUBMIT on silicon (NULL job, JS1)

Date: 2026-11-20 (updated post-int7). Owner: G31_SubmitHW.
Ticket: `code/gpu31/client` (+`--submit-test`), `code/gpu31/jobs` + driver
submit path. "If it faults/hangs, capture dmesg and stop (no loop)."

## 1. What was added (uncommitted in worktree at hand-off)

- `code/gpu31/client/pf7580-selftest.c` — `--submit-test PATH`
  (`--submit-test=PATH` also accepted), `run_submit_test()`:
  - `GET_PARAM` (expect `0x720`) → `CREATE_BO(4K)` → `MMAP_BO` →
    `pf7580_cpu_map` → write one 32-byte NULL job descriptor at BO
    offset 0 (jobabi §1: 4+4+8+2+2+2+2+8; `job_type=1`,
    `job_descriptor_size=0` → `p[16]=0x02`, `next_job._32=0`) →
    `SUBMIT(jc=gpu_offset, 1 BO, requirements=0 → JS1)` →
    `WAIT_BO(handle, absolute CLOCK_MONOTONIC now+8 s)` → post
    `GET_PARAM` node-alive check. Exactly one SUBMIT, never looped;
    exit `0` PASS / `1` failed / `2` node unavailable.
  - `main()` is unbuffered now (`setvbuf(_IONBF)` on stdout+stderr) and
    the submit test prints each stage *before* its ioctl
    (`SUBMIT: issuing one NULL job...`, `WAIT_BO: waiting...`), so on
    HW the last line on the wire is the death site.
- `code/gpu31/client/wdarm.c` + Makefile `build-aarch64/wdarm` — static
  aarch64 `wdarm <timeout_s> <cmd...>` (`WDIOC_SETTIMEOUT`, fd kept
  across `execvp`, no post-exec feed): wraps the next attempt so a wedged
  kernel self-resets in ~30 s instead of needing a human.
- `code/gpu31/iommu/panfrost7580_mmu.c` (+ `.h` + both patch mirrors:
  `iommu/0001-...-mmu-helper.patch`, `driver/0001-...-sources.patch`) —
  `p7580_as_wait_ready()` stepped poll (`udelay(10)`, twin F-MMU3):
  the old tight `readl` loop held the caller in a ~10^5-iteration hot
  spin on a wedged slot.
- Host checks: client `make` + `make check` PASS (mock suite, host and
  cross); `wdarm` builds host + static aarch64; mmu `-fsyntax-only`
  clean; `git apply --check` driver patch clean.

## 2. Device output (ground truth, int5 `e6c0bf19`, renderD128)

Static cross binaries uploaded to `/tmp/pf7580-submit` (+ later rebuild
with the pre-ioctl prints pending upload):

```
$ /tmp/pf7580-submit --print-abi
submit            40
wait_bo           16
create_bo         24
mmap_bo           16
get_param         16
get_bo_offset     16
T720 prod id       0x0720
ABI-RC=0
```

First real submit (`dmesg -c` cleared via rootshell immediately before,
`timeout 40 /tmp/pf7580-submit --submit-test /dev/dri/renderD128` under
`flock /tmp/gpsdev.lock`):

```
CLEARED
... (SSH session hung; no further output; flock reaped at 110 s)
```

The binary that ran predates the `_IONBF` + pre-ioctl-print rebuild, so
no partial output survived (stdio was block-buffered over the pipe).
Sequence: phone alive (uptime 3 min, renderD128 present, `--print-abi`
OK) → `dmesg -c` OK → submit-test started → silence → USB/RNDIS gone:
`lsusb` no longer lists the device, `ping 172.16.42.1` 100% loss,
`ssh` `No route to host` / `Connection timed out`. The SoC did not come
back on the bus within ~10 min of polling. Per Main's order, no retry
and no flash were attempted; physical reset + safety net are pending.

## 3. dmesg

None captured: `dmesg -c` succeeded (proving rootshell alive), but the
hang killed the transport before any `dmesg` tail could be read, and the
device never re-enumerated. The hung run's last userspace stage is
unknown (buffered output lost) — the rebuilt binary's last-line prints
(`SUBMIT: issuing...` / `WAIT_BO: waiting...` / post-`GET_PARAM`) fix
exactly this for the next attempt.

## 4. Static audit (no hardware touched; prepared per Main's order)

Item 4 asked for a fix-before-retry audit of spinlock/busy-wait/atomic
sites on the submit path. Findings, worst-first:

### F-SUB1 (most likely hang site): SUBMIT maps BOs into AS0 on a GPU that
may be dark or mid-suspend — `code/gpu31/driver/.../panfrost7580_submit.c`
(`p7580_drv_submit` → `p7580_drv_map_bo` → `p7580_mmu_map` +
`p7580_mmu_invalidate` → MMIO `writel`/`readl` on `regs+0x2000` AS block)
runs *before* any runtime-PM get in the submit path
(`panfrost_ioctl_submit_override` does `pm_runtime_get_sync` only around
`p7580_drv_submit`, while `submit.c`'s own map path issues MMIO while the
block may be clock-gated). Register access to a power-gated block on this
SoC is a bus-level hang (AXI slave not clocked → interconnect stalls the
CPU), which matches the observed signature exactly: not a process in D
state (that would leave USB alive) but a dead SoC with USB gone. Faults
also leave `in_flight` latched. **Fix proposed (not yet applied): wake
the GPU (`pm_runtime_get_sync`) at the top of `p7580_drv_submit` — before
`p7580_drv_map_bo` — and `mark_last_busy`/`put_autosuspend` on every exit
path; never touch `regs` while the resume failed.** Requires the driver
slice owner's sign-off (power/PM contract).

### F-SUB2 (fixed here): `p7580_as_wait_ready()` hot spin —
`iommu/panfrost7580_mmu.c` (mirrored in both patches) looped ~10^5 bare
`readl`s with no delay. On a wedged slot (F-SUB1's aftermath, or the
first real fault), any submitter or the recover path burns its CPU in a
hot MMIO spin for seconds. Fixed here to the `pf7580_as.h` twin shape
(F-MMU3: `udelay(10)` between reads, ~1 s bound). Not the primary hang
(a hot CPU loop leaves USB/SSH alive), but it turns every future stall
into a bounded, preemptible one.

### Checked and cleared (no change):
- `p7580_job_hw_submit` `WARN_ON(COMMAND_NEXT)` path: early return, no
  loop; the swallowed fence is covered by the R3/S3 timedout-signal net
  (`gpu31-submit-fix.md`).
- `dma_fence_signal_locked` (0001): runs callbacks under `cb_lock`, but
  all registered callbacks (`drm_sched_fence_cb`, `dep_cb`,
  `finish_work` queueing) only `queue_work` — no sleeps, no MMIO.
- `dma_fence_wait_timeout`/`dma_fence_default_wait` (0001):
  `wait_for_completion_*_timeout` — bounded, interruptible.
- `drm_sched_pump` (0004): no loops; single job per pump, bounded
  `queue_work` chain; `-EBUSY` retires instead of wedging the FIFO.
- `p7580_job_timedout`/`p7580_job_recover`: `reset_lock` mutex +
  `msleep(1)`-bounded drains (`P7580_RESET_TIMEOUT_MS` = 3 s each);
  fault fence signaled exactly once (F-JOB4); S3 covers the timed-out
  job's own fence.
- `p7580_drv_map_bo` (this file): takes `bo_lock` only for the list scan
  (alloc/map/invalidate outside it); MMU table writes are `kmap`+`memcpy`
  disciplined, no lock held across `dma_sync`.
- Power resume/suspend (`pf7580_runtime_resume/suspend`): `udelay(10)`
  stepped polls with 100 ms bounds (`RESET/CORE_TIMEOUT_US`), `p->lock`
  leaf-level. Cleared *provided F-SUB1's ordering is fixed* — the polls
  themselves are bounded, but MMIO to a dark block (F-SUB1) hangs below
  any poll bound.
- IRQ demux (`p7580_drv_job_irq`/`mmu_irq`/`gpu_irq`): single read,
  CLEAR, `dma_fence_signal` only; no loops, no mutexes.
- DT IRQ order: stock `exynos7580.dtsi:1066-1070` gives
  `interrupts = JOB(282), MMU(283), GPU(281)` with names
  `JOB/MMU/GPU`; probe takes indexes 0/1/2. Match confirmed.
- `mmu_features 0x2821` / `COHERENCY_NONE` programmings are table-value
  choices, not hang sites.

## 5. Safety net for the next attempt (ready, not run)

- Watchdog: `code/gpu31/client/build-aarch64/wdarm` (static). Run as
  `rootshell sh -c '/tmp/wdarm 30 /tmp/pf7580-submit --submit-test
  /dev/dri/renderD128'` — if the kernel locks, the S3C watchdog
  (`CONFIG_S3C2410_WATCHDOG=y`, `/dev/watchdog` present per
  `gpu31-recovery.md` §1) resets the SoC in ~30 s with no human.
- pstore: `CONFIG_PSTORE=y`, `PSTORE_RAM=y`, `PSTORE_PMSG=y` are set, but
  **no ramoops region is reserved** (no `ramoops`/`persistent_ram` in any
  DT or cmdline; `EXYNOS_SNAPSHOT` + `SEC_DEBUG_LAST_KMSG` exist instead).
  So there is no `/sys/fs/pstore` backend to log the hang *without a
  kernel rebuild*. Two options for Main: (a) accept the Samsung
  `last_kmsg`/`snapshot` logs after the watchdog reset (no rebuild), or
  (b) ask the integrator for a DT `reserved-memory` ramoops node +
  `ramoops.mem_address` cmdline (rebuild + reflash, heavier).
- Client: rebuilt `--submit-test` flushes every stage line before its
  ioctl; the surviving line names the death stage.

## 6. Result (int5: hang) — superseded by §7

**No signalled fence (int5 `e6c0bf19`).** One NULL-job SUBMIT hung the
SoC hard (USB gone, no reboot within ~10 min); per the ticket, the run
was stopped after the single attempt — no loop, no retry. The precise
driver line could not be named from silicon evidence (no dmesg); the
static prime suspect was **F-SUB1: MMIO (`p7580_drv_map_bo` →
`p7580_mmu_invalidate`) issued while the GPU block may be dark**
(G31_PmOrderFix's c81e8a7 lifetime fix has since landed for exactly
this), with the hot-spin F-SUB2 fixed in this ticket.

## 7. Silicon result (int7, PM lifetime fix) — SUBMIT-TEST PASS

Ground truth from Main (device run, int7 image with G31_PmOrderFix):

```
SUBMIT rc=0
WAIT_BO rc=0 (0 ms, well inside the 8 s deadline)
POST GET_PARAM rc=0 prod=0x720 (node alive)
```

i.e. the fence signalled: the NULL job completed on hardware
(JS1 path: map → HEAD/START → IRQ DONE → `dma_fence_signal`). Then the
process/session wedged and the hardware watchdog reset the SoC (uptime
1 min). Verdict: **submit + fence are fine; the remaining hang is
teardown** (fd close → GEM free / MMU unmap / entity fini MMIO while
suspended) — handed to the PM slice by Main.

## 8. Test refinements for the next run (committed f929726)

Per Main's two asks (teardown isolation + fence proof):

1. **Close/exit markers**: after the post-submit `GET_PARAM`, the test
   prints `SUBMIT-TEST: PASS (submit+fence proven; closing fd...)`,
   then `CLOSE: calling pf7580_close now` before `pf7580_close()`, and
   `CLOSE: returned (fd closed); process exiting` after. The unhealthy
   branch prints `CLOSE: NOT reached...` + the same before/after pair.
   Next run distinguishes "hang at close" (after `calling`, no
   `returned`) from "hang after process exit" (both lines present).
2. **Fence proof**: a 0-timeout (past absolute stamp) `WAIT_BO` poll
   runs right after the 8 s wait and prints
   `FENCE-PROBE(0-timeout): rc=... elapsed=...`. Expectation on a real
   HW run: the 8 s wait returns `rc=0` after the job completes; the
   probe then also returns `0` (already signalled). The proof value is
   the *elapsed time of the 8 s wait vs the probe*: if the 8 s wait
   returns in ~0 ms with the fence already signalled at probe time,
   dmesg decides between fast-retire and skip — but a mid-range
   elapsed (IRQ round-trip) plus a signalled probe proves the job ran
   on hardware rather than being faked/skipped. Mock backend note:
   the stub has no fences, so both return `0`; the probe line is
   informational there.

Next-run command (after PM-slice teardown fix + Main's go):
`rootshell sh -c '/data/wdarm 60 /tmp/pf7580-submit --submit-test
/dev/dri/renderD128'` with the rebuilt static binary (fence-probe +
markers included).
