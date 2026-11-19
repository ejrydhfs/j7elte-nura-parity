# gpu31-stress — kbase winsys + planned DRM-path stress, hang-recovery, watchdog policy

Slice: `code/gpu31/stress/` — `kbase-stress.c` (5 soft-job-only suites) +
`Makefile` (native + static aarch64 cross). Commit only my files.

Live-device note (2026-11-19, kernel #35 `3.10.108` SMP PREEMPT, gcc 16.2.0):
phone is UP over RNDIS/SSH. `/dev/mali0` present (DDK `K:r15p0-00rel0(GPL)`,
UK 10.6), `/dev/dri` absent, `/dev/fb0` + Xorg present. All device runs below
used short `flock /tmp/gpsdev.lock` SSH holds as root. No reboot, no flash,
no kernel build. Root fs 94% full — binary staged in `$HOME` (`/data` is not
user-writable; root-only). Earlier "link down" reports are stale; the phone
rebooted between the stress run and this report (uptime 1 min at report
time), which released the intentionally-stuck test contexts (see §3).

## 1. What the tool does

`kbase-stress [--device PATH] [tests...] [--jobs N] [--stuck-ms MS]
[--churn N] [--sizes a,b,c] [--fence-ms MS] [--softwait-ms MS]`,
`--print-abi` for the wire check. Exit 0 pass / 1 fail / 2 no device.
Self-contained C11 + the v1 `kbase.c` client (`kbase_uapi.h` ABI asserts:
atom 64, submit 40, event 24, UK 10.6).

Only `BASE_JD_REQ_SOFT_*` atoms are submitted (DUMP_CPU_GPU_TIME, fence
trigger/wait, soft-event wait). No Midgard job chain ever reaches the shader
cores, so no suite can wedge the GPU: every payload completes in the kernel
soft-job handler without touching the job slots. Safe to run while X/fbdev
and the GPS/FM workstreams share the phone.

Suites: `many` (N sequential soft jobs, per-job latency + stuck line +
GPU-cycle delta), `bo` (alloc/submit/free churn over page sizes, free-RAM
delta), `fence` (stuck wait+trigger batch on a never-firing fence, userspace
deadline detection, out-fence still pending, fresh-fd canary, close-fd
release), `softwait` (EVENT_WAIT with no SET, kernel-cancel latency vs the
3 s soft-job timeout), `fault` (bad stride / bad dep type / dep-on-unused —
each must be contained, then a canary).

## 2. Hardware results (all PASS, kernel #35)

Full battery (`--jobs 200 --churn 300 --fence-ms 1500 --softwait-ms 8000`):

- `many-small-jobs`: n=200, avg **0.08 ms**, worst 0.13 ms, stuck=0,
  gpu_cycles_delta=170128, total 0.02 s. No per-job wait anywhere near the
  2000 ms stuck line; the event path is ~80 µs/job.
- `bo-churn`: 300 iters over 1/4/16 pages — freeram 1066 MiB before AND
  after (delta +0 KiB). No cookie/mmap/VMA leak.
- `fence-timeout`: pending fence fd 5 (never fires) → stuck batch (atoms
  20/21) correctly pending after 1500 ms; out-fence fd 6 correctly
  unsignalled; canary on a fresh fd OK (cycles=283). PASS.
- `soft-event-cancel`: EVENT_WAIT with no SET → `JOB_CANCELLED (0x4002)`
  after **3.01 s** (budget 8000 ms) — the kernel's 3000 ms soft-job timeout
  cancelled the stuck soft-event wait exactly on schedule. PASS.
- `fault-inject` (separate run): bad-stride submit → ioctl ret 3 (sync
  reject, `Stride ... doesn't match kernel` in dmesg); bad-dep-type submit
  accepted then atom 31 → `JOB_CONFIG_FAULT (0x40)`; dep-on-unused → atom 32
  → `JOB_INVALID (0x4003)`; canary OK (cycles=227). 3/3 contained, PASS.

dmesg after the battery: only the expected stride message plus unrelated
wifi/sshd/seccomp lines — no `JS: Job Hard-Stopped`, no GPU soft-reset, no
oops. GPU utilisation back to 0. Hang-recovery finding: stuck *soft-job*
atoms never engage the job-slot scheduler, so the JS soft-stop (300 ms) /
hard-stop (350 ms) / GPU-reset (400 ms) escalation (see §4) is not involved;
recovery is per-context (close the fd; `kbasep_js_remove_cancelled_job`).

One test-design correction from the hardware: a FENCE_WAIT atom with
`fd=-1` does NOT hang — it fails synchronously with `JOB_INVALID (0x4003)`.
A genuinely stuck batch needs a *valid but never-signalled* fence, built
from a trigger atom chained behind a never-SET soft-event wait (§3 does
exactly this). The old assumption is recorded here so nobody re-learns it.

## 3. Watchdog policy (what to enforce in the winsys / kbase backend)

Kernel side (r15p0 sources, live values from
`/sys/devices/11400000.mali/{js_timeouts,soft_job_timeout,reset_timeout}`):
scheduling tick 50 ms; soft-stop 300 ms (6 ticks) / hard-stop 350 ms (7) /
GPU soft-reset 400 ms (8) for shader jobs (CL identical here; 8408 variants
N/A); soft-event-wait cancel **3000 ms**; Samsung vendor fence timer
**1000 ms** (`KBASE_FENCE_TIMEOUT`, `gpu_integration_callbacks.c`) — logs
`Release fence is not signalled` to dmesg but does NOT cancel or fake the
fence (observed: still pending past the timer). `CONFIG_MALI_FENCE_DEBUG=y`
is set on this kernel. HW-job escalation only fires for atoms actually on
the job slots; soft-job stalls are handled per-context as above.

Userspace policy (implemented in the stress tool; port to `kbw_*`):

1. Every blocking wait (event wait, fence wait, slot wait, wait-idle) takes
   an explicit deadline and returns `-ETIME` on expiry — never `poll(-1)` /
   `pthread_cond_wait` without a timeout. Stuck-line default 2 s for soft
   jobs (well under the 3 s kernel cancel, well over the 0.1 ms norm).
2. Distinguish *rejected* (`JOB_INVALID 0x4003`, `CONFIG_FAULT 0x40`, submit
   ret) from *stuck* (no event by the deadline): rejected → propagate the
   event code; stuck → treat the context as suspect, run a canary soft job
   on a fresh fd, and recycle (close) the stuck context.
3. Fence waits: deadline on the wait side AND a `poll(fd,0)` liveness check
   on the out-fence; unsignalled-past-deadline is CONTTAINED, not fatal —
   the canary decides.
4. Fault containment: malformed submits must never take down the device
   context — verify with the §2 `fault` triple after any submit-path change.
5. dmesg tripwires for CI: `JS: Job Hard-Stopped`, `Issueing GPU soft-reset`,
   `Release fence is not signalled` (warning only), `Stride ... doesn't
   match kernel` (test artifact, not a driver bug).

## 4. Planned DRM path (panfrost backport) — ready-to-run plan

The H1 render node does not exist yet (`/dev/dri` absent until M1 lands), so
no DRM stress ran. When it does, the same five shapes apply 1:1 with DRM
nouns (render fd, `drm_panfrost_submit` + syncobjs, GEM churn via
CREATE/MMAP/close, `WAIT_BO` deadline, bad-CREATE/bad-SUBMIT injection):

1. `render-node-probe` classifies H1/H2/NEITHER (gates which backend runs).
2. many-small-BOs: N `CREATE_BO(4 KiB)` + `MUNMAP`/`close`, latency + leak
   check (fd count + `/proc/meminfo`).
3. fence timeout: `SUBMIT` with an out-syncobj, `WAIT_BO`/syncobj-wait with
   a userspace deadline; stuck → `drmSyncobjReset`/context recycle canary.
4. fault inject: over-size CREATE, bad SUBMIT stride/padding, wait on an
   unsignalled syncobj — each contained, then a canary submit.
5. hang-recovery assertion: no `drm_sched` job timeout / GPU reset in dmesg
   for soft-only loads; if a real Midgard hang occurs, the backport's
   `drm_sched` timeout → job TDR → scheduler fence error path applies (not
   the kbase JS tick path of §3) — record which path fired.

## 5. Files and reproduction

- `code/gpu31/stress/kbase-stress.c`, `code/gpu31/stress/Makefile` (new).
  `make` (host, zero warnings) → `./kbase-stress --print-abi`;
  `make cross` → static aarch64 (~598 KiB, zero warnings).
- On-device (short lock hold, as root): `timeout 90 ./kbase-stress
  --jobs 200 --churn 300 --fence-ms 1500 --softwait-ms 8000`.
  Individual suites: `./kbase-stress many|bo|fence|softwait|fault`.
- Kernel references (read-only, shared tree untouched): `mali_kbase_jd.c`
  (submit/dependency/fail paths), `mali_kbase_softjobs.c` (fence/event
  waits, 3 s cancel), `backend/gpu/mali_kbase_js_backend.c` (tick
  escalation), `mali_kbase_config_defaults.h` (300/350/400 ms + 3000 ms),
  `platform/exynos/gpu_integration_callbacks.c` (`KBASE_FENCE_TIMEOUT`
  1000 ms), `mali_base_kernel.h` (event codes incl. `0x4003 JOB_INVALID`).
