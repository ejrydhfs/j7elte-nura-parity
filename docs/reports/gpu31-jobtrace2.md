# gpu31 jobtrace2 — activate the staged completion hooks (F-JT1 + F-JT2)

Date: 2026-11-20. Owner: G31_JobTrace2. Ticket: driver 0001 submit.c
run_job + free_job, gem.h/c job_done_sync.

## 1. Problem (int10 §3 caveat)

`gpu31-integration10.md §3` proves the jobtrace fixes are staged but
inactive in the applied driver snapshot:

- `p7580_gem_job_done_sync()` exists (`T` symbol) but has **no caller**
  (the free_job bo_list walk from `gpu31-jobtrace.md §3.4` never made
  it into the patch).
- `run_job` has **no out_sync install** (F-JT1 fix not applied there).

So int10 would still show zero readback / vacuous fence.

## 2. What this ticket changes

Three files (canon edits + 0001 regen, same order, hunk counts updated):

| File | Change |
|---|---|
| `code/gpu31/jobs/panfrost7580_job.h` | `void **bos` on `struct p7580_submit_job`: owned BO object pointers for the completion retire path (opaque `void*` so the header stays drm-free). |
| `code/gpu31/jobs/panfrost7580_job.c` | `kfree(job->bos)` in `p7580_job_free` (array only; refs are the driver's). |
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | regen (+70/−8 net on submit.c: `@@ -0,0 +1,718 @@` → `+1,788`; job.c `+1,785` → `+1,786`; job.h `+1,148` → `+1,155`). All 19 blocks still `git apply --check` clean, file order unchanged. |

Driver logic (all inside `panfrost7580_submit.c` in the patch):

- **F-JT1 (run_job install + SUBMIT stash):** SUBMIT validates the
  out_sync handle now (`drm_syncobj_find`, `-ENOENT` preserved) and
  stashes it on `job->out_sync`; the async `run_job` installs the
  returned fence via `drm_syncobj_replace_fence` (own ref) after
  `pm_put`, before returning it to the shim. ERR_PTR run_job failures
  skip the install (no fence to install); a handle closed between
  SUBMIT and dispatch finds no syncobj and skips (job still runs).
- **F-JT2 (free_job walk):** SUBMIT transfers one lookup ref per BO
  onto `job->bos`; `free_job` (finish_work, strictly after the fence
  signal per the 0004 shim) walks `job->bos` and calls
  `p7580_gem_job_done_sync(obj)` per BO (flush_dcache_page per
  HIGHMEM SHMEM page; CMA/dma-direct no-op inside), then drops the
  job's refs. No MMIO, never sleeps, no locks — safe beside the PM
  release. Unpublished-job error paths unwind through `err_job_bos`
  (frees the pointer array; refs still owned by `objs[]`).

Design note: the first draft recovered `struct drm_file *` in
free_job via `container_of(df, struct drm_file, driver_priv)` — wrong
(`driver_priv` is `void*`, so the type math fails, and `drv->gd.dev`
is `struct device*`, not `struct drm_device*`). The `job->bos` design
above avoids both: no lookup, no dev/file recovery, stable across
close (postclose clears `driver_priv` only after entity fini retires
every queued job, but we no longer depend on that).

## 3. Grep evidence (patch text contains both call sites)

Against `code/gpu31/driver/0001-panfrost7580-driver-sources.patch`:

- F-JT1 run_job install (line 5840):
  `drm_syncobj_replace_fence(s, fence);` — guarded by
  `if (!IS_ERR(fence) && fence && job->out_sync)` (line 5835) with
  `s = drm_syncobj_find(&df->mgr, job->out_sync)` (line 5837).
  Stash site (line 6293): `job->out_sync = args->out_sync;`.
- F-JT2 free_job walk (line 5913):
  `p7580_gem_job_done_sync(obj);` — inside
  `if (job && job->bo_count && job->bos)` (line 5906), pointer from
  `job->bos[i]` (line 5909), ref dropped after (line 5914:
  `drm_gem_object_unreference_unlocked(obj);`).
- Definition still present (line 215):
  `void p7580_gem_job_done_sync(struct drm_gem_object *obj)` plus
  decl (line 1327) and `kfree(job->bos)` (line 3188).

Caller proof: `grep -c "p7580_gem_job_done_sync\|drm_syncobj_replace_fence"`
on the patch = 12 (was definition-only for the former; the run_job
`replace_fence` at 5840 and the free_job `job_done_sync` at 5913 are
the two new call sites — the other 10 hits are pre-existing syncobj
helpers, decl, and comments).

## 4. Verification (exercised)

- Host: `jobs/job_test` PASS, `gem/gem_validate` PASS,
  `driver/submit_fix_test` PASS (unchanged scope).
- Kernel syntax (`aarch64-linux-gnu-gcc -fsyntax-only -Wall -Werror`
  vs real 3.10.108 headers + staged 0001–0004 shims, same flags as
  the slice Makefiles): `panfrost7580_submit.c` RC=0,
  `panfrost7580_job.c` RC=0, `panfrost7580_gem.c` RC=0.
- Patch hygiene: regenerated 0001 `git apply --check` clean (all 19
  blocks) + `patch -p1 --dry-run` clean (int9-class truncation guard);
  hunk counts byte-audited (submit.c `+1,788` = 788 `+` body lines).
- NOT verified (same gate as jobtrace/int10): Image build, flash,
  `/dev/dri` submit-test, egltest readback. No reboot/flash attempted.

## 5. Integrator notes

- Apply order unchanged (backport 0001–0005 → KMS Mode-B → dmabuf 0001
  → driver 0001 (this regen) + 0002 → fragments d0/d4).
- `struct p7580_submit_job` grows by one pointer (`bos`); `kzalloc`
  sites are unaffected (zero-init = NULL, free handles NULL).
- Post-flash rerun: `gpu31-jobtrace.md §6` verbatim, with the §3
  caveat now lifted — out_sync waits on the real fence, completion
  invalidate is hooked. Expect §4 row 1 (dispatch + IRQ DONE +
  `sig_rc=0`, non-zero readback `RENDER_OK`).

## 6. Files changed (this ticket)

- `code/gpu31/jobs/panfrost7580_job.h` (+7: `bos` field + comment)
- `code/gpu31/jobs/panfrost7580_job.c` (+1: `kfree(job->bos)`)
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` (regen,
  +70/−8 net on submit.c + job.h/job.c hunk-count bumps)
- `docs/reports/gpu31-jobtrace2.md` (this file)
