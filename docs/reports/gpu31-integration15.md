# gpu31 integration15 — probenumeric live, gates refreshed (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator15. Phone: up on int14; this
ticket builds only — Main flashes. Consumes 3bcffae as-is
(numeric %lu ST-OPS with `runexp=` from `p7580_drv_run_job_addr()`
weak/strong, ST-FINISH `owner=`, ST-RUN-entry first print, `run_seq`
stamp, `file_priv`/`drv_priv` stamped at SUBMIT + `run_bail`
NULLJOB/BADSLOT/BADENT/NODRV + `run_fallback`, ST-RETIRE `ran=`/`owner=`);
no source changes beyond the int15 script (copy of int14 + refreshed
numeric/bail gates + weak/strong linkage check + stamped-slot disasm)
and this report.

## 1. Build

Script: `code/gpu31/series/build-int15.sh` (committed, copy of int14
with int15 names + header citing 3bcffae + refreshed step 6b gate, 6c
freshness, step 8 scratch+stage deletion).
Fresh `cp -a` of the pristine tree → `scratch-int15`, all 9 `--check`
clean (backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002),
then `-j4` Image build, RC=0. No repair needed (driver 0001 regen at
3bcffae, submit.c `+1,897 → +1,976`, job.h `+1,155 → +1,168`,
applies clean).
Log: `…/j7k/gpu31/logs/gpu31-int15-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY arch/arm64/boot/Image`;
only `error`-grep hits are the benign ext4 `strncpy`/`__save_error_info`
+ `scsi_error.o` filename + one `KERN_ERR` macro line — none in
panfrost/drm paths; zero `No such file` lines).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Gate (step 6b, RC=1 FAIL on any disagreement — scratch kept for
diagnosis): single definitions via nm in both objects and System.map
(plus `__ksymtab_drm_sched_job_alloc` present and
`p7580_drv_run_job_addr` weak-W in shim TU / strong-T in submit TU /
T in System.map), offsetof getters (24/232/236) compiled with the
real kbuild include chain, stp/ldr immediates via objdump (alloc TU
#16 fused store; run_job stamped-slot `#4` load + `cmp #0x2` range
check — #232 sched-slot load is gone by design, stamps are primary),
pins + round-trip + numeric ST-OPS/ST-RUN/bail/retire in the applied
tree and Image strings.
Console: `…/logs/gpu31-int15-console.log` (RC=0, gate PASS lines);
evidence: `…/logs/gpu31-int15-evidence.log`.
Scratch + stage + offcheck one-shots deleted after the build; kept:
`Image-int15`, `kernel-boot-int15.img`, both logs.

## 2. Gate evidence (computed inside the build, not inferred)

Objects present (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121912 B, 2026-11-20 15:40:13)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (224704 B, 15:40:20)
- `arch/arm64/boot/Image` (18160760 B, 15:42:09)

Single definitions (duplicate shim copy would fail here):

- `drm_sched.o`: `0000000000000500 T drm_sched_job_alloc` (exactly 1)
- `submit.o`: `0000000000000284 t p7580_drv_run_job` (exactly 1),
  `0000000000000000 r p7580_drv_sched_ops` (exactly 1)
- `System.map`: `ffffffc00056cb00 T drm_sched_job_alloc`,
  `ffffffc000578024 t p7580_drv_run_job`,
  `ffffffc000b4bff0 r p7580_drv_sched_ops` (each exactly 1)
- `__ksymtab_drm_sched_job_alloc` (`r`, in `drm_sched.o`)
- `p7580_drv_run_job_addr`: `W` (weak default) in `drm_sched.o`,
  `T` (strong def) in `submit.o`, `ffffffc000577f90 T` in System.map
  (strong def wins the link; ST-OPS `run=` vs `runexp=` needs no kallsyms)

True in-tree layout, computed with the kbuild include chain (staged
shim headers + real 3.10.108 headers, `-std=gnu89`, `mov w0, N`
immediates): **ops=24 slot=232 ran=236** (all three `= 24/232/236`
compares passed).

Compiler-emitted offsets in both objects' disassembly:

- alloc TU stores entity+ops as ONE fused store (ops word = base+24):
  `550: a9015093 stp x19, x20, [x4, #16]` in `drm_sched_job_alloc`
- run_job reads the SUBMIT-stamped slot from the backend job
  (`p7580_submit_job.slot` = word 2: u32 seqno + int slot → +4):
  `2b0: b94004cc ldr w12, [x6, #4]` + `2b8: cmp w12, #0x2`
  (P7580_NUM_JOB_SLOTS range check) in `p7580_drv_run_job`;
  sched-slot #232 survives only as `sslot=` in the range-checked
  fallback path, so it is informational, not gated

Pins + round-trip + probes (applied tree + linked Image):

- `BUILD_BUG_ON(offsetof(struct drm_sched_job, ops) != 24)` in applied
  `submit.c`; slot/ran pins (`!= 232` / `!= 236`) in applied
  `drm_sched.h` AND applied `submit.c`
- `BUG_ON(job->ops != ops)` round-trip in applied `drm_sched.c`
- `file_priv` + `drv_priv` stamps in applied `panfrost7580_job.h`
  (`struct p7580_submit_job`); `p7580_drv_run_job_addr` hook in applied
  `drm_sched.c` + strong def in applied `submit.c`
- Shim TU (`drm_sched.o` strings): numeric
  `p7580_sched: ops job=%lu opsoff=%lu ops=%lu run=%lu runexp=%lu owner=%lu slot=%d`
  + `p7580_sched: finish ran=%d slot=%d owner=%lu`
- Backend TU (`submit.o` strings): ST-RUN entry
  `p7580_sched: run job=%lu ent=%lu owner=%lu ops=%lu runexp=%lu`,
  all four `p7580_sched: run_bail NULLJOB/BADSLOT/BADENT/NODRV`,
  `p7580_sched: run_fallback`, `p7580_sched: run_job`,
  ST-RETIRE `p7580_sched: retire seq=%u slot=%d sslot=%d ran=%u owner=%lu`
- Image strings (each presence-gated, not a fixed total):
  ST-OPS numeric ×1, ST-RUN entry ×1, `run_bail` ×4 (one per guard),
  `retire` ×1, ST-FINISH owner ×1 → `p7580_sched:` total = **17**
  (5 shim + 12 driver; int14's `= 11` count retired — bail/fallback/
  ST-RUN-entry/retire-owner prints are new)

## 3. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Built `.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_EXYNOS`,
`CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
set`; no `DRM_EXYNOS7580`. Vendor fbdev intact; `.driver_features =
DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`; `Mali-T720 render-only`
string in main.o (all unchanged from int14).

## 4. Freshness proof (clean tree, not incremental reuse)

- Scratch born 2026-11-20 15:38:14 (`cp -a` of the pristine tree);
  objects newer (drm_sched.o 15:40:13, submit.o 15:40:20, Image
  15:42:09) — built from this tree, not carried over.
- Pristine negatives (before `cp -a`, still true after): no
  `drivers/gpu/drm/drm_sched.c`, no `drivers/gpu/drm/panfrost/` —
  the shim and driver exist only via the 9 applied patches.
- `scratch-int15` + `stage-int15` + `offstage-int15` all deleted
  (verified absent); offcheck one-shots (`offcheck15.c/.s`) removed.

## 5. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `7631859b64bbdae2715d84425323382a4012898e2556aa6decbd2d699e24dc1d`
  (`Image-int15`, 18160760 B, 2026-11-20 15:42:12 -0500)
- boot.img: `5acfcab81dbae6ae593a5c8a7c1a8799115e272b40d46143d9d65c2936e3facb`
  (`kernel-boot-int15.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344, 2026-11-20 15:42:11 -0500)

Seeds (read-only, never rebuilt): dt.img
`352bda9bcf02bce50c9fc6ad6ca845727715fe1942fe25604d67f94e69d8f937`
(313344 B), ramdisk-new.gz
`ac0b6f8b9d1e3fe1298d931491bd972071755379cea788d3b1ada248c8491a18`
(8822392 B), cmdline `console=tty0 loglevel=7`.

## 6. Commits (this ticket)

- int15 script: `code/gpu31/series/build-int15.sh` (new) plus this
  report. No source/patch edits — 3bcffae consumed as-is.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against probenumeric §4 diagnosis rows
  (`run == runexp` with no kallsyms lookup; `opsoff - job` must equal
  24; `run` entry then `run_job` then `retire ran=1 owner=`; any
  `run_bail KIND` names the rejecting guard). No flash by this ticket.
