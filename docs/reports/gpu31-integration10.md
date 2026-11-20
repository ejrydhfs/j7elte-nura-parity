# gpu31 integration10 — jobtrace build (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator10b. Phone: int9 build host (#37
renderD128, X healthy per Main); this ticket builds only — Main flashes.
Consumes 1244082 (jobtrace: bounded trace + out_sync fix + completion
dcache invalidate) as-is; no source changes by this ticket beyond the
int10 script (copy of int9 with external-disk paths + boot pack on the
external disk) and this report.

## 1. Build

Script: `code/gpu31/series/build-int10.sh` (committed, copy of int9
with scratch-int10/log-int10/boot-int10 names and header citing
1244082; pack step writes every byte to the external disk — pack inputs
staged from read-only `/tmp/bimg` seeds into `stage-int10`, outputs to
`…/j7k/gpu31/`; nothing written to `/tmp` or `/`).
Fresh `cp -a` of pristine tree → `scratch-int10`, all 9 `--check`
clean, then `-j4` build. No repair needed (int9's `+1,718` hunk count
carries over untouched).
Log: `…/j7k/gpu31/logs/gpu31-int10-build.log` (5393 lines, tail
`SORTEX vmlinux` / `SYSMAP System.map` / `OBJCOPY arch/arm64/boot/Image`,
RC=0; only `error`-grep hits are `fs/ext4` `__save_error_info`
`-Werror`-family traces, `drivers/scsi/scsi_error.o` filename, and one
`No such file` line for the absent Android NDK gcc — none in
panfrost/drm paths, no post-repair failures since no repair was needed).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk-new.gz (never rebuilt),
`console=tty0 loglevel=7` cmdline, header `ANDROID!` magic verified.
Scratch + stage deleted after the build; kept: `Image-int10`,
`kernel-boot-int10.img`, build log.

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Built `.config` carries exactly the fragment symbols (all `=y`):
`CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`,
`CONFIG_DRM_EXYNOS`, `CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DMA_SHARED_BUFFER=y`). No `DRM_EXYNOS7580`, no
`FRAMEBUFFER_CONSOLE`. Vendor fbdev intact (`EXYNOS_DECON_FB=y`,
`MALI_MIDGARD=y`).
Config diff int9→int10: IDENTICAL (sorted `.config` diff, no output).

## 3. Symbol proof (scratch-int10 objects, aarch64; scratch deleted, evidence below)

Sched shim lives in `drivers/gpu/drm/drm_sched.o`, linked into drm
`built-in.o` and `vmlinux`:

- `aarch64-nm drm_sched.o` →
  `T drm_sched_entity_fini`, `T drm_sched_entity_wait_idle`,
  `T drm_sched_fini`, each with `__kstrtab_`/`__ksymtab_` entries.
  `T` in `vmlinux` (`ffffffc00056d128` fini / `ffffffc00056c944`
  wait_idle / `ffffffc00056cb68` sched_fini).
- Fence retirement premise (F-JT1): 0004 shim assigns
  `job->fence = run_fence` from `run_job`'s return on the async worker
  (`drm_sched.c:460/479`); SUBMIT-time `sched_job->fence` is still
  NULL — the int9-class NULL-install site is present verbatim in the
  applied `panfrost7580_submit.c:609-628` (handle validated via
  `drm_syncobj_find`, `-ENOENT` preserved).
- Bound-trace landing (1244082) in `panfrost7580_job.o` strings:
  `p7580_job: dispatch slot=%d jc=0x%llx cfg=0x%08x as=%u aff=0x%llx seq=%u`,
  `p7580_job: irq slot=%d stat=0x%08x js_status=0x%08x (%s) has_job=%d`,
  `p7580_job: fence signal slot=%d seq=%u sig_rc=%d`,
  `p7580_job: irq-err slot=%d stat=0x%08x js_status=0x%08x (%s)`.
  Seed `jd->trace_left = p7580_job_trace` (= `P7580_JOB_TRACE_DEFAULT`
  8) at `panfrost7580_job.c:231`; `out_sync` stash field +
  `trace_left` budget in `panfrost7580_job.h:52/93`.
- `aarch64-nm panfrost7580_gem.o` →
  `T p7580_gem_job_done_sync` (0x46c); also `T` in `vmlinux`
  (`ffffffc00057922c`).
- **Caveat — no caller of `p7580_gem_job_done_sync` in this build:**
  `grep -rn job_done_sync drivers/gpu/drm/panfrost/*.c` hits only the
  gem definition + header decl/comment. The jobtrace report (§3.4/§7)
  describes a `bo_list` walk in `p7580_drv_free_job`, but the applied
  `free_job` (`submit.c:177-203`) has no such walk (and the report's
  own "four canon files" list covers jobs/gem + 0001 regen, not a new
  submit.c hunk for this ticket). So the invalidate helper compiles
  and links but never runs — coherency behavior is int9-identical
  until the hook lands. Likewise the "async run_job installs out_sync
  via replace_fence" design (§3.3): `run_job` here returns the fence
  to the 0004 shim only; no install call exists in-tree.
  Both are link-safe no-ops as built (no `U` left unresolved — build
  RC=0, no undefined-symbol failure), but Main should treat the F-JT1
  fix as *staged, not yet active*.
- Disassembled `p7580_gem_job_done_sync` (gem.o 0x46c) confirms the
  staging: with `CONFIG_HIGHMEM` unset on this arm64 tree,
  `is_highmem()` folds to 0, so the compiled body is only the
  backing/pages guard + `kmap_flushed = 1` store — no
  `flush_dcache_page` call emitted (no such `U` reloc; the kernel's
  `flush_dcache_page` here only clears `PG_dcache_clean` anyway).
  Harmless as built; on HIGHMEM hardware pages it would need the
  generic `flush_dcache_page` unconditionally or the hook + caller.
- Prior fixes intact: teardown_hold/release/unmap_bo `T` in submit.o
  (hold `ffffffc000577e40` in vmlinux), dark-timeout always-signal leg
  (`dma_fence_signal` in timedout path), render-only
  (`DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`), 18
  `__pm_runtime_resume/suspend` relocs (job-lifetime PM), shmem
  (`U shmem_file_setup`, `U shmem_read_mapping_page_gfp`),
  syncobj (`T panfrost_syncobj_create/destroy/ioctl/…` in syncobj.o).

## 4. Artifacts (sha256)

On the external disk (`…/j7k/gpu31/`):

- Image: `7f07e0fffb483653c1a05d51aa4cf96c495e2bfebcb796abe6a3d0e7950b9dbd`
  (`Image-int10`, 18160760 B, 2026-11-20 ~10:07 UTC; boot kernel bytes
  identical size/prefix to int9's 18160760, content differs by the
  jobtrace hunks)
- boot.img: `6ec626fe9d7ddd13f39b3903cb62e75f7b3a67987a81114566b248586c66a`
  (`kernel-boot-int10.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 5. Commits (this ticket)

- int10 script: `code/gpu31/series/build-int10.sh` (new) plus this
  report. No source/patch edits — 1244082 consumed as-is.
- Post-flash rerun + decision table: see `gpu31-jobtrace.md` §4/§6
  (trace lines now present in the binary; §4 expectations unchanged,
  with the §3 caveat above: out_sync still waits on the shim-assigned
  fence, completion invalidate staged-but-unhooked).
