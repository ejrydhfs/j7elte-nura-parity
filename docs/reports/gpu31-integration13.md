# gpu31 integration13 — pumpfix live (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator13. Phone: up on int12; this
ticket builds only — Main flashes.
Consumes de07c37 as-is (0004 pump preserves SUBMIT slot, ST-OPS
`ops/run/slot` + ST-FINISH `ran/slot` probes, NULL-guarded run_job
call, IS_ERR_OR_NULL/ran stamp, retire-after-fence; driver stamps
`sched_job->slot` at SUBMIT, run_job trusts it); no source changes
beyond the int13 script (copy of int12 with int13 names + header
citing de07c37) and this report.

## 1. Build

Script: `code/gpu31/series/build-int13.sh` (committed, copy of int12
with scratch-int13/log-int13/boot-int13/stage-int13 names; header cites
1244082 + 1450d16 + 8fd555e + de07c37; pack step writes every byte to
the external disk — pack inputs staged from read-only `/tmp/bimg`
seeds into `stage-int13`, outputs to `…/j7k/gpu31/`; nothing written to
`/tmp` or `/`).
Fresh `cp -a` of pristine tree → `scratch-int13`, all 11 `--check`
clean (backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002),
then `-j4` build. No repair needed (driver 0001 regen at de07c37,
submit.c `+1,866`, applies clean).
Log: `…/j7k/gpu31/logs/gpu31-int13-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY arch/arm64/boot/Image`,
RC=0; only `error`-grep hits are the 6 benign int-family lines — ext4
`s_last/first_error_func` strncpy, `__save_error_info`, `scsi_error.o`
filename, one `KERN_ERR` macro line, one absent-NDK-gcc `No such file`
line — none in panfrost/drm paths).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Scratch + stage deleted after the build; kept: `Image-int13`,
`kernel-boot-int13.img`, build log.

## 2. Probe evidence (Image-int13 + objects)

Acceptance note: the ticket's literal `strings Image | grep -c 'ST-'`
yields **0** — by design, not a build failure. The ST-* IDs exist only
in source comments; the emitted `pr_info` format strings carry the
`p7580_sched:` prefix. All 11 runtime format strings are present in the
built Image (each count 1, `pm_get` ×2 arms → 11 total):

- `p7580_sched: ioctl` / `queued` / `run_job` (now `sslot=` +
  `owner=`) / `pm_get HELD` / `pm_get FAIL` / `retire` (now `sslot=`)
  (submit.c, 6 formats)
- `p7580_sched: push` / `pump` / `fence` (0004 shim, 3 formats,
  unchanged)
- `p7580_sched: ops ops=%p run=%p slot=%d` (NEW, ST-OPS — answers
  Main's Q1: prints `job->ops` and `job->ops->run_job` vs
  `p7580_drv_run_job`)
- `p7580_sched: finish ran=%d slot=%d` (NEW, ST-FINISH — answers
  Main's Q2: `ran=0` = retired without ever running)
- `strings Image-int13 | grep -c 'p7580_sched:'` = 11
- jobtrace `p7580_job:` dispatch/irq/irq-err/fence-signal still present
  (4, 1244082 unchanged)

Object-level (scratch-int13, aarch64, before deletion):
`strings panfrost7580_submit.o | grep p7580_sched` → all 6 submit.c
formats (incl. `sslot=` run/retire) + `p7580_sched_trace` budget
symbol; `strings drm_sched.o` → push/pump/fence/**ops**/**finish**
formats + budget symbol.

## 3. Slot plumbing compiled (struct offsets + disasm)

- `struct drm_sched_job` (`code/gpu31/sched/src/drm_sched.h`): new
  `int slot` (submit-chosen dispatch slot) + `int ran` (pumpfix stamp;
  `credits` removed); `drm_sched_job_alloc` zero-inits via `kzalloc`.
- Pump (`drm_sched.c`): slot clamped (`< 0` → 0), preserved across
  dispatch; NULL-ops/NULL-run_job retires via F4 free path;
  `IS_ERR_OR_NULL(run_fence)` (1 site) with NULL → `ERR_PTR(-ENODATA)`;
  `job->ran = 1` stamped after `run_job` returns.
- Driver (`0001…sources.patch`): SUBMIT stamps `sched_job->slot = slot`
  after `drm_sched_job_alloc` + `drm_sched_job_arm`;
  `p7580_drv_run_job` reads `ldr w2, [x0, #232]` (= `sched_job->slot`,
  disasm offset 0x340+4) and trusts it — entity-pointer math survives
  only as range-checked fallback (`cmp w2, #0x2`, `b.ls` to fallback;
  `df` recovery range-checked, OOB → `-ENODEV`); `run_job` symbol at
  `T +0x340`, `timedout_job` +0x284, `free_job` +0x0 in submit.o.
- Disasm shows the carried slot loaded first, then the pm_get gate,
  then `p7580_job_run` call with IS_ERR_OR_NULL-style NULL+ERR check
  (`cmp x0,#0` / `ccmp …, #0x2` / `b.hi` error path) — the pumpfix
  plumbing, compiled in.

## 4. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Built `.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_EXYNOS`,
`CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
:set`; no `DRM_EXYNOS7580`. Vendor fbdev intact.
Driver features: `.driver_features = DRIVER_GEM | DRIVER_PRIME |
DRIVER_RENDER_ONLY` (0001 line 3949); `Mali-T720 render-only` string
in main.o.

## 5. Prior fixes intact

- jobtrace2 hooks active: `U drm_syncobj_replace_fence` +
  `U p7580_gem_job_done_sync` in submit.o.
- Teardown holds: `T p7580_drv_teardown_hold` (+0x5a8) /
  `T p7580_drv_teardown_release` (+0x5c4) in submit.o.
- shmem-backed CREATE_BO, gem_vm_ops/vm_pgoff, syncobj
  `panfrost7580_syncobj.{c,o}` unchanged (driver 0001 regen
  byte-identical outside submit.c per pumpfix §7).

## 6. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `cd6a64716bb10fcd6d1de5371b004274f4a21b6e82261cddee4247a079f96973`
  (`Image-int13`, 18160760 B, 2026-11-20 ~13:48 UTC)
- boot.img: `504ded6c3166c79ff3ae18ef23a657b3666bd6d1d031874fb36f8ef339925565`
  (`kernel-boot-int13.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 7. Commits (this ticket)

- int13 script: `code/gpu31/series/build-int13.sh` (new) plus this
  report. No source/patch edits — de07c37 consumed as-is.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against pumpfix §6 expected sequence
  (ST-OPS `run=p7580_drv_run_job` + ST-RUN `sslot=1` + ST-FINISH
  `ran=1`); report which diagnosis-table row the node hits. No flash
  by this ticket.
