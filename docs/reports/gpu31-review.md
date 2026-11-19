# GPU patch review — all `code/gpu31` patches (G31_Review)

Date: 2026-11-19. Tree references below are read-only checks against the
shared-tree scratch copy (`…/j7k/gpu31/scratch`, 3.10.108 + Samsung
`drivers/gpu/arm/t7xx/r15p0`); no tree file was modified. Device state per
Main 2026-11-19: **phone IS UP** (kernel #35, SSH/RNDIS ok); stale
"link down" claims in older slice reports are disregarded. No reboot, no
flash, no kernel build performed (Integrator / M1Verify own those).

Verdict scale: **PASS** · **CONDITIONAL PASS** (integrator-checkable at
apply/build time) · **NEEDS FIXES** (must change before merge).

---

## 1. `patches/drm-backport/0001-dma-fence-minimal-subset.patch` — CONDITIONAL PASS

New `include/linux/dma-fence.h` + `drivers/base/dma-fence.c`.

- **F1 [REQUIRED] No `dma_fence_init()` helper.** Every future fence backend
  (panfrost job fence, sched job fence) must hand-assemble
  `kref`/`completion`/`cb_list`/`cb_lock`/`flags`. One missed init =
  use-after-uninit. Add `dma_fence_init()` that does `kref_init`,
  `init_completion`, `INIT_LIST_HEAD(&cb_list)`, `spin_lock_init(&cb_lock)`,
  clears `flags`, and sets the vestigial `lock` member to NULL.
- **F2 [REQUIRED] `struct dma_fence.lock` dangles.** Header keeps mainline's
  `spinlock_t *lock` timeline-lock pointer but nothing ever assigns it.
  Any consumer dereferencing `fence->lock` oopses. Init to NULL and document
  "unused in subset", or delete the member (preferred — dead weight).
  Same for unused `default_work` / `status` (signal path never sets
  `status`; `timestamp` is set but never read — harmless, keep or drop).
- **F3 [REQUIRED] `dma_fence_default_wait` returns the wrong value.**
  `wait_for_completion_timeout` returns *remaining* jiffies; the shim
  discards it and returns the original `timeout`. Mainline returns remaining
  time. `dma_resv_wait_timeout` chains `timeout = ret`, so cascading waits
  get overlong budgets. Fix: `return` the `wait_for_completion_*` result.
- Minor: `dma_fence_put` casts `ops->release` blindly — add
  `BUG_ON(!fence->ops || !fence->ops->release)` in init. `context` is
  `unsigned` (32-bit) vs mainline u64 — fine, contexts never leave the
  kernel, no wire ABI. `lockdep_assert_held`, `ktime_get`, `complete_all`
  all exist in 3.10 — OK. Callback-under-`cb_lock` matches mainline — OK.

## 2. `patches/drm-backport/0002-dma-resv-shim.patch` — NEEDS FIXES

New `include/linux/dma-resv.h` + `drivers/base/dma-resv.c`.

- **F1 [REQUIRED] Locking is documentary only — real UAF.** The header says
  "fence replacement is done under `resv->lock`", but **no function in the
  .c takes `obj->lock`**: `reserve_shared` frees `old_list` while lockless
  readers (`test_signaled`, `wait_timeout`) may be iterating it;
  `add_shared`/`add_excl` swap pointers locklessly. Concurrent SUBMIT vs
  BO_WAIT ⇒ torn reads / double-put. Fix: take `mutex_lock(&obj->lock)` in
  every mutating helper *and* in `test_signaled`/`wait_timeout`
  (snapshot refs under lock, wait after unlock), or add
  `lockdep_assert_held` + enforce caller discipline. Former is safer.
- **F2 [REQUIRED] `dma_resv_wait_timeout` waits on unpinned fences.**
  It reads `obj->fence_excl`/`fence_shared[]` without a ref, then sleeps;
  `add_excl_fence` can `put` (free) a shared fence mid-wait. Fix with F1:
  `dma_fence_get` each fence under lock, wait, then `put`.
- Minor: `shared[0]` zero-length array — accepted by gcc, but prefer C99
  `shared[]`. `dma_fence_get(NULL)`/`put(NULL)` are NULL-safe, so NULL-fence
  paths are OK. `copy_fences` needs both locks held in address order —
  document at the declaration.

## 3. `patches/drm-backport/0003-drm_syncobj-subset.patch` — CONDITIONAL PASS

New `drivers/gpu/drm/drm_syncobj.{h,c}`.

- **F1 [REQUIRED] `drm_syncobj_manager_fini` mutates the idr while iterating
  it** (`idr_remove` inside `idr_for_each_entry`). 3.10's
  `idr_for_each_entry` is not documented safe against removal of the current
  entry — entries can be skipped (handle leak). Fix: loop
  `while ((syncobj = idr_find(...first...))) { idr_remove; put; }` or drain
  to a temp list first.
- Otherwise sound: `idr_preload(GFP_KERNEL)` + `mutex_lock` + `GFP_NOWAIT`
  alloc is the correct 3.10 idiom (`idr_alloc` verified present in
  `include/linux/idr.h:78`); `replace_fence` get-before-lock / put-old-after
  is correct; NULL-fence wait returns 0 (matches mainline already-signaled);
  NULL `syncobj` handled via NULL-safe get/put. No ioctl numbers are
  consumed here (driver-private range deferred) — the future panfrost ioctl
  author MUST allocate from `DRM_COMMAND_BASE` without colliding with exynos
  GEM ioctls; add that check to the M3 slice.

## 4. `patches/drm-backport/0004-drm_sched-panfrost-job-only-shim.patch` — NEEDS FIXES

New `drivers/gpu/drm/drm_sched.{h,c}`.

- **F1 [REQUIRED] Entity never joins the scheduler list.** `entity_init`
  does `list_add_tail(entity->queue_list → sched->entities)` and then
  immediately `list_del_init(&entity->queue_list)` + re-init. Net:
  `sched->entities` is always empty; `fini` tracking is dead. Worse, the
  *same* `list_head` is reused as the pending-job FIFO, so membership and
  queue are conflated. Fix: separate members (`list` for scheduler
  membership, `job_queue` for pending jobs); drop the delete.
- **F2 [REQUIRED] Dependencies are not enforced.** `pump` dequeues the head
  job and calls `run_job` unconditionally; `add_dependency` only parks a
  callback that re-queues the pump. A job whose dep is still pending still
  runs. Fix: record the dep fence on the job and have `pump` skip (requeue)
  jobs with unsignaled deps, or gate dispatch on dep readiness.
- **F3 [REQUIRED] One `finish_cb` serves two roles.** The dep waiter and the
  completion waiter are the same `dma_fence_cb`: a second in-fence hits
  `-EBUSY` inside `add_callback`, and dep-vs-completion lifetime overlap is
  fragile. Either support N deps (embedded list) or fail a second dep with a
  clear `-EOPNOTSUPP` and document "one in-fence per job" end to end (the
  Mesa backend already enforces one in-fence — keep the contract explicit).
- **F4 [REQUIRED] `run_job` ERR_PTR path leaks the job.** `pump` dequeues,
  sets `in_flight`, gets `ERR_PTR`, clears `in_flight`, requeues the pump —
  and drops the job pointer. Fix: `free_job`/`kfree` (or requeue) on that
  path.
- **F5 [REQUIRED] Timeout is hardcoded 5 s; `sched->timeout` unused.**
  `pump` uses `msecs_to_jiffies(5000)`; `struct drm_sched_entity` has no
  back-pointer to `sched`, so the tunable is unreachable. Fix: store
  `entity->sched` (or timeout) at `entity_init` and use it in `pump`.
- Minor: `drm_sched_entity_push_job` is declared in the header but never
  defined — implement or delete (else a future caller breaks the link).
  `sched->ready` is set/cleared but never tested in `pump` — wire it or
  drop it. `alloc_ordered_workqueue` verified present
  (`include/linux/workqueue.h:446`) — OK.

## 5. `patches/drm-backport/0005-build-wiring-for-shims.patch` — PASS

3 additive Makefile lines. `dma-fence`/`dma-resv` correctly gated on
`CONFIG_DMA_SHARED_BUFFER`; `syncobj`/`sched` ride `drm-y` (only built when
DRM is). Known context-fuzz vs the display patch's `drm/Makefile` hunk is
documented in the series README — rebase by regenerating, don't hand-edit.

## 6. `patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch` — CONDITIONAL PASS

3112-line DECON/DSIM driver (sampled review: Kconfig/Makefile, CMA helper
delta, GEM/IOVA hooks, modeset/vblank/IRQ, load/unload glue).

- `drm_plane_init(dev, plane, 1, …)` matches the in-tree signature
  (`drm_crtc.h:890`: `possible_crtcs unsigned long`) — `1` = CRTC 0. OK.
  Legacy `load`/`unload` + `drm_platform_init` is correct for 3.10 (no
  `drm_dev_alloc`). fops table is the standard 3.10 set. OK.
- CMA helper delta (`DEFINE_DMA_ATTRS` + `DMA_ATTR_WRITE_COMBINE` +
  `dma_alloc_attrs`) — all symbols verified present
  (`dma-attrs.h:16/34/50`, `arm64/dma-mapping.h:94/97`). Correct arm64
  adaptation. OK.
- `drm_vblank_init(drm, 1)` before IRQ request; `vblank_init` flag drives
  both error and unload cleanup. IRQ handler only touches INT status regs
  and calls `drm_handle_vblank`; `enable/disable_vblank` take no sleeping
  locks (commented). OK.
- **F1 [INTEGRATOR CHECK] `iovmm_*` prototypes.** `iova_get` builds
  `sg_alloc_table(1) + sg_set_buf + iovmm_map(dev, sgl, 0, size,
  DMA_TO_DEVICE, 0)` and load/unload call `iovmm_activate/deactivate`.
  Argument counts must be re-verified against
  `include/linux/exynos_iovmm.h` at build time; a mismatch is a build break
  (cheap to catch, impossible to review blind). Same for
  `devm_ioremap_resource` + `of_address_to_resource` usage — build will
  tell.
- **F2 [REQUIRED] Audit `clk_get` vs `clk_put`.** Clocks are acquired with
  bare `clk_get`; the sampled `unload` path shows no `clk_put`s. Every
  acquired clock needs a put on all error/unload paths or probe-retry leaks
  references. (Prefer `devm_clk_get` if available in this tree.)
- **F3 [INTEGRATOR CHECK] `platform_get_irq(pdev, 2)`.** The driver needs 3
  memory resources/IRQs for DECON; the DT patch adds no reg/interrupt data.
  Confirm the stock `decon_fb@14830000` node actually provides ≥3
  interrupts, else `load` fails with `-EINVAL` on hardware.
- **F4 [CHECK] Encoder/connector types are LVDS for a DSI panel.**
  Functionally just an id, but userspace will report LVDS. Use
  `DRM_MODE_CONNECTOR_DSI` / encoder `DRM_MODE_ENCODER_DSI` if present in
  this tree's `drm_mode.h`, else document the wart.
- No packed wire structs; register access via `writel` on LE arm64 — no
  endian concern. `upper_32_bits` guard matches the 32-bit DMA mask. OK.
  `pm_runtime` get held for card lifetime; confirm the sampled-but-unread
  `exynos7580_pm_ops` resume path re-inits clocks + panel.

## 7. `patches/drm/0002-arm64-dts-exynos7580-j7elte-drm-display.patch` — CONDITIONAL PASS

Adds `samsung,exynos7580-decon-drm` / `-dsim` compatibles to rev00/04/06.
Intent (steal both devices from vendor fbdev) matches the driver's match
table and the config fragment's vendor off-switches — but the hunks read as
pure additions, so the integrator MUST confirm each node ends up with
**exactly one** `compatible` (appended second string ⇒ two drivers match ⇒
probe-order race; replaced string ⇒ clean cutover). Inert under the stock
`dt.img` (never rebuilt per hard rules) until the M2 flash gate.

## 8. `patches/drm/config-drm-exynos7580.fragment` — CONDITIONAL PASS

`DRM/KMS_HELPER/KMS_CMA_HELPER/EXYNOS7580=y`, fbcon on, CMA 32 MB, vendor
fbdev symbols commented out. Pairing with patch 0001/0002 is coherent.
Integrator: confirm `ARCH_EXYNOS` is the real 3.10 arch symbol (else
`DRM_EXYNOS7580` is invisible after `olddefconfig`) and confirm 32 MB CMA
covers 720×1280 dumb buffers + fbdev before raising it.

## 9. `patches/drm-m1/*` — PASS (strategy note)

Node-only M1 (in-tree exynos + G2D, vendor fbdev untouched) is
self-consistent and honestly documented. **It is an ALTERNATIVE to the
§6–§8 display-swap path, not a complement**: do not apply both Kconfig
fragments. The §10 PRIME patch targets `exynos7580_drm.c`, i.e. it belongs
to the swap path (M2), not to this fragment.

## 10. `patches/iommu-dma/0001-…-prime-export-….patch` — NEEDS FIXES

Against `exynos7580_drm.c` (§6).

- Driver-hook names verified present in 3.10 `drmP.h:922-941`
  (`prime_handle_to_fd/fd_to_handle`, `gem_prime_export/import`,
  `get/import_sg_table`, `vmap/vunmap`) — build-level OK. `get_sg_table`
  construction mirrors the display IOVA path. OK.
- **F1 [REQUIRED] `import_sg_table` is import-by-copy, not sharing.**
  It allocates a fresh CMA buffer and ignores the table contents. For the
  stated purpose (kbase UMM import of a scanout buffer) the GPU then
  renders into a private copy the display never scans — silent
  zero-copy-scanout breakage. Either implement a real attachment-backed
  import or return `ERR_PTR(-ENODEV)` loudly; never fake sharing.
- **F2 [REQUIRED] `vmap` returns an unpinned `cma_obj->vaddr` with no-op
  `vunmap`.** If the GEM object is freed while an importer holds the vmap,
  UAF. Pin with `drm_gem_object_reference` (unref in `vunmap`) or document
  a caller-held-ref contract.
- Self-declared UNCHECKED; apply order is 0001 then this (context-line
  drift will show at `git apply --check`).

## 11. `patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h` — NEEDS FIXES

- Static size/offset asserts are the right pattern; asserted offsets
  (core_req@34, pre_dep@36, gles@48) recompute correctly from the struct.
  SOFT_DVFS exclusion is correct (type 5 = `SOFT_EVENT_WAIT` in r15p0 —
  verified in `mali_base_kernel.h:575`).
- **F1 [REQUIRED] `job_submit_translate` preserves the 48/56-byte stride,
  which the r15p0 kernel ALWAYS rejects.** `mali_kbase_jd.c:1347`
  (`gpu31/scratch`): `if (stride != sizeof(base_jd_atom_v2)) return
  -EINVAL` — 64 bytes, no exceptions (uk6 path aside). The helper only
  translates the *header*; any caller that submits the result gets
  `-EINVAL`, and worse, a kernel walking a 48-byte stride over 64-byte
  atoms would misalign. Fix: set `dst->stride = sizeof(*dst)` and make the
  API do (or require, loudly documented) per-atom expansion into a 64-byte
  array. Current comment ("Keep the source stride") is wrong and must go.
- Vendor id-shift claims (`R7P0_VENDOR_FIRST/LAST/SHIFT`) are UNVERIFIED —
  no t72x tree available; needs a t72x source check before any shim honors
  those ids.

## 12. `patches/gpu-mesa-kbase/0001-…-kbase-UK-10.6-backend.patch` — NEEDS FIXES

1883-line `pan_kmod` kbase backend. UK ABI numbers all verified against
`gpu31/scratch`: `UK_FUNC_ID=512`, func ids +0/+4/+14/+16/+18/+24/+28/+31,
`BASE_MEM_*` bit values, `BASE_JD_REQ_*` values, `BASE_JD_ATOM_COUNT=256`,
all struct sizes/offsets (incl. `core_req@44`), tracking handle `3<<12`
+ `kbase_mmap:2154` check, `_IOC_SIZE`-only dispatch in `kbase_ioctl`
  (direction bits ignored — the `_IOC_WRITE` encoding is fine),
  `kbase_check_alloc_flags` semantics (EX⋀(WR∨GROW) rejected — the backend
  never combines them; a both-flags caller just gets a clean alloc
  failure), event struct 24 B, dump-counters 64 B. Solid ABI groundwork.

- **F1 [REQUIRED — slot accounting] `n_slots` is short by one, always.**
  `n_slots=1 (+1 if wait)` but the batch needs job+trigger (+wait):
  2 or 3 slots. `kbase_alloc_slots_locked` fills only `n_slots` entries,
  so `trig_slot` (and `job_slot` in the wait case) reads an uninitialized
  stack slot → duplicate/garbage `atom_number`s → atom-table collisions and
  retiring the wrong atoms. Fix: `n_slots = 2; if (in_fd >= 0) n_slots++;`.
- **F2 [REQUIRED] BO free path relies on munmap-is-free.** That matches the
  kernel (`mali_kbase_mem.c:1243`: SAME_VA freed through munmap) — correct,
  but `kbo->mk_flags` is then write-only; drop it or assert on it. Also
  confirm `munmap` failure paths can't strand kernel mappings (munmap
  essentially can't fail here — fine).
- Non-blocking notes (no fix required): single in-fence limit (`>1` →
  `-EINVAL`) is consistent with the §4 shim but caps multi-wait UMs — keep
  explicit. `core_req` mapping drops CF/V/AFBC — benign hint simplification.
  Sync `wait()` is a 1 ms poll loop — fine for bring-up, note power cost.
  `bo_import/export` honestly unimplemented ⇒ display→GPU PRIME import has
  no Mesa side yet (pairs with §10 F1). `va_alignment=0` + `commit_pages=1`
  for GROW matches kernel expectations. `handle 0 = no fence` respected.
  `stream_create` name/fd layout matches `uku.h:213`. Props
  `coherency_mode != 31` relies on `COHERENCY_NONE=31` + absent DT property
  — plausible, mark TRUSTED-BUT-UNVERIFIED (didn't read
  `mali_midg_coherency.h`). aarch64-only (no 32-bit `compat_value` path) —
  document; `CONFIG_COMPAT` userland would mis-encode `addr.value`.
- The `drmGetVersion`-refuses-DRM probe in this patch is superseded by the
  §15 adapt diff (required for the H2 render-node shim); apply order: 0001
  then adapt.

## 13. `patches/gpu-mesa-kbase/0002-…-backend-neutral-JM-….patch` — CONDITIONAL PASS

Hook dispatch + `pan_kmod_sync_wait(…, INT64_MAX, WAIT_FOR_ALL, NULL)` is
the right neutralisation; DRM-backend behaviour preserved by fallthrough.
**Check in full `pan_jm.c` context**: the added early `return` after the
sync wait must not skip `jm_ctx_destroy` for backends that provide it —
read the hunk as "wait-then-return inside the syncobj branch", and confirm
the destroy path is still reached for the kbase backend (handle 0,
destroy no-op — leak-free either way, but verify no double-return
confusion).

## 14. `patches/gpu-mesa-kbase/0003-…-kbase-skeleton-….patch` — PASS

Honest bring-up probe (screen + context, no drawing, documented exit
codes). Only risk is build-time: `#include "driinfo_panfrost.h"` + static
option table must match what the DRI target compiles in — the Mesa build
will confirm or fail loudly. No ABI surface.

## 15. `patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff` — PASS

`F_DUPFD_CLOEXEC` + side-effect-free version-query probe: real DRM fails
the UK call (`ENOTTY` → refused, DRM backends keep it); the H2 shim passes
and is adopted, with the full handshake repeated later on the real fd.
No fd lifetime change, `close(probe)` on all paths. Correct as the §12
companion.

## 16. `patches/userspace/gpu/0001-j7elte-gpu-soft-package.patch` — PASS

llvmpipe-on-fbdev + `LIBGL_ALWAYS_SOFTWARE=1`. Correct safe default while
no HW GL exists; the env file itself says to remove it once HW GL lands —
make that removal an explicit M3-cutover checklist item so llvmpipe can't
shadow the kbase winsys.

## 17. `patches/userspace/gpu-panfrost/0001-…-mali-t720-compatible.patch` — PASS

Second compatible keeps kbase bound today (`"arm,mali"` first, kbase
built-in/early). At cutover, `CONFIG_MALI_MIDGARD=n` is mandatory — if both
drivers are ever built-in together, probe order decides the bind. Document
alongside the §7 DT change.

## 18. `patches/drm-kms-coexist/DESIGN.md` — PASS (spec, no code)

Accurately describes `drm_platform.c` (minors created before `load()`; no
`DRM_MINOR_RENDER` allocation on the 3.10 platform path — allocator support
exists in `drm_stub.c` but is never invoked there). Consistent with the
H2-shim strategy (§15). No action; keep as the reference for the M2/M3
ioctl-number allocation (§3 F-note).

---

## Cross-cutting

- **Locking:** §1 correct (cb_lock discipline, GFP_ATOMIC under spinlock);
  §2/§4 are the problem children (missing resv locking; entity/job list
  conflation). §6 IRQ/vblank paths take no sleeping locks — clean. Mesa
  backend (§12) mutex/cond usage is sound (MONOTONIC cond clock matches the
  deadline source).
- **Endianness/alignment:** all kernel structures native-LE; ioctl size
  conveyed by `_IOC_SIZE`; static asserts cover the UK layouts. No wire
  packing issues. 32-bit compat userland is out of scope for the Mesa
  backend (document it).
- **3.10 API misuse:** `idr`/`alloc_ordered_workqueue` usage verified
  against tree headers; `iovmm_*`, `ARCH_EXYNOS`, DSI connector types, and
  `devm_clk_get` availability are the four build-time verifications for
  the integrator (all fail loudly at compile, none are silent runtime
  risks — except §12 F1 and §11 F1, which are silent and hence REQUIRED).
- **Patch interaction map:** §6→§7→§8→§10 chain (swap path) vs §9
  (node-only path) — pick one per milestone. §11 blocks any r7p0-blob shim
  until the stride fix lands. §12 F1 blocks any submit test. §4 F1–F5 block
  the M3 scheduler. Suggested merge order after fixes: §5, §1–§3, §4, §6–§9
  (one path), §10, §11, §12+§15, §13–§14, §16–§17.
- **HW gates (M1Verify, no reboot without Main's go):** stock DT interrupt
  count (§6 F3); `/dev/dri` node presence per chosen path; kbase UK
  version/handshake round-trip; single-compat DT post-image (§7); CMA
  pressure at 32 MB.
