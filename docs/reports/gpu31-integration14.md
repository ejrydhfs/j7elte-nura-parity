# gpu31 integration14 — opsfix live, layout computed in-build (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator14. Phone: up on int13; this
ticket builds only — Main flashes. Consumes df7c4b9 as-is
(BUILD_BUG_ON ops/slot/ran pins in both TUs, alloc round-trip BUG_ON,
ST-OPS `job/opsoff/ops/run/slot` probe, submit call-site guard);
no source changes beyond the int14 script (copy of int13 + in-build
ops-layout gate) and this report.

## 1. Build

Script: `code/gpu31/series/build-int14.sh` (committed, copy of int13
with int14 names + header citing df7c4b9 + step 6b gate, 6c freshness,
step 8 scratch+stage deletion).
Fresh `cp -a` of the pristine tree → `scratch-int14`, all 9 `--check`
clean (backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002),
then `-j4` Image build, RC=0. No repair needed (driver 0001 regen at
df7c4b9, submit.c `+1,897`, applies clean).
Log: `…/j7k/gpu31/logs/gpu31-int14-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY arch/arm64/boot/Image`;
only `error`-grep hits are the benign ext4 `strncpy`/`__save_error_info`
+ `scsi_error.o` filename + one `KERN_ERR` macro line — none in
panfrost/drm paths; zero `No such file` lines).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Gate (step 6b, RC=1 FAIL on any disagreement — scratch kept for
diagnosis): single definitions via nm in both objects and System.map,
`__ksymtab_drm_sched_job_alloc` present, offsetof getters (24/232/236)
compiled with the real kbuild include chain, stp/ldr immediates via
objdump, pins + round-trip + ST-OPS in the applied tree and strings.
Console: `…/logs/gpu31-int14-console.log` (RC=0, gate PASS lines);
evidence: `…/logs/gpu31-int14-evidence.log`.
Scratch + stage deleted after the build; kept: `Image-int14`,
`kernel-boot-int14.img`, both logs. Offcheck one-shot files
(`offcheck14.c/.s`, `offstage-int14`) removed with the scratch.

## 2. Gate evidence (computed inside the build, not inferred)

Objects present (before deletion, aarch64, `--time-style=full-iso`):

- `drivers/gpu/drm/drm_sched.o` (121464 B, 2026-11-20 14:42:48)
- `drivers/gpu/drm/panfrost/panfrost7580_submit.o` (220120 B, 14:42:59)
- `arch/arm64/boot/Image` (18160760 B, 14:44:54)

Single definitions (duplicate shim copy would fail here):

- `drm_sched.o`: `0000000000000500 T drm_sched_job_alloc` (exactly 1)
- `submit.o`: `0000000000000340 t p7580_drv_run_job` (exactly 1),
  `0000000000000000 r p7580_drv_sched_ops` (exactly 1)
- `System.map`: `ffffffc00056cb00 T drm_sched_job_alloc`,
  `ffffffc0005780c0 t p7580_drv_run_job`,
  `ffffffc000b4bff0 r p7580_drv_sched_ops` (each exactly 1)
- `__ksymtab_drm_sched_job_alloc` (`r`, in `drm_sched.o`)

True in-tree layout, computed with the kbuild include chain (staged
shim headers + real 3.10.108 headers, `-std=gnu89`, `mov w0, N`
immediates): **ops=24 slot=232 ran=236** (all three `= 24/232/236`
compares passed).

Compiler-emitted offsets in both objects' disassembly:

- alloc TU stores entity+ops as ONE fused store (ops word = base+24):
  `550: a9015093 stp x19, x20, [x4, #16]` in `drm_sched_job_alloc`
- driver reads the carried slot at the computed offset:
  `344: b940e802 ldr w2, [x0, #232]` and
  `518: b940e803 ldr w3, [x0, #232]` in `p7580_drv_run_job`
  (slot re-derived nowhere — entity-pointer math survives only as the
  range-checked fallback, `cmp w2, #0x2` / `b.ls`)

Pins + round-trip + probe (applied tree + linked Image):

- `BUILD_BUG_ON(offsetof(struct drm_sched_job, ops) != 24)` in applied
  `drm_sched.c` AND applied `submit.c` (line 711, inside the layout-pin
  function); slot/ran pins (`!= 232` / `!= 236`) in applied
  `drm_sched.h` AND applied `submit.c`
- `BUG_ON(job->ops != ops)` round-trip in applied `drm_sched.c`
- `p7580_sched: ops job=%p opsoff=%p ops=%p run=%p slot=%d` in applied
  `drm_sched.c`, in `drm_sched.o` strings, and in the Image
- `strings Image-int14 | grep -c 'p7580_sched:'` = **11**
  (6 submit.c + push/pump/fence/ops/finish; `pm_get` ×2 arms)
- `strings Image-int14 | grep 'p7580_sched: ops'` →
  `p7580_sched: ops job=%p opsoff=%p ops=%p run=%p slot=%d`

## 3. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Built `.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_EXYNOS`,
`CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
:set`; no `DRM_EXYNOS7580`. Vendor fbdev intact; `.driver_features =
DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`; `Mali-T720 render-only`
string in main.o (all unchanged from int13).

## 4. Freshness proof (clean tree, not incremental reuse)

- Scratch born 2026-11-20 14:40:21 (`cp -a` of the pristine tree);
  objects newer (drm_sched.o 14:42:48, submit.o 14:42:59, Image
  14:44:54) — built from this tree, not carried over.
- Pristine negatives (before `cp -a`, still true after): no
  `drivers/gpu/drm/drm_sched.c`, no `drivers/gpu/drm/panfrost/` —
  the shim and driver exist only via the 9 applied patches.
- `scratch-int14` + `stage-int14` + `offstage-int14` all deleted
  (verified absent); offcheck one-shots removed.

## 5. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `927d028757b0d52e60779c3f22239e977d06f9267d776696e22d1fc54c3709b9`
  (`Image-int14`, 18160760 B, 2026-11-20 14:44:55 -0500)
- boot.img: `4cd04be7cd4defe8163d5d4716139d9e7eb743309e659a14fc25ba9acedceda9`
  (`kernel-boot-int14.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344, 2026-11-20 14:44:55 -0500)

Seeds (read-only, never rebuilt): dt.img
`352bda9bcf02bce50c9fc6ad6ca845727715fe1942fe25604d67f94e69d8f937`
(313344 B), ramdisk-new.gz
`ac0b6f8b9d1e3fe1298d931491bd972071755379cea788d3b1ada248c8491a18`
(8822392 B), cmdline `console=tty0 loglevel=7`.

## 6. Commits (this ticket)

- int14 script: `code/gpu31/series/build-int14.sh` (new) plus this
  report. No source/patch edits — df7c4b9 consumed as-is.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against opsfix §6 diagnosis rows
  with the new `job=`/`opsoff=` fields (`opsoff - job` must equal
  `0x18`; `ops` vs `/proc/kallsyms p7580_drv_sched_ops`,
  `run` vs `t p7580_drv_run_job`). No flash by this ticket.
