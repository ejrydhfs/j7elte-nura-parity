# gpu31-syncobjwait — syncobj WAIT blocks on a pending job fence

Date: 2026-11-21 (UTC). Slice: G31_SyncobjWait. Phone on int30
(armfence live); no flash per ticket. External disk only.

Related: `gpu31-syncobj.md` (ioctl surface), `gpu31-abialign.md`
(32 B WAIT / SYNC_FILE tolerance), `gpu31-armfence.md` (arm-time
fence), `gpu31-jobtrace.md` §1 F-JT1 (NULL-install leg),
`gpu31-mesa-render.md` §1 (elapsed=0 rows).

## 0. Verdict

A wait on a pending job's out-fence blocks until the fence signals
(or the deadline) on the int30 kernel. The ticket's instant-0 symptom
— `SYNCOBJ-WAIT(+8s abs): rc=0 elapsed=0 ms` for a job that only
completes on the 5 s timeout — was the pre-armfence empty-syncobj leg
(F-JT1): `sched_job->fence` was NULL at SUBMIT, the install skipped,
and the syncobj stayed in its already-signaled empty state. That leg
is gone on int30: the 0-timeout probe now returns `-ETIME` (live
pending fence at SUBMIT return) and the 8 s wait retires on the real
fence. This slice proves the full WAIT path correct, fixes two
residual defects in it (WAIT_ALL zero-budget hole, const-ops
`.rodata` write), and adds the host blocking test (sim-clock) plus
the device recheck lines.

## 1. BEFORE (pre-armfence kernel; ticket statement + F-JT1)

- `SYNCOBJ-WAIT(+8s abs): rc=0 elapsed=0 ms` for a job that only
  completes on the 5 s timeout. A pending fence cannot return 0 in
  0 ms; the fence waited on was therefore empty (NULL == signaled)
  or already signaled.
- Mechanism (`gpu31-jobtrace.md` F-JT1): the 0004 shim assigned
  `sched_job->fence` from `run_job`'s return on the async worker, so
  at SUBMIT time the field was still zero and
  `if (fence) replace_fence(...)` never ran. Mesa then recycled the
  batch BO while the GPU still read it — the PTEWALK MISMATCH
  (`w0=0x52`, tables MATCH, bytes wrong).
- Same signature in `gpu31-mesa-render.md` §1.1:
  `SYNCOBJ-WAIT(+8s abs): rc=0 (OK) elapsed=0 ms` on every pattern
  run (there: fast retire, but indistinguishable from the empty leg
  without the probe).

## 2. AFTER (int30, arm-time install live; measured today)

Two binaries, same node, same result:

```
-- /data/pf7580-submit (prebuilt 00:29) --
FENCE-PROBE(0-timeout): rc=-62 (Timer expired) elapsed=0 ms
SYNCOBJ-WAIT(+8s abs): rc=0 (OK) elapsed=0 ms
-- /tmp/pf7580-syncwait (this slice, fresh cross-build) --
FENCE-PROBE(0-timeout): rc=-62 (Timer expired) elapsed=0 ms
SYNCOBJ-WAIT(+8s abs): rc=0 (OK) elapsed=0 ms
WAIT-RECHECK(0-timeout, post-wait): rc=0 (OK)
```

Reading: the probe `-ETIME` proves the out-sync held a **live
pending fence at SUBMIT return** (arm-time install working — an
empty syncobj would probe 0). The NULL job retires sub-ms via IRQ,
so the 8 s wait returning 0 in 0 ms is a genuine fast retire, and
the new WAIT-RECHECK confirms the fence is signaled after (not
empty). Dark/stuck-slot expectation, unchanged: the 5 s
`timedout_job → recover` path signals the same fence, so a stuck
job's 8 s wait returns `rc=0 elapsed≈5000 ms`; a truly dead GPU
returns `-ETIME` after the full 8 s. (No faulting job was submitted
to prove the dark leg from this slice — Main can read it off any
faulting run's elapsed line.)

## 3. WAIT path audit (all checked, two fixed)

- Absolute→relative: `ktime_get_ts` delta → `nsecs_to_jiffies`,
  past stamp = 0 (non-blocking), huge stamp capped at `LONG_MAX`.
  Same shape as WAIT_BO. Correct; HZ=100 → 8 s = 800 jiffies.
- `dma_fence_wait_timeout` (0001): signaled-at-entry returns the
  budget; else `default_wait` blocks on the completion; expiry with
  budget → `-ETIME`, with 0 budget → 0. Correct mainline shape.
- Dispatcher: `_IOC_NR`-keyed, copy-in truncated to our 32 B,
  copy-back capped at 32 B — Mesa/libdrm's 40 B WAIT (deadline tail)
  shares offsets 0–28, so timeout/handles/flags survive. Correct.
- 32 B `p7580_syncobj_wait` vs Mesa: `handles` u64@0, `timeout_nsec`
  s64@8, `count` u32@16, `flags` u32@20, `first_signaled` u32@24,
  `pad` u32@28 — matches 20.x `drm.h` (no `deadline_nsec` tail).
  `BUILD_BUG_ON` pins hold (`-fsyntax-only` + real `-c` PASS).
- Empty-syncobj semantics: NULL fence counts as signaled in WAIT
  (0003 contract, mainline parity); HANDLE_TO_FD on NULL → `-EINVAL`;
  RESET drops to NULL. Consistent; only SUBMIT installs the fence.
- FIX 1 (functional): WAIT_ALL with `timeout == 0` on a pending
  fence returned **0 (success)** — `default_wait` maps helper-0 to
  0 when no budget was given, and the loop never re-tested. Now
  re-tests `dma_fence_is_signaled` and returns `-ETIME` unless the
  fence is genuinely signaled. ANY already probed correctly
  (`timeout == 0` → `-ETIME` up front); its `r >= 0 || !r` dead
  clause is cleaned to `r >= 0`.
- FIX 2 (robustness): CREATE_SIGNALED/SIGNAL built the pre-signaled
  fence from a `static const` ops table with `.release = NULL` and
  patched the hook through a cast on first use — a `.rodata` write
  (plus `dma_fence_init` BUGs on NULL release). The table is now a
  plain static with `.release` assigned at definition; the
  first-use mutation is deleted.

## 4. Host test: wait blocks on unsignaled (sim-clock)

The mock backend used to auto-signal every waited syncobj (instant
0, untestable). It now carries a virtual CLOCK_MONOTONIC
(`mock_now_ns`, `pf7580_mock_now/advance/signal_at` in `pf7580.h`):
WAIT advances the clock to the first fire inside the absolute
deadline (rc 0) or to the deadline (rc `-ETIME`) — no sleeping, fully
deterministic. New `t_syncobj_wait_block` covers: ANY/ALL pending
` t=0` probes → `-ETIME` consuming no time; ANY fire at +5 s inside
+8 s → 0 with the clock at +5 s; fired fence stays signaled;
deadline-before-fire → `-ETIME` at the deadline; ALL consumes the
latest in-budget fire; ALL with one fire past the deadline →
`-ETIME`; unknown handle → `-ENOENT`.

```
native:        MOCK SUITE: PASS (incl. t_syncobj_wait_block)
qemu-aarch64:  MOCK SUITE: PASS
sched 280/280, jobs job_test, gem gem_validate: PASS
syncobj.c: -fsyntax-only PASS + real -c object 18928 B, zero warnings
```

## 5. Device test delta (this slice, in `--submit-test`)

- Prints `WAIT-RECHECK(0-timeout, post-wait)` after the 8 s wait so
  the retire-vs-empty call is on the record (probe `-ETIME` → wait
  0 → recheck 0 = blocked-then-signaled; probe 0 = empty/fast path).
- No new failure modes: informational prints only, exit codes
  unchanged (verified PASS above on int30).

## 6. Files changed

- `code/gpu31/gem/panfrost7580_syncobj.c` — FIX 1 (WAIT_ALL
  re-test), ANY cleanup, FIX 2 (writable signaled-ops, release at
  definition, first-use mutation deleted).
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` —
  regen of the syncobj.c hunk only (`@@ -0,0 +1,859 @@`; all other
  hunks byte-identical).
- `code/gpu31/client/pf7580.{c,h}` — mock sim-clock
  (`mock_now_ns`, `signal_at_ns`, `now/advance/signal_at`;
  FTOH carries the fire time; UNKNOWN handle still `-ENOENT`).
- `code/gpu31/client/pf7580-selftest.c` — `t_syncobj_wait_block`
  (mock) + `WAIT-RECHECK` (`--submit-test` device path).
- this report.

Checks: `client make check` PASS native + qemu-aarch64;
`sched make check` 280/280 native + qemu; `jobs` + `gem` checks
PASS; syncobj `-fsyntax-only` + real `-c` PASS; regen'd 0001
`git apply --check --unsafe-paths` clean. Full-series Image build
is the integrator's (int31 script).

## 7. Handoff

Integrator (G31_Integrator30): rebuild with this 0001 (syncobj
hunk +1,859), rerun `/tmp/pf7580-syncwait --submit-test` post-flash
(expect §2 AFTER rows), and run any faulting batch to confirm the
dark leg (`SYNCOBJ-WAIT` rc=0 elapsed≈5000 ms). No flash from this
ticket. Sibling scope note: `code/gpu31/jobs/` dirt in the tree is
G31_SubmitSnapshot's live work (parked/popped around this commit,
never touched here) — this commit is path-scoped to the §6 list.
