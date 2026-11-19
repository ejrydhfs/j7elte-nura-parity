# gpu31 integration — candidate builds (live)

Owner: G31_Integrator. Scratch: external-drive
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/scratch`
(copy of the shared tree; shared tree NEVER written). Build = `code/kernel/build.sh`
semantics: pmaports base config + `*.patch`, `oldconfig`, `Image` with
`KCFLAGS="-Wno-error"`, pack with `/tmp/mkboot.py` + stock `/tmp/bimg/dt.img`.
Fallback `/tmp/bindboot.img` untouched. Phone IS UP per Main 2026-11-19;
flash gate: ONLY G31_M1Verify flashes, only on Main's explicit go. No reboot
by integrator.

## Candidate 1 — 2026-11-19 (M1 render-node path + M3 fence/sched foundation)

Series (`code/gpu31/series/`, 3 peer sources):
1. `patches/drm-m1/m1-kconfig.fragment` (GpuDrm2 slice) — CONFIG_MODULES=y,
   CONFIG_DRM=y, KMS_HELPER, EXYNOS, EXYNOS_G2D, EXYNOS_DMABUF.
2. `patches/drm-backport/000[1-5]` (M3 foundation slice) — new files only:
   dma-fence, dma-resv, drm_syncobj, drm_sched + build wiring. Applied first.
3. `patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch` (display-driver
   slice, CODE ONLY — 0002 DT patch deliberately NOT applied; stock dt.img
   kept, vendor EXYNOS_DECON_FB stays master). Compiled but NOT enabled
   (`# CONFIG_DRM_EXYNOS7580 is not set`), so no double-drive risk.

Config diff vs pmaports base (after oldconfig): `CONFIG_MODULES=n→y`,
`CONFIG_DRM` unset→`y + KMS_HELPER + EXYNOS + EXYNOS_DMABUF + EXYNOS_G2D`;
`DRM_EXYNOS7580 is not set`; CMA untouched (0 MB); fbdev stack untouched.
Vendor fbdev path intact by construction.

Build: RC=0 (`-j<n>`, TMPDIR on external drive — host /tmp tmpfs is full).
Image `arch/arm64/boot/Image` 18110712 B,
sha256 `8a7d0452d2de4ce1dd0453d5aa7243996ec48a5a96a68e9814e9a16144bef328`.
Symbol proof (in scratch objects/vmlinux): `dma_fence_add_callback`,
`dma_fence_context_alloc`, `drm_sched_init/job_submit/job_alloc`,
`drm_syncobj_create`, `exynos_drm_platform_probe` present; vmlinux grep count
109 for dma_fence|drm_sched|drm_syncobj|dma_resv. Note: strict `-Werror`
(without KCFLAGS) still fails on pre-existing GCC-16 stock warnings
(aes-glue-ce, kernel/cpu.c, setup.c) — same as baseline, not from peer patches.
Candidate packed: `/tmp/kernel-boot.img` 27250688 B,
sha256 `cf25ed70948963ddbe8c3643800d723bd988d2e5fce16895b02cc51cb68be726`
(stock dt.img + ramdisk-new.gz + cmdline reused).

Status: candidate ready for G31_M1Verify on Main's go. Expected: /dev/dri/card0
+ controlD64, zero connectors, /dev/fb0 + fbdev X untouched.
Rejected this round: `patches/drm/0002` DT patch (stock dt.img rule);
`patches/drm/config-drm-exynos7580.fragment` (would swap display master —
deferred to M2 with coexistence design); any slice editing the shared tree
(none received — all consumed as files under `patches/`).
