# GPU31 drmcore — minimal 3.10 DRM-core compat for panfrost v5.2 bring-up

Date: 2026-11-19. Slice: G31_DrmCore. Target: `code/gpu31/drmcore/` (NEW
files only — zero hunks to the shared tree, no Makefile/Kconfig wiring).
Device at check time (live, read-only, short flock-guarded SSH):
`Linux nura-j7elte 3.10.108 #35 SMP PREEMPT aarch64`; `/dev/dri` absent;
`/dev/mali0` + `/dev/fb0` present; `Mali-T72x MP2 r1p0 0x0720`.
No reboot, no flash, no kernel build (Integrator / M1Verify own those).

Related: `docs/reports/gpu31-target.md` (G31_PanfrostTarget dependency
table, v5.2 = `14817897`), `docs/reports/gpu31-review.md` (G31_Review
verdicts), `docs/reports/gpu-drm.md` (M1–M3), `patches/drm-backport/`
(fence→resv→syncobj→sched shims, owned by G31_Sched).

## Scope (narrowed by peer agreement)

G31_PanfrostTarget confirmed **v5.2** and needs exactly four things from
this slice — delivered as two headers + one compile-test consumer:

1. Platform registration adaptation: use 3.10 `drm_platform_init()` /
   `drm_platform_exit()` (legacy attach; no `drm_dev_alloc` /
   `drm_dev_register` backport — 3.10 embeds `struct drm_device` in
   `drm_fill_in_dev` + `drm_get_minor` inside `drm_get_platform_dev`).
2. Legacy `DRM_IOCTL_DEF_DRV` rows for the 6 v5.2 ioctls
   (SUBMIT/WAIT_BO/CREATE_BO/MMAP_BO/GET_PARAM/GET_BO_OFFSET).
3. `DEFINE_DRM_GEM_FOPS` equivalent + CMA mmap/vm_ops glue
   (present old-shape `drm_gem_mmap` / `drm_gem_create_mmap_offset`
   reused; no `drm_gem_shmem_*` port — G31_Gem owns BO backing via the
   present 272-line CMA helper pattern).
4. Local `readl_poll_timeout` helper (3.10 has `readl()` but no
   `linux/iopoll.h`; M0 `pf_poll` uses the same udelay-loop shape).

Explicitly NOT ported (per target confirmation): gem_shmem, atomic
helpers (render-only, unneeded), `dev_pm_domain_attach` / `device_link`
(deleted; vendor `pm_domains.c` notifier covers pd-g3d).

Zero-overlap confirmations (IRC): G31_Gem owns `panfrost_gem.*` + its
Kconfig/Makefile hunk (my slice creates no `drm_gem_shmem*` files and
touches no `drm_stub`/`drm_platform`/`drm_drv`); G31_Sched owns
`patches/drm-backport/0001-0005` (my series-dependency note stays
valid); G31_Kms is sole writer to the 5 Mode-B core files (my patches
are new-files-only: no `drmP.h` / `drm_platform.c` / `drm_stub.c` /
`drm_fops.c` / `drm_drv.c` hunks and no Makefile/Kconfig touches —
headers are `#include`d by driver sources, hence no build wiring).

## Delivered files

| File | Purpose |
|---|---|
| `code/gpu31/drmcore/drm_compat_310.h` | `readl_poll_timeout` (mainline subset semantics), `DEFINE_DRM_GEM_FOPS` / `_COMPAT` (3.10 `drm_open`/`drm_release`/`drm_ioctl`/`drm_gem_cma_mmap`/`drm_poll`/`drm_read`/`noop_llseek`, `CONFIG_COMPAT`-guarded compat entry), `DRM_RENDER_ALLOW → DRM_UNLOCKED` mapping (SUBMIT keeps `DRM_AUTH`; render-minor enforcement lives in G31_Kms Mode-B core), platform-registration reminder comment |
| `code/gpu31/drmcore/panfrost_ioctls_310.h` | 6 v5.2 ioctl numbers (0x00–0x05, `DRM_COMMAND_BASE`-relative) + UAPI command macros verified byte-identical to v5.2 `panfrost_drm.h`, minimal field-identical struct mirrors (to be dropped in favour of G31_Gem's full UAPI mirror at land time), `PANFROST_IOCTLS_310` legacy row constructors with v5.2 flags |
| `code/gpu31/drmcore/drm_compat_310_test.c` | Compile-only consumer: stub handlers with `drm_ioctl_t` signatures, `PANFROST_IOCTLS_310` table, CMA fops instantiation, `readl_poll_timeout` + `drm_platform_init/exit` + `num_ioctls` touch. Host-side check only, never in the kernel build |
| `code/gpu31/drmcore/compile.log` | Full `-fsyntax-only` + `-c` log (this report's evidence verbatim) |

## Compile evidence (acceptance)

All against the real 3.10.108 headers (`-nostdinc`, `arch/arm64/include`
+ `include`, `__KERNEL__`, kconfig include), `aarch64-linux-gnu-gcc 16.2.0`:

- `-fsyntax-only drm_compat_310.h` — **PASS, zero warnings**
- `-fsyntax-only panfrost_ioctls_310.h` — **PASS, zero warnings**
- `-fsyntax-only drm_compat_310_test.c` — **PASS, zero warnings**
- `-c drm_compat_310_test.c → .o` (4680 B) — **PASS, zero warnings**

## Present-verified, NOT ported (with tree evidence)

- `drm_mm` incl. `color_adjust` (`include/drm/drm_mm.h:79`), `drivers/iommu/iova.c` — IOVA window substrate for the driver.
- GEM CMA helper: `drivers/gpu/drm/drm_gem_cma_helper.c` (272 lines), `drm_gem_cma_create/mmap/vm_ops` (`EXPORT_SYMBOL_GPL`); GEM mmap-offset: `drm_gem_create_mmap_offset` / `drm_gem_mmap` (`EXPORT_SYMBOL`, `drm_gem.c`).
- dma-buf prime import/export (`drm_prime.c`, 11 `EXPORT_SYMBOL`s incl. `drm_gem_prime_export/import`, `prime_fd_to_handle/handle_to_fd`).
- Render-minor plumbing: `DRM_MINOR_RENDER=3` (`drmP.h:986`), `renderD%d` sysfs (`drm_sysfs.c:513`), `drm_get_minor`/`drm_put_minor`/`drm_platform_init` (`EXPORT_SYMBOL` in `drm_stub.c:393,425` / `drm_platform.c:194`).
- `DRM_IOCTL_DEF_DRV` legacy constructor (`drmP.h`), `DRM_AUTH/MASTER/ROOT_ONLY/CONTROL_ALLOW/UNLOCKED` gates, `drm_ioctl` dispatch (`drm_drv.c:373-411`, `DRM_COMMAND_END=0xA0`), exynos precedent (`exynos_drm_drv.c:221-260,290-310`).
- `devfreq` + old OPP (`CONFIG_PM_DEVFREQ=y` path, `opp_find_freq_*`), vendor pm-domains notifier (`pm_domains.c:413,431`).
- Absent as claimed: `linux/iopoll.h`, `linux/dma-fence.h`, `linux/dma-resv.h`, `drm_syncobj.c`, scheduler dir, `*shmem*` helpers, atomic helpers, `drm_dev_alloc` (all fence/resv/syncobj/sched gaps covered by G31_Sched's series).

## Review disposition (`gpu31-review.md`, commit 625ed18)

- **No finding names a `code/gpu31/drmcore/` file**: §§1–4 (fence F1–F3,
  resv F1–F2, syncobj F1, sched F1–F5) target G31_Sched's
  `patches/drm-backport/` series; §§6–8 (iovmm prototypes, clk_put
  coverage, IRQ count, LVDS-vs-DSI, single-compatible DT, `ARCH_EXYNOS`
  + 32 MB CMA, drm-m1-vs-swap path choice) target the display-swap path;
  §§10–12 (PRIME import-by-copy, kbase stride-64, Mesa `n_slots`) target
  G31_DmaBuf / G31_KbaseShim / G31_Mesa. Nothing in §§1–18 requires an
  edit to `drm_compat_310.h` / `panfrost_ioctls_310.h` /
  `drm_compat_310_test.c` — recorded here explicitly per Main's order.
- Review-driven verifications completed inside this slice (integrator
  re-checks at build time):
  - `ARCH_EXYNOS` is the real 3.10 arm64 symbol (`arch/arm64/Kconfig:191`).
  - `iovmm_map(dev, sg, offset, size, direction, id)` 6-arg signature
    confirmed (`include/linux/exynos_iovmm.h`); `iovmm_activate/deactivate`
    present.
  - DT: decon node has 3 interrupts (`exynos7580.dtsi`, fifo/cmd/frame_done)
    backing `platform_get_irq(pdev, 2)`; `dsim_0: dsim@14800000` +
    `decon_fb@14830000` nodes carry **no** `compatible` in the dtsi (vendor
    match comes from the board dts), so the 0002 patch's added single
    `compatible = "samsung,exynos7580-…"` per node satisfies the §7
    single-compatible rule as written (integrator confirms no second string
    at apply time).
  - LVDS-vs-DSI wart is documented in the driver source itself (no
    `DRM_MODE_CONNECTOR_DSI` in this tree's `drm_mode.h` — wart retained
    deliberately, §6 F4).
  - `devm_clk_get` exists in-tree (exynos fimd/gsc precedent); bare-`clk_get`
    without `clk_put` in the display driver (§6 F2) belongs to that slice's
    owner, not this one — flagged here, not fixed here.
  - `patches/drm-m1` (node-only) vs §§6–8 (swap path) are alternatives, not
    complements (§9) — this slice is path-neutral (works under either; the
    render minor comes from G31_Kms Mode B).
- Handoff notes for the integrator: apply order — `patches/drm-backport/`
  0001–0005 (G31_Sched, after its F-fixes) → Mode-B core (G31_Kms) →
  drmcore headers consumed by the panfrost driver sources (G31_Gem /
  G31_PanfrostTarget; `#include "drm_compat_310.h"` via `-I
  code/gpu31/drmcore`, no build wiring needed). GET_PARAM/GET_BO_OFFSET
  need no fence/sched support and come up first.

## What is NOT done (honest gaps)

- No full kernel `Image` link, no boot test, no GPU-read test (Integrator /
  M1Verify own build/flash; Main's go required; no reboot performed here).
- No panfrost GEM/job-slot driver code (G31_Gem / G31_PanfrostTarget own
  it); SUBMIT needs the G31_Sched chain landed + its review F-fixes.
- `drm_compat_310_test.o` is a host-side artifact only (kept out of the
  repo; log records its size).
