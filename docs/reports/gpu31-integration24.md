# gpu31 integration24 — iova lifetime fix, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator24. Phone: up on int23;
this ticket builds only — Main flashes.

Consumes 2dd207c (iova: free-time `p7580_gem_gpu_unmap` before
`iova_free`, PM-held TLB invalidate-map-invalidate reuse fence in
map_bo, OVERLAP detector, IOVA snapshot + ERR-MAP dump, iova_arm
budget knob) on top of 71a9412 (int23) + all prior fixes. No
source changes beyond the int24 script (copy of int23 + renamed
names + 6k iova gate), two link-compat repairs in driver 0001
(`p7580_drv_iova_resolve`/`p7580_iova_trace` de-staticed with
shared decls in `panfrost7580_drv.h`; `p7580_gem_dma_bind` decl
restored in gem.h — both were TU-visibility gaps that failed the
`-Werror=implicit-function-declaration` build), the matching
one-line slice restore in `gem/panfrost7580_gem.h`, and this
report.

## 1. Build

Script: `code/gpu31/series/build-int24.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int24`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. Third run green after the two link fixes
plus one gate correction (map_bo is static/inlined — see §2);
no other script defects.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int24-build.log`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int24-evidence.log`.
Scratch + stage deleted after the build; kept:
`Image-int24`, `kernel-boot-int24.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121912 B, 2026-11-21 00:02:05)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (242544 B, 00:02:13)
- `drivers/gpu/drm/panfrost/panfrost7580_gem.o` (253384 B, 00:02:13)
- `drivers/gpu/drm/panfrost/panfrost7580_ioctl.o` (173000 B, 00:02:13)
- `drivers/gpu/drm/panfrost/panfrost7580_syncobj.o` (220096 B, 00:02:15)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (213824 B, 00:02:14)
- `arch/arm64/boot/Image` (18177144 B, 00:03:43)

Prior fixes intact (unchanged int23 gates, all PASS):

- Ops layout: **ops=24 slot=232 ran=236**, single defs, numeric
  ST-OPS/`runexp`/ST-RUN/bails/retire/finish, weak/strong
  run_job_addr linkage live.
- IRQ/completion: `(status | rawstat)` demux, rearm + AS0 program on
  recover/resume, all `p7580_job:` probes in tree + objects + Image.
- Slot routing (2936796): FS → JS0, req0 → JS1, ONLY_COMPUTE → JS2
  in jm_hw.h + `p7580_job: dispatch` in job.o + Image.
- ABI-align (de142b3): BUILD_BUG_ON pins, SYNC_FILE tolerance,
  copy caps, handler symbols in object + System.map.
- gpuwrite (67eaef1): Zero contract, `0xCF` fops route,
  `job_done_sync` reuse, 6-row table, debug symbols live.
- Coherency (c207bdd): 3 pre_signal call sites (IRQ `job->bos`,
  timedout `job->bos`, recover `fault_job->bos`), `T` in gem.o,
  single `U` in job.o, `bl` in all three fns, one `T` in System.map.
- Errtrace (7a5bf9b): `p7580_job: ERR slot=` + TIMEOUT + `trace_arm`
  + `dump` in tree + job.o + Image; `Image p7580_job: count = 17`,
  `dispatch = 1`, `irq = 4`, `ERR = 2`.
- Instrtype (71a9412): `dma_resv_add_excl_fence(` at submit.c :289
  (inside `p7580_drv_run_job`, `rj->bo_count` walk over stable
  `job->bos` refs); no `mutex_lock` within 30 lines after the call;
  shim owns the lock; `U` in submit.o, `bl` at @41c with
  `R_AARCH64_CALL26` reloc, one `T` in System.map.

Iova (2dd207c, this ticket's change — 6k gate, PASS):

- Applied tree: free path calls `p7580_gem_gpu_unmap(obj);` (:706)
  BEFORE `p7580_iova_free(gd, bo);` (:710, line-order gated).
- Applied submit.c: `^void p7580_gem_gpu_unmap` def,
  `p7580_drv_register_iova_trace`, `p7580_iova: OVERLAP`,
  `p7580_iova: map obj=`, `p7580_iova: unmap obj=` all present;
  `p7580_drv_map_bo` body carries exactly 2
  `p7580_mmu_invalidate(drv->regs` calls (purge-before + publish-after).
- Applied job.c: `p7580_job: ERR-MAP` (2 legs), `"iova_arm"` knob,
  `p7580_job_set_iova_resolver` + `p7580_job_register_iova_budget`.
- Objects: `T p7580_gem_gpu_unmap` (@a68) in submit.o,
  `U p7580_gem_gpu_unmap` in gem.o; free_object disasm @fac
  `bl 0 <p7580_gem_gpu_unmap>`; map_bo is static and inlined at
  -O2 so its two invalidate sites are reloc-gated in the caller:
  `p7580_drv_submit` disasm carries exactly 2
  `R_AARCH64_CALL26 p7580_mmu_invalidate` (@1510/@16c4) and
  `p7580_gem_gpu_unmap` carries 1 (@c14).
- Strings live in objects + Image: `p7580_iova: map obj=`,
  `p7580_iova: unmap obj=`, `p7580_iova: OVERLAP`,
  `p7580_job: ERR-MAP`, `iova_arm` (all 5 in submit.o/job.o and
  in Image). Linked Image: exactly one `T p7580_gem_gpu_unmap`
  (`ffffffc000578928`) in System.map.

## 3. Outputs (sha256)

- `Image-int24`:
  `7005038568ca1c3d510dc3bec8411d25884186493c82a9d8862e28ade41222c1`
- `kernel-boot-int24.img`:
  `b4619a66b4968d98b3a3772f60d62ddcb1a602f757ab83549fef5c5ae8661267`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
