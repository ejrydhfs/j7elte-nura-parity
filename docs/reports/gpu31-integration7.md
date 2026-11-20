# gpu31 integration7 — shmem CREATE_BO + job-lifetime PM + syncobj ioctls (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator7. Phone: kernel #37 (int5+mmap2),
renderD128, X healthy. No flash from this ticket — Main flashes.

Fixed series vs int6 (0c4cacd): backport 0001–0005 unchanged; Mode-B v2
render-only unchanged; CMA writecombine port unchanged; driver 0001 now
folds shmem-backed CREATE_BO (dcd5171), job-lifetime PM model (c81e8a7),
generic syncobj ioctls (2ad6d6e); driver 0002 adds syncobj.o.
Step 4b (PM-ordering overlay) retired: c81e8a7 folded it into driver 0001.
Syncobj (G31_SyncobjIoctl) NOT waited for — landed as 2ad6d6e, built in.

## 1. Build

Script: `code/gpu31/series/build-int7.sh` (committed). Fresh `cp -a` of
pristine tree → `scratch-int7`, `--check` clean, then `-j4` build.
Two build-blocking defects in driver 0001 repaired en route (minimal,
mechanical, both verify with `git apply --check` + `patch(1)` full apply):
- main.c new-file hunk header claimed 644, body 656 (syncobj commit):
  patch(1) truncated the file mid-platform-driver
  (`expected expression at end of input`). Fixed to 656.
- submit.c: `free_job` calls `p7580_drv_job_pm_release()` (static, defined
  later) → implicit-declaration + static-follows-non-static under -Werror.
  Added static forward decls for `p7580_drv_job_pm_arm/release` before the
  backend ops; header count 623→625. Header-only count claims for
  submit.c-623/devfreq.h-251 left as-is (trailing-newline artifacts;
  verified complete content after apply).
Log: `…/j7k/gpu31/logs/gpu31-int7-build.log` (5393 lines, tail
`SORTEX vmlinux / SYSMAP / OBJCOPY Image`, RC=0, 0 `error:` grep hits).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk (never rebuilt).

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig)

`grep .config`: `CONFIG_MODULES=y`, `CONFIG_DRM=y`,
`CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_GEM_CMA_HELPER=y`,
`CONFIG_DRM_EXYNOS=y`, `CONFIG_DRM_EXYNOS_DMABUF=y`,
`CONFIG_DRM_EXYNOS_G2D=y`, `CONFIG_DRM_PANFROST7580=y`,
`CONFIG_DMA_SHARED_BUFFER=y`. No `DRM_EXYNOS7580`, no `FRAMEBUFFER_CONSOLE`.
Vendor fbdev intact (`EXYNOS_DECON_FB=y`, `MALI_MIDGARD=y`).
Base→built diff (DRM-relevant): `CONFIG_DRM` n→y (+ helpers), EXYNOS
subdrivers on (G2D only per M1 rationale), `CONFIG_DRM_PANFROST7580=y`
new. Remainder is default-n closure noise.

## 3. Symbol proof (scratch-int7 objects/vmlinux, aarch64)

- Probe: `p7580_drv_probe` present (`t ffffffc0005775e0`), platform-driver
  `.probe`, name `panfrost7580`; `p7580_drm_driver` (`d ffffffc00111e650`).
- Render-only: `panfrost7580_main.c:265`
  `.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`
  (no card0 from panfrost7580; same Mode-B v2 core as int6).
- PM ops linked: `p7580_drv_pm_get/put` (`T` in submit.o + built-in.o);
  `__pm_runtime_resume/suspend` relocations (`R_AARCH64_CALL26`) in
  submit.o run_job/timedout_job/free-job paths; power.o references
  `pm_runtime_enable/__pm_runtime_resume/__pm_runtime_suspend/
  set_autosuspend_delay/use_autosuspend`; main.o `U p7580_drv_pm_get/put`.
- shmem path compiled: `p7580_shmem_free` (`t` in gem.o/built-in.o);
  `U shmem_file_setup`, `U shmem_read_mapping_page_gfp` in gem.o
  (16 MiB tiler-heap CREATE_BO path; CMA kept for WC scanout only).
- syncobj: `panfrost7580_syncobj.o` built and linked
  (12 panfrost objects); fops `.unlocked_ioctl/.compat_ioctl =
  panfrost_syncobj_ioctl[/compat]` in main.o (forwarded to drm_ioctl).

## 4. Artifacts (sha256)

- Image: `386d466581b66a5c1b045e280eb16f00ee26847c334ab06c74e3937750610bc4`
  (`scratch-int7/arch/arm64/boot/Image`, 18160760 B, 2026-11-20 02:34 EST)
- boot.img: `9369de7addc1af0eeaf8fa29016217c58c836610fcc43d46ce527e699f50581d`
  (`/tmp/kernel-boot-int7.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 5. Commits (this ticket)

- `aea33e4` amended → `7b63ff9`/`15dad0a` lineage:
  `gpu31-integration7: int7 script + fix main.c hunk count + submit.c fwd decls`
  (build-int7.sh new + 0001 header/decl repairs).
- Sibling landings consumed as-is: 78e37f8 (fini decl), 2ad6d6e (syncobj).
  No logic changes by this ticket beyond the two mechanical build fixes.
