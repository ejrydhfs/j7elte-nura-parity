# gpu31-integration35 — cachedir (direction-correct DMA cache maintenance on every BO path), all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator35. Phone: on int34 (Mesa
patched); this ticket builds only — Main flashes. No flash per
ticket.

Consumes 6d187b2 (gpu31-cachedir: `p7580_gem_clean_for_device` =
`sync_for_device(DMA_TO_DEVICE)` and `p7580_gem_invalidate_for_cpu` =
clean THEN `sync_for_cpu(DMA_FROM_DEVICE)`; SUBMIT = clean only;
completion / debug read / snapshot / JDBODY dump = clean-then-
invalidate; `dma_unbind` cleans before `dma_unmap_page`; driver 0001
regenerated with the snapshot bare-`job_done_sync` removed and the
PTEWALK owner-BO invalidate added; host model
`gem/cachedir_model.c`, 27 checks) on top of int34 (0072a74
serialfence). No source changes beyond the int35 script (copy of
int34 + renamed names + new 6u gates) and this report.

## 1. Build

Script: `code/gpu31/series/build-int35.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int35` (birth 19:49:23, tree mtime
19:53:40), all patches `--check` green (backport 0001–0005 ×5,
Mode-B ×1, dmabuf ×1, driver 0001+0002 — driver 0001 is the
6d187b2-regenerated source), `-j4` Image build, RC=0.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int35-build.log` (5393 lines, 0
`error:`, ends `OBJCOPY arch/arm64/boot/Image`; the only link
warnings are the ambient `vmlinux has a LOAD segment with RWX
permissions` ones, same as int33/int34).
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
27332608 B = kernel 18193592 + ramdisk 8822392 + dt 313344
(+3280 header), built by `stage-int35/mkboot.py`.
Console: build stdout (RC=0, 18 PASS notes, 0 FAIL); evidence:
`…/logs/gpu31-int35-evidence.log` (390 lines).
Scratch + stage deleted after the pack; kept: `Image-int35`,
`kernel-boot-int35.img`, both logs, `offstage-int35`.

Freshness (evidence tail): scratch birth/mtime as above; the
pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory (the shim and the driver exist
only because the patches created them).

## 2. Gate evidence (computed inside the build, not inferred)

Prior fixes intact — all 18 gate groups PASS unchanged: ops layout
**ops=24 slot=232 ran=236**, single defs, numeric ST-OPS/`runexp`,
ST-RUN/bails/retire/finish, weak/strong `run_job_addr`; IRQ
(`(status|rawstat)` demux, rearm + AS0 program on recover/resume);
slot routing (FS→JS0, req0→JS1, ONLY_COMPUTE→JS2); ABI-align
(BUILD_BUG_ON pins, SYNC_FILE tolerance, copy caps); gpuwrite (Zero
contract, `0xCF` fops route, `job_done_sync` reuse, 6-row table);
coherency (3 pre-signal call sites, `bl` in IRQ/timedout/recover,
post-signal free_job walk); errtrace (`ERR`/`TIMEOUT`/`trace_arm`/
`dump`, `Image p7580_job: ERR = 2`); instrtype (`dma_resv_add_excl_fence`
in run_job, shim-owned lock); iova (free-time unmap before
`iova_free`, invalidate-map-invalidate); armfence (alloc at arm,
SUBMIT out_sync install, identity pump); ptewalk (snap/live owner,
unconditional 8-word dump); pmget (5 sites, 1 inline `< 0`);
timedout probes (`TMD-` count = 3); submitsnap (`desc[8]`/`sw[8]`,
`sw=` count = 5); syncobj-wait (WAIT_ALL re-test, writable
signaled-ops); jddecode (`bo_count` stamp, JDBODY count = 3);
multidep (AND node, staged errno, no `EOPNOTSUPP`); serialfence
(`serial_prev` chain, 3 `add_dependency` relocs, publish/rollback/
fini).

This ticket's new 6u cachedir gates (PASS):

- Tree: `p7580_gem_clean_for_device` / `p7580_gem_invalidate_for_cpu`
  defs + header decls present; `p7580_bo_sync_pages` carries exactly
  **2** `dma_sync_single_for_` calls, `DMA_TO_DEVICE` on the
  for-device leg and `DMA_FROM_DEVICE` on the for-CPU leg, and **no
  `DMA_BIDIRECTIONAL`**; `invalidate_for_cpu` body order is
  `sync_pages(bo, 1)` (:7) then `sync_pages(bo, 0)` (:8);
  `submit_sync` calls `clean_for_device(obj)` and contains no
  invalidate/`DMA_FROM_DEVICE`; `job_done_sync` calls
  `invalidate_for_cpu(obj)` and no bare `sync_for_cpu(bo->sync_dev`;
  `dma_unbind` calls `clean_for_device(obj)` (:409) before
  `dma_unmap_page(` (:411).
- Driver tree: snapshot calls `p7580_gem_invalidate_for_cpu(&jbo->base);`
  (:1357) **before** the `job->desc[wi]` read (:1360); PTEWALK dump
  calls `p7580_gem_invalidate_for_cpu(owner);` (:510) before the
  first `p7580_job: JDBODY` line; exactly **1**
  `p7580_gem_job_done_sync(` site left in submit.c (free_job, :645).
- Tree-computed ABI values: `DMA_TO_DEVICE=1`, `DMA_FROM_DEVICE=2`
  from `dma-direction.h`; `dma_map_ops` slots parsed from
  `dma-mapping.h` = **for_cpu=64 for_device=72 unmap=40**.
- gem.o: `T p7580_gem_clean_for_device` (:0x540), `T
  p7580_gem_invalidate_for_cpu` (:0x580), `t p7580_bo_sync_pages`
  (:0x0). The compiled walk passes the directions through the
  computed slots: `ldr x4, [x3, #72]` + `mov w3, #0x1` (for_device +
  TO_DEVICE) and `ldr x4, [x3, #64]` + `mov w3, #0x2` (for_cpu +
  FROM_DEVICE), paired within the same basic block; no `w3, #0x0`
  anywhere in the walk (no `DMA_BIDIRECTIONAL` sync left).
- Helper object code: `clean_for_device` makes exactly 1
  `p7580_bo_sync_pages` call with no `toward_device=0`;
  `submit_sync` exactly 1 call, no `toward_device=0`;
  `invalidate_for_cpu` and `job_done_sync` each make 2 calls in the
  order `bl sync_pages` → `mov w1, #0x0` → `bl sync_pages` (clean
  first, then the invalidate leg). `dma_unbind`: `bl
  p7580_gem_clean_for_device` (:0x984) before the unmap_page dma_ops
  slot `[x1, #40]` (:0x9a4), unmap direction `w3, #0x0`
  (BIDIRECTIONAL) — clean-before-invalidate preserved.
- Reads route through the pair: `p7580_gem_debug_read_word` and
  `p7580_gem_job_bos_pre_signal_sync` both `bl
  p7580_gem_invalidate_for_cpu` (no bare invalidate).
- submit.o: `U p7580_gem_invalidate_for_cpu`, `U
  p7580_gem_submit_sync`; `p7580_drv_submit` carries exactly 1
  invalidate_for_cpu call (:0x2230, snapshot) + 2 submit_sync calls
  (:0x1404 JD/BO drain, :0x232c chain_mode fixup) and **no**
  `job_done_sync` (the snapshot no longer bare-invalidates);
  `p7580_drv_ptewalk_pin` invalidates the owner BO (:0x3d8);
  `p7580_drv_free_job` keeps the post-signal `job_done_sync`
  (:0x118).
- Linked kernel: `T p7580_gem_clean_for_device`
  (`ffffffc00057c560`) and `T p7580_gem_invalidate_for_cpu`
  (`ffffffc00057c5a0`), exactly one each in `System.map`.

Hunk integrity (PASS): driver 0001 submit.c fully applied — late
function `int panfrost_ioctl_submit_override` present in the applied
tree, `T` in submit.o, one `T` in System.map
(`ffffffc00057be6c`).

## 3. Outputs (sha256)

- `Image-int35`:
  `04aec1bb5b3bbdac5af5972ab6b4f948f1825e067d88d21b3d03195603fe9d2d`
- `kernel-boot-int35.img`:
  `4252e2659267b030d08770a383e4c5f59f03d5540303eb3c0ea2491682bc0ff9`
- build log:
  `cd98792a15d6a06270492b5628adaa93c1bc2975a7292449fc4e2898f3c020a9`
- evidence log:
  `58de1f881121ff63e24ea3878ae229dcff7bfc5302e31837253daf616627e31c`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).

## 4. Script notes

- New 6u gates were pinned against the real -O2 codegen before the
  full run: a probe tree (patches + config + only
  `panfrost7580_{gem,submit,job}.o` and `drm_sched.o`, 27 s) showed
  the walk is outlined (`t p7580_bo_sync_pages`), the helpers
  tail-call it, and the cache ops dispatch indirectly through
  `dma_ops` (`blr x4`) — so the gates use the header-computed slot
  offsets + direction immediates + call order rather than expecting
  `bl dma_sync_single_for_*` relocs. One source-gate false positive
  was fixed in the same pass (the `job_done_sync` body comment
  mentions the old `dma_sync_single_for_cpu(BIDIRECTIONAL)`; the
  gate now matches the real call form
  `dma_sync_single_for_cpu(bo->sync_dev`). The full build then
  passed on the first run; no in-flight gate fix was needed
  post-build (probe scratch + test harness deleted).
- Direction values and dma_map_ops slots are recomputed from the
  built tree's headers on every run (pahole-free), so a header
  change that silently moves a slot fails the gate instead of
  mis-decoding the disasm.
