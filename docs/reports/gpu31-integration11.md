# gpu31 integration11 — jobtrace2 hooks active (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator11. Phone: int9 build host;
this ticket builds only — Main flashes.
Consumes 1450d16 as-is (run_job out_sync install + free_job
job_done_sync walk, job->bos refcount transfer on SUBMIT); no source
changes beyond the int11 script (copy of int10 with int11 names +
header citing 1450d16) and this report.

## 1. Build

Script: `code/gpu31/series/build-int11.sh` (committed, copy of int10
with scratch-int11/log-int11/boot-int11/stage-int11 names; header cites
1244082 + 1450d16; pack step writes every byte to the external disk —
pack inputs staged from read-only `/tmp/bimg` seeds into `stage-int11`,
outputs to `…/j7k/gpu31/`; nothing written to `/tmp` or `/`).
Fresh `cp -a` of pristine tree → `scratch-int11`, all 11 `--check`
clean (backport 0001–0005 ×5, Mode-B ×1, dmabuf ×1, driver 0001+0002),
then `-j4` build. No repair needed (int9/int10 hunk counts carry over;
driver 0001 regen at 1450d16 applies clean).
Log: `…/j7k/gpu31/logs/gpu31-int11-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY arch/arm64/boot/Image`,
RC=0; only `error`-grep hits are `fs/ext4` `__save_error_info`
`-Werror`-family traces, `drivers/scsi/scsi_error.o` filename, one
`KERN_ERR` macro line, and one `No such file` line for the absent
Android NDK gcc — none in panfrost/drm paths, no post-repair failures
since no repair was needed).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, 27299840 B =
kernel 18160760 + ramdisk 8822392 + dt 313344.
Scratch + stage deleted after the build; kept: `Image-int11`,
`kernel-boot-int11.img`, build log.

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Built `.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`,
`CONFIG_DRM_EXYNOS`, `CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). `# CONFIG_FRAMEBUFFER_CONSOLE is not
set`; no `DRM_EXYNOS7580`. Vendor fbdev intact.
Config diff int9→int11: IDENTICAL (sorted `.config` diff vs
scratch-int9, no output; int10 scratch already deleted at int10 close).

## 3. Call-site proof (scratch-int11 objects, aarch64; scratch deleted, evidence below)

Both int10-caveat hooks are now **active call sites** in the applied
driver snapshot (1450d16 applied clean, submit.c `+1,788`):

- `aarch64-nm panfrost7580_submit.o` →
  `t p7580_drv_run_job`, `t p7580_drv_free_job`,
  `U drm_syncobj_replace_fence`, `U p7580_gem_job_done_sync`.
  (Int10 had neither `U`; the helper was def-only.)
- Reloc→function attribution (`objdump -dr`, one enclosing `<fn>`: per
  reloc line):
  - `p7580_drv_free_job: 64: R_AARCH64_CALL26 p7580_gem_job_done_sync`
  - `p7580_drv_run_job: 368: R_AARCH64_CALL26 drm_syncobj_replace_fence`
  - third `replace_fence` reloc (offset 1048) sits under
    `<p7580_drv_submit>` = the SUBMIT-time NULL-install site
    (out_sync handle validation, `-ENOENT` preserved) — present
    alongside the new async install, as designed.
- `aarch64-nm panfrost7580_gem.o` → `T p7580_gem_job_done_sync`
  (0x46c); also `T` in `vmlinux` (`ffffffc00057938c`).
  `drm_syncobj_replace_fence` is `T` in `vmlinux`
  (`ffffffc00056bfc0` + `__ksymtab`/`__kstrtab` entries).
- Applied source (scratch-int11 `panfrost7580_submit.c`):
  - run_job (offset 0x290): `if (!IS_ERR(fence) && fence &&
    job->out_sync)` → `drm_syncobj_find(&df->mgr, job->out_sync)` →
    `drm_syncobj_replace_fence(s, fence)` + `drm_syncobj_put(s)`,
    after `pm_put`, before returning the fence to the 0004 shim.
  - free_job (offset 0x0): `if (job && job->bo_count && job->bos)`
    → per-BO `p7580_gem_job_done_sync(obj)` +
    `drm_gem_object_unreference_unlocked(obj)`, before `p7580_job_free`.
  - SUBMIT: `job->out_sync = args->out_sync` stash;
    `job->bos = kcalloc(...)` + one lookup ref per BO stored
    (`job->bos[i] = objs[i]`); `err_job_bos` unwind on unpublished-job
    error paths.
- Bound-trace strings still present in `panfrost7580_job.o`
  (dispatch/irq/fence-signal/irq-err, 1244082 unchanged).

## 4. Prior fixes intact

- Teardown holds: `T p7580_drv_teardown_hold/release` in submit.o
  (hold `ffffffc000577fc4` in vmlinux); render-only
  (`.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`);
  18 `__pm_runtime` relocs (job-lifetime PM); shmem
  (`U shmem_file_setup`, `U shmem_read_mapping_page_gfp` in gem.o);
  syncobj (`T panfrost_syncobj_create/destroy/ioctl/…` in syncobj.o +
  vmlinux); dark-timeout always-signal leg (`U dma_fence_signal` ×2
  relocs in submit.o).

## 5. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `3afc34ec3b6af8efced980a054f83d4683165ced3d784f01b3f03e5452e24fbe`
  (`Image-int11`, 18160760 B, 2026-11-20 ~10:44 UTC)
- boot.img: `637253d5ca7df500f26f0ea99de17772a5554092474fb68b5a9dcbe008dc782d`
  (`kernel-boot-int11.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 6. Commits (this ticket)

- int11 script: `code/gpu31/series/build-int11.sh` (new) plus this
  report. No source/patch edits — 1450d16 consumed as-is.
- Post-flash rerun: `gpu31-jobtrace.md` §4/§6 (same table as int10,
  with the int10 §3 caveat now lifted: out_sync waits on the real
  fence installed from run_job, completion invalidate walks job->bos
  in free_job). Expect §4 row 1 (dispatch + IRQ DONE + `sig_rc=0`,
  non-zero readback `RENDER_OK`). No flash by this ticket.
