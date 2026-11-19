# GPU31 driver — panfrost7580 render-minor-only platform driver

Date: 2026-11-19. Slice: `G31_DriverAssemble2`. Target:
`code/gpu31/driver/*.patch` (new dir) + this report
`docs/reports/gpu31-driver.md`. Device state per Main: phone UP on
kernel #37 (M1 DRM core built, fbdev X 720x1280, `/dev/mali0` present,
**no /dev/dri** — no driver bound). No reboot, no flash, no shared-tree
edits (read-only `-fsyntax-only`/`-c` checks in `/tmp` against the
pristine `universal7580-2a746c1` tree, headers staged from
`patches/drm-backport/`).

Related: `docs/reports/gpu31-{dt,drmcore,gem,iommu,jobs,power}.md`,
`docs/reports/gpu31-devfreq.md`, `docs/reports/gpu31-mesa.md`
(G31_Mesa userspace), `patches/drm-backport/` (0001 fence, 0002 resv,
0003 syncobj, 0004 sched), `code/gpu31/{drmcore,gem,jobs,mmu,iommu,
power,devfreq}/`, `code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch`
(Mode B), `code/gpu31/config/fragments/d4-panfrost7580.fragment`
(`CONFIG_DRM_PANFROST7580=y` on top of M1).

## 0. What this delivers

A self-consistent patch series that binds the **stock**
`mali@0x11400000` node (`arm,mali`, reg `0x11400000/0x5000`, IRQs
job/mmu/gpu SPI 282/283/281 index order 0/1/2, 6 clocks,
power-domain `0x60`) and registers a **render-minor-only** DRM device:
no `DRIVER_MODESET`, no CRTC/encoder/connector, no fbdev changes.
With the Mode-B core patch the same attach also yields `renderD128`
while vendor DECON keeps the panel and `/dev/fb0` + fbdev X stay
untouched. Six v5.2 ioctls, GEM, one GPU address space, 3 job slots,
power/clocks/regulator, devfreq DVFS. Kconfig `CONFIG_DRM_PANFROST7580`
tristate, default n.

| Patch | Contents |
|---|---|
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | 17 new files under `drivers/gpu/drm/panfrost/`: 3 new driver files (`_main.c`, `_submit.c`, `_drv.h`) + verbatim snapshots of the 14 live slice sources (GEM×5 incl. UAPI, jobs×3, MMU×2, power×2, devfreq×2) |
| `code/gpu31/driver/0002-panfrost7580-build-wiring.patch` | `drivers/gpu/drm/{Makefile,Kconfig}` one-line additions + new `drivers/gpu/drm/panfrost/{Kconfig,Makefile}` (`CONFIG_DRM_PANFROST7580`, `default n`, `depends on DRM && ARM64 && MMU`) |
| (int4 fix, folded 2026-11-19) | panfrost `ccflags-y` gains `-Idrivers/gpu/drm` so the 0003/0004 shim headers (`drm_syncobj.h`/`drm_sched.h`, local files in `drivers/gpu/drm/`, no stock `-I` covers that dir) resolve via quoted includes; `select DRM_GEM_CMA_HELPER` kept, requires the dmabuf slice's `0001-drm-gem-cma-use-dma-attrs-writecombine.patch` (stock `drm_gem_cma_helper.c` uses 32-bit-only `dma_alloc_writecombine`) |

Apply order (integrator): backport 0001–0005 → KMS Mode-B core →
this series → M1 fragment + D4 fragment.

## 1. Binding map (stock DT → driver)

| DT property | Value | Driver use (`panfrost7580_main.c`) |
|---|---|---|
| `compatible = "arm,mali"` | single string | `p7580_of_match[]` matches `arm,mali-t720` + `arm,mali`; probe then requires `mem.start == 0x11400000` so foreign `arm,mali` SoCs get `-ENODEV` |
| `reg <0 0x11400000 0 0x5000>` | 20 KiB G3D SFR | `platform_get_resource(IORESOURCE_MEM, 0)` + `devm_ioremap_resource`; size < 0x5000 → `-ENODEV` |
| `interrupts` index 0/1/2 | SPI 282/283/281 (JOB/MMU/GPU) | `platform_get_irq(pdev, 0/1/2)` → `irq_job/mmu/gpu`; names ignored (kbase ABI); `IRQF_SHARED` |
| `clocks` / `clock-names` | 6 stock names | `pf7580_power_probe` → `clk_get` ×6 in kbase order (power slice, unchanged) |
| `samsung,power-domain = <0x60>` | `&pd_g3d` | genpd-owned (vendor bus notifier); driver only does runtime-PM get/put (power slice, unchanged) |
| absent `mali-supply` / OPP / `iommus` | rail is always-on `vdd_g3d`, static C OPP table, GPU-internal MMU | `regulator_get(NULL, "vdd_g3d")`, static 8-row ladder, no sysmmu (all in power/devfreq/MMU slices, unchanged) |

## 2. Ioctl map (v5.2, byte-identical `panfrost_drm.h`)

| # | Ioctl | Handler | Slice path |
|---|---|---|---|
| 0x00 | `DRM_PANFROST_SUBMIT` | `panfrost_ioctl_submit_override` (table) → `p7580_drv_submit` (submit.c) | validate (`p7580_jc_valid` + caps + slot present) → stage user arrays (`copy_from_user`) → in-sync resolve (0003) → BO lookup + MMU-map AS 0 (GEM+MMU) → sched job + deps (0004) + submit → out-sync install (0003); runtime-PM get/put around it |
| 0x01 | `DRM_PANFROST_WAIT_BO` | `panfrost_ioctl_wait_bo` (GEM verbatim) | absolute→relative ns→jiffies → `p7580_gem_wait` → `dma_resv_wait_timeout` (0002) |
| 0x02 | `DRM_PANFROST_CREATE_BO` | `panfrost_ioctl_create_bo` (GEM verbatim) | `p7580_gem_create` CMA/WC + IOVA + `drm_gem_handle_create` |
| 0x03 | `DRM_PANFROST_MMAP_BO` | `panfrost_ioctl_mmap_bo` (GEM verbatim) | fake offset from `map_list.hash.key`; `mmap` → `p7580_gem_mmap` fault path |
| 0x04 | `DRM_PANFROST_GET_PARAM` | `panfrost_ioctl_get_param` (GEM verbatim) | `DRM_PANFROST_PARAM_GPU_PROD_ID` → `0x720`; rest `-EINVAL` |
| 0x05 | `DRM_PANFROST_GET_BO_OFFSET` | `panfrost_ioctl_get_bo_offset` (GEM verbatim) | per-BO `iova` (nonzero; T720 treats 0 as unmapped) |

Table flags: SUBMIT `DRM_AUTH | DRM_UNLOCKED`, rest `DRM_UNLOCKED`
(v5.2 `DRM_RENDER_ALLOW`; render-always-authenticated + DRM_MASTER-on-
render deny come from the Mode-B core patch, not this driver).

## 3. Subsystem map (who owns what)

- `panfrost7580_main.c` (NEW): OF match, probe/remove, IRQ demux
  (MMU/GPU; JOB demux stays in jobs), `drm_driver` (no MODESET,
  GEM/PRIME hooks, 6 ioctl rows), `dev_pm_ops` (delegates to
  `pf7580_{runtime,pm}_*` + `pf7580_dvfs_{suspend,resume}`), platform
  driver (`module_platform_driver`).
- `panfrost7580_submit.c` (NEW): SUBMIT dispatch joining GEM (lookup,
  resv, `p7580_bo_npages/page_phys`), MMU (`map/invalidate`), jobs
  (`p7580_jc_valid/slot/submit_args` shape, `run/timedout/free`
  backend), syncobj (0003), sched (0004: per-file entity per slot on
  one shared ordered queue), `bo_list` tracking for remove/unload.
- `panfrost7580_drv.h` (NEW): `struct p7580_drv` (starts with
  `struct p7580_dev gd` so GEM's `dev_private` helpers work) +
  `struct p7580_drv_file` (leads with GEM's `p7580_file_priv` layout
  so 5 GEM ioctls work unchanged).
- GEM/jobs/MMU/power/devfreq sources: **verbatim snapshots** of the
  live slices (no edits; see §6 for drift procedure).

## 4. Verification (acceptance evidence)

Against the pristine tree (`-nostdinc`, `arch/arm64/include` +
`include`, `__KERNEL__`, kconfig include), `aarch64-linux-gnu-gcc 16.2.0`:

- `git apply --check -v` 0001 + 0002 on pristine
  `drivers/gpu/drm/{Makefile,Kconfig}`: **PASS, all 19 + 4 hunks**.
- `-fsyntax-only -Wall -Wextra` all 8 `.c` (2 new + 6 slice): **PASS,
  zero errors** (only stock-tree `dma-debug.h` inline noise + one
  pre-existing sign-compare in submit.c).
- Real `-c` to `.o` all 8: **PASS, zero errors** (24–25 KiB objects).
- `ld -r` link of all 8: **PASS**; 134 undefined symbols, every one a
  verified in-tree `EXPORT_SYMBOL[_GPL]` (DRM/GEM/PRIME, clk,
  regulator, devres, PM runtime, shmem, devfreq/OPP, MMU) or a staged
  0001–0004 shim export (fence/resv/syncobj/sched).
- Post-fold re-check (int4 layout: `-I drivers/gpu/drm` + `-I drivers/gpu/drm/panfrost`, quoted shim includes): 8/8 `-c` **PASS**, `ld -r` **PASS**.

NOT verified here (integrator / M1Verify own): full `Image` build,
boot, `/dev/dri/renderD128` on device, Mesa triangle. No reboot/flash
performed. The D4 fragment (`CONFIG_DRM_PANFROST7580=y`) is staged for
the integrator's build window, not enabled here.

## 5. Gaps and follow-ups (honest)

1. **Slice drift**: `gem/` + `devfreq/` peers were mid-edit while this
   was assembled (jobs `p7580_jc_valid`/fence-alias fixes already
   consumed; GEM's `devm_*`/F-GEM review fixes consumed). Procedure:
   re-snapshot the 14 files, regenerate 0001, re-run the §4 checks.
   The 3 new files are drift-proof (they only use published slice
   entry points, all cited in §3).
2. **SUBMIT sync for imported BOs**: `p7580_drv_map_bo` walks
   CMA/SHMEM/imported pages via `p7580_bo_page_phys`; dma-buf
   coherency for concurrent importer writes follows the GEM slice's
   `dma_sync_*` discipline (review F-fixes there), not duplicated here.
3. **Fault recovery wiring**: `p7580_job_recover` re-arms slots + GPU
   reset; the MMU AS re-program between reset and re-arm is the
   integrator's call site (noted in the jobs header), invoked from
   `timedout_job` → already wired through our backend.
4. **AS count**: single AS 0 (all slots share). Multi-context AS
   numbering is a future slice, not this bring-up.
5. **Devfreq optional**: `pf7580_dvfs_init` failure degrades to
   clock-on operation (boot OPP) instead of failing probe — matches
   the DT report's degraded-but-alive rule.
6. **kbase coexistence**: this driver binds `mali@0x11400000`, which
   kbase also matches. Cutover is the integrator's Kconfig/DT call
   (`m0-mali-off.fragment` + D4); both must never be `=y` together.

## 6. Regeneration (integrator)

```sh
# re-snapshot slices, rebuild patches, re-verify (all read-only vs tree):
for f in gem/panfrost7580_gem.c gem/panfrost7580_gem.h \
         gem/panfrost7580_ioctl.c gem/panfrost7580_ioctl.h gem/panfrost_drm.h \
         jobs/panfrost7580_job.c jobs/panfrost7580_job.h jobs/panfrost7580_jm_hw.h \
         iommu/panfrost7580_mmu.c iommu/panfrost7580_mmu.h \
         power/panfrost7580_power.c power/panfrost7580_power.h \
         devfreq/pf7580_devfreq.c devfreq/pf7580_devfreq.h; do
  cp code/gpu31/$f /tmp/pkg/drv/$(basename $f)
done
sh /tmp/genp/gen.sh   # rewrites code/gpu31/driver/*.patch
# then: git apply --check + -fsyntax-only/-c/-r per §4
```
