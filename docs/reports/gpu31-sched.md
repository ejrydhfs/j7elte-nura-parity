# GPU31 sched — drm-backport review fixes (0001/0002/0004 NEEDS FIXES → fixed, 0003 CONDITIONAL PASS → hardened)

Date: 2026-11-19 (UTC). Slice: `sched` (G31_Sched3).
Device state per Main 2026-11-19: **phone IS UP**, kernel `3.10.108 #37`
(SMP PREEMPT, gcc 16.2.0), `/dev/mali0` present, `/dev/dri` absent.
No reboot, no flash, no kernel build performed in this slice
(Integrator builds; M1Verify flashes after Main's explicit go).
The shared kernel tree was never modified — read-only `-fsyntax-only`
+ `-c` checks against a private copy of the 3.10.108 headers.
Stock `dt.img` untouched. No device contact in this slice.

## Verdict

All ten `gpu31-review.md` findings against the backport series are
**fixed in this revision** (0001 F1/F2/F3, 0002 F1/F2, 0004 F1–F5 +
push_job minor, 0003 F1): updated series `patches/drm-backport/0001-0004`
(+ `code/gpu31/series/` mirror, byte-identical) + 182-check host test
(native + qemu-aarch64 PASS) + kernel `-fsyntax-only` AND real `-c`
object compiles PASS (4 files, zero warnings) + 16 structural gates
PASS. 0005 was already PASS and is unchanged (re-verified apply).

Prior art: `gpu-panfrost.md` (backport audit), `gpu-drm.md` (M1–M3),
`gpu31-target.md` (v5.2 UAPI lock), `gpu31-dt.md` (zero-DT-change rule),
`code/gpu31/jobs/` (SUBMIT/slot backend that consumes the 0004 shim),
`code/gpu31/gem/` (BO resv embedded on the 0002 shim layout),
`docs/reports/gpu31-review.md` §§1–4 (the findings answered in §5).

Delivered under `code/gpu31/sched/` (slice owns all of it; only the
Integrator applies/builds):

| File | Lines | What it is |
|---|---|---|
| `src/dma-fence.{h,c}` | 173 + 229 | FIXED 0001 bodies: `dma_fence_init()` (F1); no `lock`/`status`/`default_work`/`event` members (F2); `default_wait` returns remaining budget (F3) |
| `src/dma-resv.{h,c}` | 101 + 300 | FIXED 0002 bodies: `resv->lock` taken in every helper (F1); wait paths pin fences under the lock (F2); `shared[]` C99 flex (minor) |
| `src/drm_syncobj.{h,c}` | 79 + 165 | FIXED 0003 bodies: `fini` drains via `idr_get_next` (F1); `copy_fences` address-order note in header |
| `src/drm_sched.{h,c}` | 143 + 398 | FIXED 0004 bodies: `sched_link` vs `queue_list` split (F1); dispatch enforces deps (F2); `dep_cb` + one-in-fence `-EOPNOTSUPP` (F3); ERR_PTR frees job (F4); `entity->timeout` (F5); `push_job` defined (minor); `ready` honoured |
| `emu/include/linux/` | 19 files | Single-threaded kernel emulation for the host test (`kernel-emu.h` + `list.h` + 15 redirect headers + staged shim copies at canonical paths) |
| `tests/sched-host-test.c` | 591 | 182-check host verification (see §4) |
| `patches/0001-0005*` | — | Slice-local copies of the delivered series (authoritative: `patches/drm-backport/`) |
| `Makefile` | 102 | `make check`: host test (native + qemu) + 16 gates + kernel `kcheck` |

## 1. What the shims are (unchanged scope)

Minimal `dma-fence → dma-resv → drm_syncobj → drm_sched` foundation for
the panfrost M3 driver (new files only, no DT, no `drm/` changes).
0005 (build wiring) is untouched. Scope stays a correctness subset:
no fence-array/chain, no ww_mutex, no timeline payloads, no priorities/
load-balancing/GPU-reset — same omissions the series README documents.

## 2. Kernel evidence (read-only, private tree copy)

- `git apply --check` PASS for all 5 patches individually and as a full
  series (verified in a private clone seeded with pristine
  `drivers/base/Makefile` + `drivers/gpu/drm/Makefile`).
- `-fsyntax-only` AND real `-c` object compiles PASS for all 4 `.c`
  against the actual 3.10.108 headers (`-std=gnu89 -Wall`, `__KERNEL__`,
  kconfig include), zero warnings. Objects: `dma_fence.o` 10280 B,
  `dma_resv.o` 13760 B, `drm_syncobj.o` 10704 B, `drm_sched.o` 14472 B.
- Tree primitives re-verified: `idr_get_next` exported (`lib/idr.c:805`),
  `idr_for_each_entry` is a `idr_get_next` macro (`idr.h:132-133`) with no
  removal-safety; `wait_for_completion_timeout` returns `unsigned long`
  remaining (`completion.h:83`); `alloc_ordered_workqueue` present
  (`workqueue.h:446`); `mutex_lock_interruptible` present.
- NOT verified (integrator/M1Verify own): full Image build, link into
  `built-in.o`, boot on device, any panfrost GEM/job-slot code (these
  patches are foundation only).

## 3. Host tests (`code/gpu31/sched/`, `make check`)

182 checks, native PASS + aarch64/qemu PASS. Each finding has a failing
test without the fix (verified by running the old bodies where noted):

- **T-init (0001 F1):** `dma_fence_init` fills ops/context/seqno/flags,
  empty `cb_list`, refcount 1; NULL-release contract asserted.
- **T-remaining (0001 F3):** signaled fence `wait_timeout(100)` returns
  100; unsignaled `wait(0)` returns 0; unsignaled `wait(50)` returns
  `-ETIME` (old code conflated the last two paths by discarding the
  helper's return).
- **T-chain (0001 F3 × 0002):** resv with 2 shared fences: unsignaled
  `wait_timeout(10)` → `-ETIME`; signaled both → returns 10 (chained
  remaining budget threads through instead of resetting).
- **T-pin (0002 F1/F2):** excl install + `get_excl` pins (refcount 3);
  wait on unsignaled → `-ETIME` with fence alive; swap excl while a
  waiter holds the old pin → old pointer stays valid; empty resv wait
  returns budget; `copy_fences` self-copy no-op, real copy pins refs.
- **T-drain (0003 F1):** 300 handles created, every third destroyed,
  `fini` drops the rest with no skips; manager reusable after drain
  (old `idr_for_each_entry` + `idr_remove` could skip entries).
- **T-lists (0004 F1):** `sched->entities` non-empty after `entity_init`
  (2 entities counted via `sched_link`), `queue_list` empty until submit;
  submit does not touch `sched->entities`; `fini` unlinks; `fini` sets
  `ready=false`, `wq=NULL`.
- **T-gate (0004 F2/F3):** unsignaled dep blocks `run_job` (count 0, job
  stays queued); `add_dependency` second distinct fence → `-EOPNOTSUPP`;
  same fence again → 0; signal dep → pump re-queued → `run_job` runs,
  finish path retires the job.
- **T-err (0004 F4):** `run_job` ERR_PTR frees the job (`free_job`
  count 1) and the FIFO advances to the next job.
- **T-timeout (0004 F5 + minor + ready):** `entity->timeout` copies
  `sched->timeout` (42); `push_job` queues + counts + kicks the pump;
  zero timeout arms nothing; nonzero arms `delayed_work` with
  `delay == entity->timeout`; `ready=false` stops dispatch.
- **16 structural gates** (Makefile `gates`): vestigial members absent
  (`spinlock_t *lock`, `struct work_struct default_work`,
  `int status`, `wait_queue_head_t *event`, `shared[0]`),
  `msecs_to_jiffies(5000)` absent, `idr_for_each_entry` call absent,
  entity conflation line absent; `dma_fence_init`, `sched_link`,
  `dep_cb`, `dep_fence`, `EOPNOTSUPP`, `push_job`, `mutex_lock`,
  `idr_get_next`, `dma_fence_get` present.

`make check` also runs `kcheck` (§2) and the gates before the binaries.

## 4. Downstream consumer impact (checked, not edited)

- `code/gpu31/jobs/panfrost7580_job.c` hand-inits fences with a comment
  citing the old missing-`init` (lines ~170-174) — it should switch to
  `dma_fence_init()` at integration (its field list already matches the
  fixed struct). It resolves in-fences through callbacks, never touching
  `queue_list`/`finish_cb`, so F1/F3 renames are source-compatible.
- `code/gpu31/gem/` embeds `struct dma_resv` by value against a layout
  check (`lock`/`fence_excl`/`fence_shared` offsets) — member order is
  unchanged, so the `BUILD_BUG_ON`s still hold. `shared[0]` → `shared[]`
  is ABI-identical (offset of `shared` unchanged).
- No in-tree caller references `fence->lock`/`status`/`default_work`/
  `event` (grep over `code/gpu31/`); the deletions are safe.

## 5. Review response (gpu31-review.md §§1–4 → fixed)

- **0001 F1 (`dma_fence_init` missing) — FIXED.** New `dma_fence_init()`
  (`dma-fence.c:47-58`) does `kref_init` + `init_completion` +
  `INIT_LIST_HEAD(cb_list)` + `spin_lock_init(cb_lock)` + flags/ops/
  context/seqno, with `BUG_ON(!fence || !ops || !ops->release)`.
- **0001 F2 (`fence->lock` dangles; `status`/`default_work` dead) —
  FIXED by deletion.** Struct is now refcount/ops/context/seqno/flags/
  completion/timestamp/cb_list/cb_lock only. `release_base` is a pure
  leak check (`WARN_ON(!list_empty)`); the `event` kmalloc/free path is
  gone — the completion alone wakes waiters.
- **0001 F3 (`default_wait` returns `timeout`) — FIXED.**
  `dma_fence_default_wait` returns the `wait_for_completion_*` result
  (remaining jiffies), mapping `<= 0` to `-ETIME`/`0` exactly as before.
- **0002 F1 (locking documentary) — FIXED.** `reserve_shared`,
  `add_shared_fence`, `add_excl_fence`, `get_excl`, `test_signaled`,
  `wait_timeout`, `copy_fences` all take `resv->lock`. Readers snapshot
  refs under the lock, then wait/signal-test after unlock. `copy_fences`
  takes both locks in address order (documented in the header).
- **0002 F2 (wait on unpinned fences) — FIXED.** `wait_timeout` pins
  excl + up to 8 shared on-stack (heap overflow, `-ENOMEM` on OOM under
  lock with pins dropped) before sleeping; `test_signaled` pins the same
  way before testing. `add_excl` puts the replaced pointer only after
  installing the new one, and only after unlock.
- **0002 minor (`shared[0]`) — FIXED.** Now `shared[]`.
- **0003 F1 (`fini` mutates idr during iteration) — FIXED.** `fini`
  loops `idr_get_next(&idr, &id=0)` + `idr_remove` + `put` until empty
  (safe under `mgr->lock`), then `idr_destroy`.
- **0004 F1 (entity conflation) — FIXED.** `sched_link` (membership in
  `sched->entities`) vs `queue_list` (pending-job FIFO) are separate
  members; the `list_del_init` right after `list_add_tail` is deleted.
  `entity_init` records `sched` + copies `timeout`; `fini` unlinks from
  `sched->entities` under `sched->lock` before flushing. `fini` also
  drains `retire` with `remove_callback` + `put` on each job's dep.
- **0004 F2 (deps unenforced) — FIXED.** The pump peeks the head job and
  leaves it queued while `dep_fence` is unsignaled; the dep callback
  re-queues the pump on signal. Verified by T-gate.
- **0004 F3 (`finish_cb` dual role) — FIXED.** `dep_cb` (dependency
  role) and `finish_cb` (completion role) are separate cbs; contract is
  one in-fence per job — a second distinct fence fails `-EOPNOTSUPP`
  (matches the one-in-fence Mesa contract, review §12 note). Same-fence
  re-add is a no-op 0; already-signaled dep drops the ref and returns 0.
- **0004 F4 (ERR_PTR leak) — FIXED.** The pump detaches both cbs, puts
  the dep, calls `free_job`/`kfree`, then re-queues the pump for the
  next job. Also handles a stray `-EBUSY` from the completion callback
  by retiring instead of wedging the FIFO.
- **0004 F5 (hardcoded 5 s) — FIXED.** `entity->timeout` (jiffies,
  copied from `sched->timeout` at `entity_init`) drives
  `queue_delayed_work`; `timeout == 0` arms nothing.
- **0004 minor (`push_job` declared, never defined) — FIXED.**
  `drm_sched_entity_push_job()` implemented; `drm_sched_job_submit()`
  delegates to it. `sched->ready` is now tested in the pump (fini
  clears it under `sched->lock` and evicts entities).

## 6. Integrator commands (NOT run — owner: G31_Integrator)

```sh
# apply order (private tree copy first):
for p in patches/drm-backport/000[1-5]-*.patch; do
  git apply --check -v "$p" || break
done
git apply patches/drm-backport/000[1-5]-*.patch
# host verification (this slice):
make -C code/gpu31/sched check   # 182 native + 182 qemu + 16 gates + kcheck
# on device (after flash gate, Main's go only): /dev/dri render node
# present, SUBMIT no-op batch retires, BO_WAIT wakes — expect ordered
# seqno retirement per fence context, no UAF under concurrent SUBMIT/WAIT.
```
