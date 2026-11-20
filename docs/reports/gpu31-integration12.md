# gpu31 integration12 — schedtrace probes live (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator12. Phone: on int11; this
ticket builds only — Main flashes.
Consumes 8fd555e as-is (bounded ST-IOCTL/ST-QUEUE/ST-RUN/ST-PMGET/
ST-RETIRE probes in submit.c + ST-PUSH/ST-PUMP/ST-FENCE in the 0004
shim, budget 8 each; jobtrace2 1450d16 hooks carry over); no source
changes beyond the int12 script (copy of int11 with int12 names +
header citing 8fd555e) and this report.

## 1. Build

Script: `code/gpu31/series/build-int12.sh` (committed, copy of int11
with scratch-int12/log-int12/boot-int12/stage-int12 names; header cites
1244082 + 1450d16 + 8fd555e; pack step writes every byte to the external
disk — pack inputs staged from read-only `/tmp/bimg` seeds into
`stage-int12`, outputs to `…/j7k/gpu31/`; nothing written to `/tmp` or `/`).
Fresh `cp -a` of pristine tree → `scratch-int12`, all 11 `--check`
clean (backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002),
then `-j4` build. No repair needed (driver 0001 regen at 8fd555e,
submit.c `+1,842`, applies clean).
Log: `…/j7k/gpu31/logs/gpu31-int12-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY arch/arm64/boot/Image`,
RC=0; only `error`-grep hits are ext4 `s_last/first_error_func`
strncpy lines plus the int11-family traces (`__save_error_info`
`-Werror` traces, `scsi_error.o` filename, one `KERN_ERR` macro line,
one `No such file` line for the absent Android NDK gcc — none in
panfrost/drm paths, no post-repair failures since no repair was needed).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Scratch + stage deleted after the build; kept: `Image-int12`,
`kernel-boot-int12.img`, build log.

## 2. Probe-string evidence (Image-int12)

Acceptance note: the ticket's literal `strings Image | grep -c 'ST-'`
yields **0** — by design, not a build failure. The ST-* IDs exist only
in source comments (`submit.c` ST-IOCTL/ST-QUEUE/ST-RUN/ST-PMGET/
ST-RETIRE, `drm_sched.c` ST-PUSH/ST-PUMP/ST-FENCE per 8fd555e §1); the
emitted `pr_info` format strings carry the `p7580_sched:` prefix. All 9
runtime format strings are present in the built Image (each count 1):

- `p7580_sched: ioctl` / `queued` / `run_job` / `pm_get HELD` /
  `pm_get FAIL` / `retire` (submit.c, 5 sites + both pm_get arms)
- `p7580_sched: push` / `pump` / `fence` (0004 shim, 3 sites)
- `strings Image-int12 | grep -c 'p7580_sched:'` = 9
- jobtrace `p7580_job:` dispatch/irq/fence-signal/irq-err still present
  (count 4, 1244082 unchanged)

Object-level (scratch-int12, aarch64, before deletion):
`strings panfrost7580_submit.o | grep p7580_sched` → all 6 submit.c
formats + `p7580_sched_trace` budget symbol; `strings drm_sched.o` →
push/pump/fence formats + budget symbol.

## 3. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Built `.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`,
`CONFIG_DRM_EXYNOS`, `CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
:set`; no `DRM_EXYNOS7580`. Vendor fbdev intact.

## 4. Prior fixes intact

- jobtrace2 hooks active: `U drm_syncobj_replace_fence` +
  `U p7580_gem_job_done_sync` in submit.o; relocs `64:
  R_AARCH64_CALL26 p7580_gem_job_done_sync` (free_job),
  `37c: replace_fence` (run_job async install) + `1034:
  replace_fence` (SUBMIT NULL-install site); `T p7580_gem_job_done_sync`
  (0x46c) in gem.o.
- Teardown holds: `T p7580_drv_teardown_hold/release` in submit.o;
  render-only (`.driver_features = DRIVER_GEM | DRIVER_PRIME |
  DRIVER_RENDER_ONLY` in main.c:283); shmem refs in gem.c;
  syncobj `panfrost7580_syncobj.{c,o}` present.

## 5. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `2aca5b8a06ec123c5c6b3fd13ea7c2f7b363c8937d8288bcf5889b915c0361f2`
  (`Image-int12`, 18160760 B, 2026-11-20 ~12:21 UTC)
- boot.img: `4e9947fdbc1f7422be95b074dc33e0c9d5f9d7355f6ad4eb6633d958aa3a2242`
  (`kernel-boot-int12.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 6. Commits (this ticket)

- int12 script: `code/gpu31/series/build-int12.sh` (new) plus this
  report. No source/patch edits — 8fd555e consumed as-is.
- Post-flash rerun: NULL-job submit-test with `dmesg | grep -E
  'p7580_sched|p7580_job'`, match against schedtrace §2 sequences and
  report which diagnosis-table row the node hits. No flash by this ticket.
