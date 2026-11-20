# gpu31 integration8 — teardown PM guards build (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator8. Phone: kernel #37 (int5+mmap2),
renderD128, X healthy. No flash from this ticket — Main flashes.

Fixed series vs int7 (773cf27): backport 0001–0005 unchanged; Mode-B v2
render-only unchanged; CMA writecombine port unchanged; driver 0001 now
folds teardown PM guards (5c52f1f) on top of job-lifetime PM (c81e8a7),
shmem CREATE_BO (dcd5171), generic syncobj ioctls (2ad6d6e), fini decl
(78e37f8); driver 0002 adds syncobj.o. Step 4b stays retired.
No logic changes by this ticket beyond adding the int8 script.

## 1. Build

Script: `code/gpu31/series/build-int8.sh` (committed, copy of int7 with
scratch-int8/log-int8/boot-int8 names, 4b comment c81e8a7+5c52f1f).
Fresh `cp -a` of pristine tree → `scratch-int8`, all `--check` clean,
then `-j4` build. No build defects encountered (int7's hunk-count/fwd-decl
repairs already landed in driver 0001).
Log: `…/j7k/gpu31/logs/gpu31-int8-build.log` (5393 lines, tail
`SORTEX vmlinux / SYSMAP / OBJCOPY Image`, RC=0, 0 `error:` grep hits).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk (never rebuilt) → `/tmp/kernel-boot-int8.img`.

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

Base (`config-samsung-j7elte.aarch64`): `CONFIG_DMA_SHARED_BUFFER=y`,
`# CONFIG_DRM is not set`. Built `.config` adds exactly the fragment
symbols (all `=y`): `CONFIG_MODULES`, `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`,
`CONFIG_DRM_EXYNOS`, `CONFIG_DRM_EXYNOS_G2D`, `CONFIG_DRM_EXYNOS_DMABUF`,
`CONFIG_DRM_PANFROST7580` (plus auto `CONFIG_DRM_GEM_CMA_HELPER=y`).
`grep .config`: `CONFIG_MODULES=y`, `CONFIG_DRM=y`,
`CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DRM_EXYNOS=y`, `CONFIG_DRM_EXYNOS_DMABUF=y`,
`CONFIG_DRM_EXYNOS_G2D=y`, `CONFIG_DRM_PANFROST7580=y`,
`CONFIG_DMA_SHARED_BUFFER=y`. No `DRM_EXYNOS7580`, no `FRAMEBUFFER_CONSOLE`.
Vendor fbdev intact (`EXYNOS_DECON_FB=y`, `MALI_MIDGARD=y`).

## 3. Symbol proof (scratch-int8 objects, aarch64)

Driver lives at `drivers/gpu/drm/panfrost/` (9 panfrost7580/pf7580 `.o`,
incl. `panfrost7580_syncobj.o` built and linked).

- teardown_hold/release/unmap_bo in submit.o:
  `aarch64-nm panfrost7580_submit.o` →
  `T p7580_drv_pm_get`, `T p7580_drv_pm_put`,
  `T p7580_drv_teardown_hold` (0x260, calls `pm_get`),
  `T p7580_drv_teardown_release`, `T p7580_drv_unmap_bo`;
  all five also `T` in `built-in.o` and `vmlinux`
  (`ffffffffc000577c80/…ca0/…ce0`).
- pm_runtime_get_sync call sites (objdump `-dr`, `__pm_runtime_resume`
  reloc shown; `pm_runtime_get_sync` is an inline wrapper):
  - `p7580_drv_pm_get+0xa4`: `bl __pm_runtime_resume`
    (the single get primitive; every path funnels here).
  - `p7580_drv_file_fini+0x554`: `bl p7580_drv_pm_get` — inner
    PM hold across the entity walk (`file_fini` source:
    `if (drv && p7580_drv_teardown_hold(drv))`).
  - `p7580_drv_postclose+0x368/0x38c` in main.o:
    `bl p7580_drv_teardown_hold` … `bl p7580_drv_teardown_release`
    across `file_fini`+`kfree` (fail-closed: dark → fini+kfree anyway).
  - `p7580_drv_remove+0x4a0/0x610` in main.o:
    `bl p7580_drv_teardown_hold` … per-BO
    `bl p7580_drv_unmap_bo` … `bl p7580_drv_teardown_release`.
    `submit`/`run_job`/`timedout_job`/`free_job` keep their c81e8a7
    `__pm_runtime_resume/suspend` relocs (job-lifetime ref intact).
- render-only: `panfrost7580_main.c:283`
  `.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`
  (no card0 from panfrost7580; same Mode-B v2 core as int6/7).
- shmem: `p7580_shmem_free` (`t` in gem.o), `U shmem_file_setup`,
  `U shmem_read_mapping_page_gfp` in gem.o (16 MiB tiler-heap path;
  CMA kept for WC scanout only).
- syncobj: `panfrost_syncobj_ioctl` (`T` in syncobj.o + built-in.o);
  main.o fops `.unlocked_ioctl/.compat_ioctl = panfrost_syncobj_ioctl[/compat]`
  (`U` in main.o, resolved at link).

## 4. Artifacts (sha256)

- Image: `169a8090a52823e94960f10c485243bd1353af32f6cd72defa2a083b05a23ef7`
  (`scratch-int8/arch/arm64/boot/Image`, 18160760 B, 2026-11-20 02:52 EST)
- boot.img: `81ac6abcb16d40f34ec2ad84d828c672b89d583c7a53e38d063eb19d8b29176d`
  (`/tmp/kernel-boot-int8.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 5. Commits (this ticket)

- int8 script commit (this ticket): `code/gpu31/series/build-int8.sh`
  new (+ this report). No driver/logic changes.
- Consumed as-is: 5c52f1f (teardown holds), 01609ae/6f547c4/f929726
  (submit-hw handoff), c81e8a7/dcd5171/2ad6d6e/78e37f8 (prior landings).
