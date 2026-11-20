# gpu31 integration6 — full candidate with PM-ordering step (render-only + mmap/mmap2 + stepped poll)

Date: 2026-11-20. Owner: G31_Integrator6. Phone: kernel #37 (int5+mmap2),
renderD128, X healthy, no flash from this ticket — Main flashes. You build,
Main flashes.

Fixed series = int5 series + post-int5 driver deltas + PM-ordering step:

| # | Source file (repo, committed) | Target | Notes vs int5 (d6f018c) |
|---|---|---|---|
| 1–5 | `patches/drm-backport/000[1-5]-*.patch` | shims | unchanged (f698920; shas c109abac…/9d0e58ee…/d0282eea…/7722a670…/68a6756f…) |
| 6 | `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch` | render-only core | unchanged (02bd790 Mode-B v2, sha f815228c…) |
| 7 | `code/gpu31/dmabuf/0001-drm-gem-cma-use-dma-attrs-writecombine.patch` | CMA port | unchanged (REQUIRED, sha 75a287ea…) |
| 8 | `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | panfrost7580 | f399e8ce… (was dbea… at int5): folds gem_vm_ops mmap (5d97c65), vm_pgoff fix (5f8ecd2), submit-hw stepped poll + wait_ready (645e511), mmu hunk-count repair (f3717b8) |
| 9 | `code/gpu31/driver/0002-panfrost7580-build-wiring.patch` | build wiring | unchanged (12bdaec, sha a4d9c2ae…) |

PM-ordering patch (`code/gpu31/pm-order/*.patch`, G31_PmOrderFix deliverable):
NOT LANDED at build time. `build-int6.sh` step 4b re-checked the path 6×
with 60 s sleeps (~6 min), then proceeded per the assignment: step 4b is a
logged no-op (`int6: no code/gpu31/pm-order/*.patch present;
driver-snapshot PM guards stand`). The driver snapshot already carries
pm_runtime get/put guards on the submit path (evidence §4). If the sibling
lands later, rerun this script — step 4b picks the patches up automatically.

## 1. Build

Script: `code/gpu31/series/build-int6.sh` (committed herewith; copy of
int5.sh with step 4b + REPO-root fix: int5's
`REPO=.../$(dirname $0)/../..` resolves to `code/` when run from
`code/gpu31/series/`; int6 uses `../../..` = repo root).
Scratch: `…/j7k/gpu31/scratch-int6` (fresh `cp -a` of pristine + 4× series,
`--check` clean incl. throwaway-copy full-series pre-verify S1–S4 OK).
Log: `…/j7k/gpu31/logs/gpu31-int6-build.log` (5392 lines, tail
`LD vmlinux / SYSMAP / OBJCOPY Image`, RC=0). Command:
`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j4 Image`.
The 6 `error` grep hits are benign (KERN_ERR macro defs, scsi_error.o
compile lines, strncpy warnings — no `error:` build failures).

Build-blocking repair folded mid-ticket (mechanical, keeps series green):
commit 645e511's stepped-poll edit added `udelay` use to the inner mmu.c
new-file content of driver/0001 without the inner `#include <linux/delay.h>`
and overflowed the outer hunk header (`+1,440` vs 446 true lines), so
git-apply truncated mmu.c mid-`fault_atype` (`udelay` implicit + `end of
input`). Fixed as hunk header `+1,447` + inner delay.h include; converged
with G31_SubmitHW's f3717b8 (identical content, tree clean against it).
Coordinated with G31_SubmitHW (owner) and G31_GemShmem (GEM/ioctl hunks
untouched) by IRC.

## 2. Config (pmaports base + d0-m1-minimal + d4-panfrost7580, olddefconfig closure)

Closure result (`grep .config`): `CONFIG_MODULES=y`, `CONFIG_DRM=y`,
`CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_GEM_CMA_HELPER=y` (auto-select),
`CONFIG_DRM_EXYNOS=y`, `CONFIG_DRM_EXYNOS_DMABUF=y`,
`CONFIG_DRM_EXYNOS_G2D=y`, `CONFIG_DRM_PANFROST7580=y`,
`CONFIG_DMA_SHARED_BUFFER=y`. Same shape as int5 (d0+d4 only delta is the
driver symbol; full `diff base .config` = 54 lines, remainder default-n
closure noise, no `DRM_EXYNOS7580`, no `FRAMEBUFFER_CONSOLE`).
No `DRM_EXYNOS7580`, no `DRM_KMS_CMA_HELPER`, no `FRAMEBUFFER_CONSOLE`.
Vendor fbdev intact: `EXYNOS_DECON_FB=y`, `MALI_MIDGARD=y` (kbase).

Full diff available in scratch: `…/scratch-int6/.config` vs
`code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`.

## 3. Symbol proof (scratch-int6 vmlinux/System.map, aarch64)

- Probe: `p7580_drv_probe` present (`t ffffffc000577548`, platform-driver
  `.probe`, name `panfrost7580`).
- Core attach: `drm_get_platform_dev` `T ffffffc00055bf0c`.
- Render-only: `drm_get_minor`/`drm_put_minor` both `T`;
  `drm_platform.o` disassembly: `tbnz w0,#15,1d0` (bit 15 =
  `DRIVER_RENDER_ONLY 0x8000`) → render path, legacy `w2=#0x1`
  (`DRM_MINOR_LEGACY`) only in the non-render-only fallthrough — no
  card0 from panfrost7580.
- PM ops linked: `p7580_drv_runtime_resume/suspend` (t),
  `p7580_drv_pm_resume/suspend` (t), `pf7580_pm_suspend/resume` (T),
  `pf7580_power_probe/remove` (T), `p7580_drv_pm_ops` (r).
- PM guard on submit path (visible): `panfrost_submit.o` relocation
  evidence — `panfrost_ioctl_submit_override` (+0xc0c) calls
  `__pm_runtime_resume` at +0xd34, then `p7580_drv_submit` at +0xd58,
  then `__pm_runtime_suspend` at +0xd80 (objdump -dr `R_AARCH64_CALL26`).
  `nm` confirms: `p7580_drv_submit` T, `panfrost_ioctl_submit_override` T,
  `U __pm_runtime_resume/suspend` in submit.o; power.o `U pm_runtime_enable`,
  `pm_runtime_set_autosuspend_delay`, etc.
- MMU/map path: `p7580_mmu_invalidate` T, `p7580_mmu_fault_code/atype` T,
  `p7580_gem_mmap` T, `drm_gem_mmap` T (+ksymtab).
- No kbase/decon regression: 560 `mali_kbase|kbase_` matches (T symbols +
  initcall), decon linked, dt.img untouched.

## 4. Image + sha256

- Image: `…/scratch-int6/arch/arm64/boot/Image`, 18144376 B,
  `2d9c61d87b09a24e1c9009c680240b8d86e1778f66a03bd1a42d4d0f8bd680a2`.
- Boot image: `/tmp/kernel-boot-int6.img` (27283456 B = kernel 18144376 +
  ramdisk 8822392 + dt 313344; ramdisk `ac0b6f8b…`, dt `352bda9b…` — both
  stock reference),
  `674e46ecfd28fbc41ccc3d7b8d6ec14f809f9c19e2c55b0c5f1021c57efb141e`.
- Pack rule honoured: dt.img NEVER rebuilt (stock bytes reused;
  sha `352bda9bcf02bce50c9fc6ad6ca845727715fe1942fe25604d67f94e69d8f937`).

## 5. Flash request (QUEUED — DO NOT FLASH from this ticket)

Request for Main: flash `/tmp/kernel-boot-int6.img`
(`674e46ec…fb141e`) at Main's discretion via the normal
G31_M1Verify-gated path, with `/tmp/bindboot.img`
(`fcec651e…f038`) as fallback. Expected on device:
`/dev/dri/renderD128` ONLY from panfrost7580 (no panfrost card0),
vendor `/dev/fb0` + fbdev X untouched, kbase `/dev/mali0` present,
NULL-job submit under wdarm-60 per G31_SubmitHW's standing plan.
This ticket performs no flash, no reboot, no device contact.

## 6. Commit

`code/gpu31/series/build-int6.sh` committed herewith (REPO-root fix +
4b PM-ordering step). Driver 0001 repair converged into f3717b8
(G31_SubmitHW). Report: `docs/reports/gpu31-integration6.md` (this file).
