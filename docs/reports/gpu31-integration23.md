# gpu31 integration23 — instrtype excl-fence publish, all prior gates intact

Date: 2026-11-21. Owner: G31_Integrator23. Phone: up on int22;
this ticket builds only — Main flashes.

Consumes 71a9412 (instrtype: `dma_resv_add_excl_fence` publish at
dispatch time in `p7580_drv_run_job` for every submitted BO;
resv->lock taken inside add_excl; stable job->bos refs) on top of
48f58ee (int22) + 7a5bf9b (errtrace) + c207bdd (coherency pre-signal).
No source changes beyond the int23 script (copy of int22 + renamed
names + 6j instrtype gate), a one-line hunk-count repair in driver
0001 (`@@ -0,0 +1,995 @@` → `+1,1025`, the file grew by the 30-line
instrtype hunk without the count being bumped — without it the tail
of submit.c pasted truncated and the build died at
`panfrost_ioctl_submit_override` :995), and this report.

## 1. Build

Script: `code/gpu31/series/build-int23.sh` (committed). Fresh `cp -a`
of the pristine tree → `scratch-int23`, all 9 `--check` green
(backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002), then
`-j4` Image build, RC=0. Second run green after the hunk-count fix;
no other script defects.

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.
Log: `…/j7k/gpu31/logs/gpu31-int23-build.log`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27316224 B =
kernel 18177144 + ramdisk 8822392 + dt 313344.
Console: build stdout (RC=0, all gates PASS);
evidence: `…/logs/gpu31-int23-evidence.log`.
Scratch + stage deleted after the build; kept:
`Image-int23`, `kernel-boot-int23.img`, both logs, `offcheck22.s`.

## 2. Gate evidence (computed inside the build, not inferred)

Objects (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o`
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (225312 B, 2026-11-20 22:11:52)
- `drivers/gpu/drm/panfrost/panfrost7580_gem.o` (252872 B, 22:11:52)
- `drivers/gpu/drm/panfrost/panfrost7580_ioctl.o` (173000 B, 22:11:52)
- `drivers/gpu/drm/panfrost/panfrost7580_syncobj.o` (220096 B, 22:11:54)
- `drivers/gpu/drm/panfrost/panfrost7580_job.o` (196448 B, 2026-11-20 22:11:52)
- `arch/arm64/boot/Image` (18177144 B, 2026-11-20 22:14)

Prior fixes intact (unchanged int22 gates, all PASS):

- Ops layout: **ops=24 slot=232 ran=236**, single defs, numeric
  ST-OPS/`runexp`/ST-RUN/bails/retire/finish, weak/strong
  run_job_addr linkage live.
- IRQ/completion: `(status | rawstat)` demux, rearm + AS0 program on
  recover/resume, all 7 `p7580_job:` probes in tree + objects + Image.
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
  + `dump` in tree + job.o + Image; `Image p7580_job: count = 12`,
  `ERR = 1`.

Instrtype (71a9412, this ticket's change — 6j gate, PASS):

- Applied tree: `dma_resv_add_excl_fence(` at submit.c :283
  (inside `p7580_drv_run_job`, `rj->bo_count` walk over stable
  `job->bos` refs); no `mutex_lock` within 30 lines after the call
  (shim owns the lock — SUBMIT-DEADLOCK rule).
- Resv locking: applied `drivers/base/dma-resv.c`
  `dma_resv_add_excl_fence` def takes `mutex_lock(&obj->lock)`
  internally; `get_excl` leg unchanged (same discipline).
- Object relocs: `U dma_resv_add_excl_fence` in submit.o (nm);
  objdump `p7580_drv_run_job` @41c `bl 0 <dma_resv_add_excl_fence>`
  with `R_AARCH64_CALL26 dma_resv_add_excl_fence` reloc
  (preceded by `add x0, x0, #0x98` resv-embed + `mov x1, x21` fence).
- Linked Image: exactly one `T dma_resv_add_excl_fence`
  (`ffffffc0005d1f20`) in System.map.

## 3. Outputs (sha256)

- `Image-int23`:
  `50947481e2f723f81342994f04b5e0622fcbba1ad5d12166f22cb07a2f0fb3ec`
- `kernel-boot-int23.img`:
  `d4daba55e06330ea2d19a9f42575c712aebef5b3f4c1a319ac752abdfdf5cf29`

Config: pmaports `config-samsung-j7elte.aarch64` base + d0-m1-minimal
+ d4-panfrost7580 fragments, then `olddefconfig` closure. No flash
per ticket (Main flashes).
