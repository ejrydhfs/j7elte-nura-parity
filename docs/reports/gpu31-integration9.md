# gpu31 integration9 — close-fix build (render-only + d0+d4)

Date: 2026-11-20. Owner: G31_Integrator9. Phone: kernel #37 (int5+mmap2),
renderD128, X healthy. No flash from this ticket — Main flashes.

Fixed series vs int8 (a5554cf): backport 0001–0005 unchanged in content but
now include the close fix (476b75d): 0001 gains the NULL-fence
`remove_callback` guard, 0003 documents non-blocking `syncobj_manager_fini`,
0004 rewrites the sched shim (bounded idempotent `entity_fini` via
`wait_idle` = one timeout + 8 s margin, `push` re-arms `entity_idle` on
0→1, `sched_fini` stops workers before draining, NULL-safe
`remove_callback`); Mode-B v2 render-only unchanged; CMA writecombine port
unchanged; driver 0001 adds the dark-timeout always-signal leg (timedout_job
dark early-return now signals the job fence before returning) plus teardown
close-ordering docs on top of teardown PM guards (5c52f1f) / job-lifetime PM
(c81e8a7) / shmem CREATE_BO (dcd5171) / generic syncobj ioctls (2ad6d6e) /
fini decl (78e37f8); driver 0002 adds syncobj.o. Step 4b stays retired.
No logic changes by this ticket beyond adding the int9 script (copy of int8
with scratch-int9/log-int9/boot-int9 names, header cites 476b75d).

Repair found during this build (RELANDS into the tracked patch, not a local
overlay): driver 0001's `panfrost7580_submit.c` new-file hunk header read
`@@ -0,0 +1,691 @@` while the body is 718 `+` lines (close-fix hunk at
5685+10/+5695+24 grew the file; same `patch(1)` truncation class as the int7
`mmu.c 440→447` and `main.c` repairs at 7b63ff9). `git apply` silently
truncated the file at 691 lines (mid-`copy_from_user`, `panfrost_ioctl_submit_override`
unterminated → `expected expression at end of input`), so `-j4` failed at
`panfrost7580_submit.o`. Fixed the header to `+1,718` in the tracked patch;
re-`--check`ed all 9 patches in a clean clone (all PASS), re-seeded
scratch-int9, rebuilt. The KMS audit flags (`@@` counts 11/12, 15/16, 27/28,
15/16 in render-node-mode-b) are auditor false positives: those hunks carry
`\ No newline at end of file` markers which the counter treats as an extra
line; the patch `--check`s clean and `drm_drv.c` applies intact.

## 1. Build

Script: `code/gpu31/series/build-int9.sh` (committed, copy of int8 with
scratch-int9/log-int9/boot-int9 names, 4b comment c81e8a7+5c52f1f).
Fresh `cp -a` of pristine tree → `scratch-int9`, all 9 `--check` clean,
then `-j4` build. One defect found and repaired (hunk count, above; landed
in driver 0001 itself).
Log: `…/j7k/gpu31/logs/gpu31-int9-build.log` (5393 lines, tail
`LD net/built-in.o`, RC=0; the only `error`-grep hits are `KERN_ERR`
defines, the int9 `panfrost7580_submit.c` pre-repair failure lines, and one
`-Wunused-variable staged` warning — no post-repair errors).
Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
Pack: stock dt.img + reference ramdisk (never rebuilt) → `/tmp/kernel-boot-int9.img`.

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
Config diff int8→int9: IDENTICAL (sorted grep of both `.config`s, no diff).

## 3. Symbol proof (scratch-int9 objects, aarch64)

Sched shim lives in `drivers/gpu/drm/drm_sched.o`, linked into drm
`built-in.o` and `vmlinux`:

- `aarch64-nm drm_sched.o` →
  `T drm_sched_entity_fini` (0xb28), `T drm_sched_entity_wait_idle`
  (0x344), `T drm_sched_fini` (0x568), each with `__kstrtab_`/`__ksymtab_`
  entries (EXPORT_SYMBOL). All three also `T` in `vmlinux`
  (`ffffffc00056d128` fini / `ffffffc00056c944` wait_idle /
  `ffffffc00056cb68` sched_fini).
- Driver lives at `drivers/gpu/drm/panfrost/` (10 panfrost7580/pf7580 `.o`,
  incl. `panfrost7580_syncobj.o` built and linked).
- teardown_hold/release/unmap_bo in submit.o:
  `aarch64-nm panfrost7580_submit.o` →
  `T p7580_drv_pm_get`, `T p7580_drv_pm_put`,
  `T p7580_drv_teardown_hold`, `T p7580_drv_teardown_release`,
  `T p7580_drv_unmap_bo`; hold/release also `T` in `vmlinux`
  (`ffffffc000577e40/…e60`). `p7580_drv_timedout_job` is `t`
  (static) with `U p7580_job_timedout` resolved at link.
- dark-timeout always-signal leg (submit.c:167-171, the 476b75d hunk):
  `if (p7580_drv_pm_get(drv)) { f = job ? job->done_fence : NULL; if (f &&
  !dma_fence_is_signaled(f)) dma_fence_signal(f); return; }` — CPU-only
  signal, safe dark; retirement unconditional so `entity_fini→wait_idle`
  terminates.
- `aarch64-objdump -dr panfrost7580_submit.o`: 18
  `__pm_runtime_resume/suspend` relocs (job-lifetime PM model intact).
- render-only: `panfrost7580_main.c:283`
  `.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER_ONLY`
  (no card0 from panfrost7580; same Mode-B v2 core as int6/7/8).
- shmem: `p7580_shmem_free` (`t` in gem.o), `U shmem_file_setup`,
  `U shmem_read_mapping_page_gfp` in gem.o (16 MiB tiler-heap path;
  CMA kept for WC scanout only).
- syncobj: `panfrost_syncobj_create/destroy` + `panfrost_syncobj_compat_ioctl`
  (`T` in syncobj.o); main.o fops `.unlocked_ioctl/.compat_ioctl =
  panfrost_syncobj_ioctl[/compat]` (`U` in main.o, resolved at link);
  11 `p7580_drv_teardown|panfrost_syncobj` symbols in panfrost `built-in.o`.

## 4. Artifacts (sha256)

- Image: `900732bd2e5359b948833fc796e3762bba58bfb6e665a3e19e0f247d7fe8d2f8`
  (`scratch-int9/arch/arm64/boot/Image`, 18160760 B, 2026-11-20 04:50 EST)
- boot.img: `74d927decedbee37faed145574bf5585efefa16b9f2484665868ccca3f533dfe`
  (`/tmp/kernel-boot-int9.img`, 27299840 B = kernel 18160760 +
  ramdisk 8822392 + dt 313344)

## 5. Commits (this ticket)

- int9 script commit (this ticket): `code/gpu31/series/build-int9.sh`
  new (+ this report) plus the hunk-count repair inside driver 0001
  (`+1,691` → `+1,718` for `panfrost7580_submit.c`).
- Consumed as-is: 476b75d (close fix), 5c52f1f (teardown holds),
  01609ae/6f547c4/f929726 (submit-hw handoff),
  c81e8a7/dcd5171/2ad6d6e/78e37f8 (prior landings).
