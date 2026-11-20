# gpu31-opsfix — `job->ops` reads NULL in the pump despite a correct store

Date: 2026-11-20. Owner: G31_OpsFix. Phone up on int13; no flash by this ticket — Main flashes.

## 1. Silicon symptom (int13, one NULL-job)

```
p7580_sched: pump job_count=1
p7580_sched: ops ops=0000000000000000 run=0000000000000000 slot=1
p7580_sched: retire seq=0 slot=1 sslot=1
```

`job->ops` reads **NULL** inside the same TU's pump, so `run_job` is never
called and the job retires through the NULL-ops F4 path. Slot plumbing
already works (`sslot=1`). Submit passes `&p7580_drv_sched_ops` and
`drm_sched_job_alloc()` stores it (`job->ops = ops`, drm_sched.c:286).

## 2. Analysis — which hypothesis the evidence supports

Three candidates were put forward:

- (a) the probe prints a different pointer/type than `struct drm_sched_job`
  (cast/offset mistake in the pump or probe);
- (b) submit's call resolved to a different `drm_sched_job_alloc` (link
  picked a duplicate symbol);
- (c) two compiled copies of the shim with different struct layouts.

Evidence:

- **(b) is ruled out.** `install` links exactly one `drm_sched_job_alloc`:
  surviving scratch `System.map` files show a single `T drm_sched_job_alloc`
  (the int9 tree additionally shows the single `t p7580_drv_run_job` and
  the single `r p7580_drv_sched_ops`). The int13 build log links one
  `drivers/gpu/drm/drm_sched.o` and one
  `drivers/gpu/drm/panfrost/panfrost7580_submit.o`.
- **(a) is ruled out for the pump side.** The pump dequeues with
  `list_first_entry(..., struct drm_sched_job, node)` and prints
  `job->ops` — same type, same TU that stored it. The upgraded ST-OPS
  probe now additionally prints `job` and `&job->ops`, so any future
  cast/offset mistake is visible in one line.
- **(c) is ruled out as a *source* skew but remains the live suspect as a
  *build* skew.** There is exactly one struct definition in the repo
  (`code/gpu31/sched/src/drm_sched.h`, the single authority all TUs
  include: shim via `"drm_sched.h"`, driver via `ccflags
  -Idrivers/gpu/drm` → `<drm_sched.h>`); no `include/drm/drm_sched.h`
  shadow exists in the pristine tree, and `grep drm_sched_job_alloc`
  over the pristine tree finds no in-tree duplicate. What the evidence
  cannot exclude from the host is a **stale `.o` / duplicate shim copy
  winning the Image link on the build machine**: `job->ops` is stored
  correctly, read correctly, at the same offset — yet NULL at runtime.
  That triple is the signature of the pump reading memory written by a
  different alloc (stale `drm_sched.o`) or the submit call binding a
  different copy than the pump's.

The fix therefore does not guess which object was stale: it makes every
remaining possibility **fail at build time or at submit time** instead of
retiring silently.

## 3. Offset evidence (ground truth, measured — not inferred)

True in-tree layout (arm64 3.10 headers, `aarch64-linux-gnu-gcc -S`,
offsetof getters compiled against the real tree):

| field | offset |
|---|---|
| node | 0 |
| entity | 16 |
| **ops** | **24** |
| fence | 32 |
| dep_fence | 40 |
| dep_cb | 48 |
| finish_cb | 72 |
| timeout_work | 96 (`delayed_work` = 104: `work_struct` 32 + timer + wq + cpu) |
| finish_work | 200 (`work_struct` = 32: `atomic_long` + list + func, no lockdep) |
| slot | 232 |
| ran | 236 |
| owner | 240 |
| sizeof | 248 (`dma_fence` 96, `dma_fence_cb` 24) |

Host emu layout differs (emu `work_struct` is 16 bytes, no lockdep/timer
bloat): node=0 entity=16 **ops=24** fence=32 slot=144 ran=148 owner=152
sizeof=160. The one field that matters — **ops=24 — is identical in both
worlds**, so the cross-TU contract (`ops` offset) holds everywhere while
each world pins its own full layout.

Method: neutralised the new asserts in a scratch copy, compiled
`return (int)offsetof(...)` getters with the kbuild include chain
(`-nostdinc`, arm64 includes, `-D__KERNEL__ -include linux/kconfig.h`,
`-DKBUILD_MODNAME`), read the `mov w0, N` immediates from `.s`. Host
offsets from the compiled `offprobe` (`cc -Iemu/include -Isrc`).

## 4. Fix

Shim (`code/gpu31/sched/src/drm_sched.{h,c}` → regen 0004 + 2 mirrors):

- `drm_sched.h` is declared the **single authoritative definition**; both
  TUs include it through the same path. New compile-time pins, per-TU
  correct (`__KERNEL__` selects 232/236, emu selects 144/148; ops=24
  everywhere):
  `DRM_SCHED_ASSERT_OPS_IS_24`, `DRM_SCHED_ASSERT_SLOT_IS_232`,
  `DRM_SCHED_ASSERT_RAN_IS_236` (`BUILD_BUG_ON_ZERO` enum constants;
  portable fallback macro when emu has no `<linux/bug.h>`).
- `drm_sched_job_alloc`: after `job->ops = ops`, `BUILD_BUG_ON(offset !=
  24)` ( TU layout) + `BUG_ON(job->ops != ops)` (store round-trip). A
  drifted TU or a stale `.o` now fails loudly instead of queueing a
  NULL-ops job.
- ST-OPS probe upgraded: `ops job=%p opsoff=%p ops=%p run=%p slot=%d` —
  prints the dequeued job address and `&job->ops` alongside the values,
  so dmesg shows whether the pump read the word the alloc wrote.
  Compare `ops` against the driver's `&p7580_drv_sched_ops` from
  `/proc/kallsyms`; `opsoff - job` must equal 24.

Driver (`code/gpu31/driver/0001-...-sources.patch`, submit.c only):

- `#include <linux/bug.h>`; a `p7580_drv_sched_layout_pin()` noinline
  probe carrying the same three `BUILD_BUG_ON`s in the submit TU, so a
  drifted driver-side header breaks the build.
- Call-site guard after `drm_sched_job_alloc`: `BUILD_BUG_ON(ops != 24)`
  plus a runtime check — `!entity || sched_job->entity != entity ||
  sched_job->ops != &p7580_drv_sched_ops || !run_job` → cleanup, free,
  `-EFAULT`, do not queue. A stale `.o` / duplicate shim copy that
  corrupts the store now fails the ioctl instead of retiring silently.

Host test (`tests/sched-host-test.c`, `test_opsfix_roundtrip`): allocates
a job via the shim API, asserts `job->ops == &ops_a`, asserts
`&job->ops - job == 24`, submits, pumps, asserts `run_job` fired exactly
once **through the same ops pointer** (`ops_seen_in_run == &ops_a`),
`ran == 1`, fence present, then signals + finishes and asserts
`free_count == 1`.

## 5. Verification (exercised)

- Host: `sched-host-test` native **263 checks, 0 failures**;
  aarch64/`qemu-aarch64` **263 checks, 0 failures** (was 252 before;
  +11 from the new round-trip test).
- `make gates` PASS; `make kcheck` PASS (4 files syntax-only + `-c`
  object compiles vs real 3.10.108 headers).
- Scratch-tree `git apply --check` clean for all 9 patches
  (backport 0001–0005 + KMS Mode-B + dmabuf + driver 0001 rebuilt here +
  driver 0002) against a pristine tree copy on the external disk.
- Applied tree: `drm_sched.c` + `panfrost7580_submit.c` **-fsyntax-only
  clean** AND **`-c` object-compile clean** vs real 3.10.108 headers
  (this caught a real bug mid-task: file-scope `BUILD_BUG_ON` is illegal
  — fixed by wrapping the driver asserts in a function).
- `submit_fix_test`, `job_test`, `gem_validate` scopes untouched (no
  changes outside submit.c/sched-shim).
- Host offprobe + kernel `.s` immediates agree on ops=24 (table above).
- Patch hygiene: single new-file hunk for submit.c grows `+1,866 →
  +1,897`; `+N` counts recount-after-edit (the earlier "corrupt patch"
  was a merged `++`/blank-line from a sloppy replace — rebuilt cleanly
  from HEAD bytes with one-`+`-per-line discipline); trailing bytes
  preserved (`--check` clean is the gate).
- Mirrors byte-identical; `code/gpu31/series/SHA256SUMS` 0004 line
  regenerated (`0b163f5a…`).
- NOT verified (Main/integrator own): Image build, flash, `/dev/dri`
  submit-test, egltest readback. No reboot/flash attempted. All build
  artefacts on the external disk; nothing written to `/` or `/tmp`
  (scratch trees under the external disk; `/tmp` holds only throwaway
  copies of committed patches for `git apply --check`).

## 6. Expected dmesg after flash (one NULL-job, slot 1)

```
p7580_sched: ops job=… opsoff=… ops=… run=… slot=1
```

Diagnosis rows for the new probe:

| Seen | Meaning |
|---|---|
| `opsoff - job != 0x18` (24) | TU reading a different struct than the alloc wrote; check which `drm_sched.h` the TU included |
| `ops != &p7580_drv_sched_ops` (kallsyms) | stale `drm_sched.o` or duplicate shim copy won the link; clean-rebuild the tree |
| `run != p7580_drv_run_job` (kallsyms `t`) | ops table corrupted or wrong backend; check submit call site |
| `ops=0` still, with the submit guard live but no `-EFAULT` ioctl error | the queued job never passed through this submit.c (stale Image or second submit path); confirm Image hash |

## 7. Integrator notes

- `patches/drm-backport/0004` is authoritative; the two
  `code/gpu31/{series,sched/patches}` copies are mirrors (byte-identical).
- Apply order unchanged (backport 0001–0005 → KMS Mode-B → dmabuf 0001 →
  driver 0001 (this regen) + 0002 → fragments d0/d4).
- `code/gpu31/driver/0001` regen: submit.c `+1,866 → +1,897` (1 include,
  11-line layout pin incl. wrapper, 15-line call-site guard, header
  recount); all other driver files byte-identical.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against §6; report which diagnosis row
  the node hits.
