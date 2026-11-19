# Panfrost backport inventory — 3.10.108 (Nura/exynos7580) vs panfrost v5.2

Scope: read-only audit of the Nura 3.10.108 tree
(`$K = .../android_kernel_samsung_universal7580-2a746c...`);
no kernel edits, no patches, no device access.
Prior art: `docs/reports/gpu-panfrost.md` (M0 + audit), `code/userspace/gpu/panfrost/`
(register/MMU core + M0 kmod). This file is the dependency-by-dependency
backport shopping list with file:line evidence.

## 1. Target version + file inventory

**Recommended target: panfrost at Linux v5.2** (`refs/tags/v5.2`,
`14817899749faa879ecbeda203c73138c4d65e79`). Rationale in §4.

v5.2 `drivers/gpu/drm/panfrost/` file list (fetched from
`raw.githubusercontent.com/torvalds/linux/v5.2/...`; `panfrost_ioctl.*` and
`panfrost_perfcnt.*` do not exist yet at v5.2 — `rm` confirmed empty fetch):

| File | LOC (`wc -l`) | Role |
|---|---|---|
| `panfrost_drv.c` | 475 | `drm_driver`, 6-ioctl table (§3.8), gem/shmem fops, probe/remove |
| `panfrost_device.c` / `.h` | 257 / 125 | probe, clk/regulator/pm-runtime attach, IRQ install |
| `panfrost_gpu.c` / `panfrost_gpu.h` | 367 / 19 | GPU_ID decode, SOFT_RESET + HARD_RESET fallback, PWRON/READY power-up (L2→shader→tiler) |
| `panfrost_mmu.c` / `panfrost_mmu.h` | 394 / 17 | `alloc_io_pgtable_ops(ARM_MALI_LPAE)`, AS_TRANSTAB/TRANSFG programming, fault IRQ |
| `panfrost_job.c` / `panfrost_job.h` | 564 / 51 | `drm_gpu_scheduler` backend, `dma_fence` job fences, syncobj in/out, JS slot submit |
| `panfrost_gem.c` / `panfrost_gem.h` | 96 / 30 | shmem GEM create/map wrappers (`drm_gem_shmem_*`) |
| `panfrost_devfreq.c` / `panfrost_devfreq.h` | 221 / 14 | devfreq + `dev_pm_opp` DVFS |
| `panfrost_regs.h` | 298 | register map (matches kbase bit-for-bit, cf. `gpu-panfrost.md` §3.1) |
| `Kconfig` / `Makefile` / `TODO` | 16 / 12 / 27 | build glue |
| `include/uapi/drm/panfrost_drm.h` | 142 | **UAPI to keep byte-identical** (see below) |
| **Total** | **3070** | |

v5.2 UAPI (`panfrost_drm.h:15-27,32-134`): exactly 6 ioctls —
`SUBMIT(0x00)`, `WAIT_BO(0x01)`, `CREATE_BO(0x02)`, `MMAP_BO(0x03)`,
`GET_PARAM(0x04)`, `GET_BO_OFFSET(0x05)`; structs `drm_panfrost_submit`,
`drm_panfrost_wait_bo`, `drm_panfrost_create_bo`, `drm_panfrost_mmap_bo`,
`drm_panfrost_get_param` (only `DRM_PANFROST_PARAM_GPU_PROD_ID`),
`drm_panfrost_get_bo_offset`. Driver table `panfrost_drv.c:329-340`
registers all six with `DRM_RENDER_ALLOW` (SUBMIT additionally `DRM_AUTH`).
**No `JM_CTX_CREATE`, no `MADVISE`, no `PERFCNT_*`** — those appear in
later kernels (≥5.7) and are deliberately excluded by choosing v5.2.

## 2. Dependency table (present / absent + evidence)

Paths relative to `$K` unless stated. "Absent" = file missing AND zero
symbol hits in `include/` + `drivers/` (checked 2026-11-18).

| # | Dependency (what v5.2 panfrost uses) | In 3.10? | Evidence |
|---|---|---|---|
| D1 | `drm_sched` / `drm_gpu_scheduler` (`panfrost_job.c:10` `#include <drm/gpu_scheduler.h>`, `sched` struct, `panfrost_job_run/dependency`) | **ABSENT** | `ls drivers/gpu/drm/scheduler` → no such dir; `grep -rl gpu_scheduler\|drm_sched drivers/gpu/drm/ include/drm/` → no hits outside radeon-internal `*_fence_ring_emit` names |
| D2 | `dma-fence` (`panfrost_job.h`: `struct dma_fence *{render_done,done,in,implicit}_fence*`; `panfrost_fence_ops`) | **ABSENT** | `ls include/linux/dma-fence*` → no such file; only false positives are `radeon:..._dma_fence_ring_emit` (`radeon_asic.c:963` etc.), not the framework |
| D3 | `dma-resv` / `reservation_object` (BO reservation locking under job submit) | **ABSENT** | `ls include/linux/{dma-resv,reservation}*` → no such file; `grep resv\|reservation drivers/base/dma-buf.c` → zero hits (`dma-buf.c` is 733 lines, pre-resv API) |
| D4 | `drm_syncobj` (SUBMIT `in_syncs/out_sync`; `panfrost_drv.c` `#include <drm/drm_syncobj.h>`) | **ABSENT** | `ls drivers/gpu/drm/drm_syncobj.c` → no such file; `grep -rn drm_syncobj include/drm/ drivers/gpu/drm/` → no hits |
| D5 | `drm_gem_shmem_helper` (`panfrost_gem.c`, `panfrost_mmu.c: sgt = drm_gem_shmem_get_pages_sgt(obj)`, `DEFINE_DRM_GEM_SHMEM_FOPS`) | **ABSENT** — but `drm_gem_cma_helper` **PRESENT** | shmem: `ls drivers/gpu/drm/*shmem* include/drm/*shmem*` → no such file. CMA present: `include/drm/drm_gem_cma_helper.h:4` (`struct drm_gem_cma_object`), `drivers/gpu/drm/drm_gem_cma_helper.c` 272 lines, exports `drm_gem_cma_create:48→:92`, `drm_gem_cma_free_object:139→:154`, `drm_gem_cma_dumb_create:163`, `drm_gem_cma_vm_ops:212`, `drm_gem_cma_mmap:221` |
| D6 | `io-pgtable` + `ARM_MALI_LPAE` (`panfrost_mmu.c`: `alloc_io_pgtable_ops`, `struct io_pgtable_cfg/ops`) | **ABSENT** (generic framework); GPU format knowledge **PRESENT in kbase** | `ls drivers/iommu/io-pgtable* include/linux/io-pgtable*` → no such file; `grep io-pgtable drivers/iommu/Makefile` → no hits. Format itself proven identical kbase↔mainline: `code/userspace/gpu/panfrost/mali_hw.h` + `mali_lpae_test.c` (114 checks) |
| D7 | ARM LPAE page-table substrate (arm64 4-level tables the encoder mimics) | **PRESENT** (arch-native, not a backport) | arm64 always-LPAE: `arch/arm64/mm/` (`pgd.c`, `mmu.c`, `proc.S` …); CPU tables work — M0 encoder self-test passes under `qemu-aarch64` (`gpu-panfrost.md` §6.1) |
| D8 | GEM prime helpers (`drm_gem_prime_export/import/handle_to_fd/fd_to_handle`) | **PRESENT** | `drivers/gpu/drm/drm_prime.c:148` (`drm_gem_prime_dmabuf_ops`), `:185` `drm_gem_prime_export`, `:254` `drm_gem_prime_import`, `:309` `drm_gem_prime_fd_to_handle`; all `EXPORT_SYMBOL` |
| D9 | GEM mmap-offset helpers (`drm_gem_create_mmap_offset/free_mmap_offset`, `drm_gem_mmap`, `gem_vm_ops`) | **PRESENT** (old shape) | `drivers/gpu/drm/drm_gem.c:658` `drm_gem_mmap` → `:717` `EXPORT_SYMBOL`; `:334`/`EXPORT` `drm_gem_create_mmap_offset`, `:309` `drm_gem_free_mmap_offset`; per-driver hook `include/drm/drmP.h:958` (`gem_vm_ops`); CMA vm_ops at `drm_gem_cma_helper.c:212`. **Absent** is only the newer layering: `grep gem_object_funcs\|gem_object_put_locked include/drm/ drivers/gpu/drm/drm_gem.c` → zero hits |
| D10 | `devfreq` + OPP (v5.2 `panfrost_devfreq.c`: `dev_pm_opp_get_opp_count/opp_put`, `devfreq` struct) | **PRESENT (old API)** | `CONFIG_PM_DEVFREQ=y` (`config-samsung-j7elte.aarch64:3265`); `drivers/devfreq/devfreq.c` + `#include <linux/opp.h:21>`, uses `opp_find_freq_ceil` (`devfreq.c:898`); old `include/linux/opp.h` + `drivers/base/power/opp.c:257` `opp_find_freq_exact`. No `dev_pm_opp_set_rate/clkname` modern helpers — port kbase ASV tables instead (cf. D15) |
| D11 | reset (`reset_control_*`) | **PRESENT (stub-ish), NOT NEEDED** | `include/linux/reset.h:5-8` declares `reset_control_reset/assert`; `drivers/reset/` = `Kconfig Makefile core.c` only. DT has no reset controller for mali; both kbase and panfrost reset via `GPU_CMD=SOFT_RESET` + poll `RESET_COMPLETED` (M0 implements this) → drop the reset-controller path |
| D12a | `dev_pm_domain_attach*` / `device_link_add` (modern genpd attach in panfrost_device.c path) | **ABSENT** — **by design, do not port** | `grep -rn dev_pm_domain_attach\|device_link_add $K --include=*.h --include=*.c` → zero hits (`device_link` hits in `drivers/base/class.c:551-589` are the unrelated sysfs-link `device_create(..., device_link)` parameter, not the genpd API) |
| D12b | vendor pm_domains notifier (replaces D12a) | **PRESENT** | `arch/arm64/mach-exynos/pm_domains.c:413` `of_parse_phandle(dev->of_node, "samsung,power-domain", 0)`; `:431` `case BUS_NOTIFY_BIND_DRIVER:` → `exynos_read_domain_from_dt` → `exynos_add_device_to_pd`. mali node already carries `samsung,power-domain = <&pd_g3d>` (`arch/arm64/boot/dts/exynos7580.dtsi:1075`) → any platform driver for `mali@0x11400000` is auto-attached; panfrost's genpd/device-link calls are deleted, not backported |
| D13 | DT compatibles (`arm,mali-t720` et al., clocks, interrupts) | **PRESENT, needs cutover edit at land time (NOT in this task)** | mali node `exynos7580.dtsi:1066-1076`: `compatible = "arm,mali"` (kbase-only), `reg = <0x11400000 0x5000>` (`:1068`), `interrupts` JOB/MMU/GPU (`:1069-1070`), 6 clocks with vendor names `fin_pll/fout_g3d_pll/aclk_g3d/mout_g3d/dout_aclk_g3d/aclk_g3d_400` (`:1071-1074`). v5.2 match table `panfrost_drv.c:444-452` includes `"arm,mali-t720"`. M0 kmod already resolves `"aclk_g3d"`/`"aclk_g3d_400"` by name (`kmod/panfrost7580.c:590,599`) and `regulator_get(NULL,"vdd_g3d")` (`:609`) |
| D14 | DRM core: `drm_dev_alloc`/`drm_dev_register` split, `readl_poll_timeout`, `drm_atomic_helper`, `DEFINE_DRM_GEM_*_FOPS` | **ABSENT** (all four) | `grep drm_dev_alloc\|drm_dev_register drivers/gpu/drm/drm_stub.c drivers/gpu/drm/drm_platform.c` → no hits (3.10 uses `drm_platform_init:185` + `drm_stub.c` legacy attach); `grep readl_poll_timeout include/linux/io.h` → no hits (M0 ships local poll helper); `ls drivers/gpu/drm/*atomic*` → no such file; 3.10 ioctl core is the legacy table `drivers/gpu/drm/drm_drv.c:63-109` (`DRM_IOCTL_DEF`, `drm_ioctls[]`) with **none** of the panfrost ioctls — SUBMIT/CREATE_BO/MMAP_BO/GET_PARAM/WAIT_BO/GET_BO_OFFSET exist only in the v5.2 UAPI header, and `JM_CTX_CREATE` exists in **neither** (it postdates v5.2) |
| D15 | kbase ownership / clocks / regulator / power (cutover preconditions) | **kbase OWNS all three — cutover required** | `CONFIG_MALI_MIDGARD=y` + `CONFIG_MALI_T7XX=y` (`config-samsung-j7elte.aarch64:2285,2289`, `T6XX` unset `:2269`); kbase clocks `aclk_g3d` enable `:260`, `aclk_g3d_400` `:422`, `vdd_g3d` BUCK4 `regulator_get(NULL,…)` `:711` in `drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c`; DT `mali@0x11400000` + `pd_g3d` (`exynos7580.dtsi:1066-1076`). Both drivers program `AS_TRANSTAB`, own the 3 job slots and reset the block → cannot coexist: **cutover = `CONFIG_MALI_MIDGARD=n`** (see §5) |

Notes on PRESENT items: `drm_mm` with `color_adjust` (`include/drm/drm_mm.h:79`),
`drivers/iommu/iova.c` (435 lines), `drivers/base/dma-buf.c` (733 lines, pre-resv),
staging Android `sync` (`drivers/staging/android/sync.c`, `sync.h`) as the
fence-semantics donor for any D2 shim, `pm_runtime` stubs
(`include/linux/pm_runtime.h:205-221`), `clk_get` (`include/linux/clk.h:139`),
`devm_regulator_get` (`include/linux/regulator/consumer.h:136`),
`of_platform` (`include/linux/of_platform.h:57-62`).

## 3. Difficulty rank (easiest → hardest)

| Rank | Item | Strategy | Why this rank |
|---|---|---|---|
| 1 | D12 PM domain, D11 reset | **delete, don't port** | vendor notifier + SOFT_RESET already proven in M0 |
| 2 | D13 DT/clocks/regulator | **shim at probe** | M0 already resolves vendor clock names + `vdd_g3d`; DT compatible append is one line at cutover |
| 3 | D8/D9/D14-core GEM+prime+mmap, `drm_platform_init`, local poll helper | **adapt in place** | all present; only the `drm_driver`/`fops`/`vm_ops` spelling differs from v5.2 |
| 4 | D10 devfreq/OPP, D7/D6-MMU encoder | **reuse kbase + M0** | M0 4-level encoder tested (114 checks); DVFS = port kbase Exynos7580 freq/ASV tables to OPP/devfreq, no framework backport |
| 5 | D5 GEM backing | **reuse 3.10 CMA helper, do NOT port shmem** | shmem (16 KiB, `drm_gem_shmem_helper.c` at v5.2) needs sg_table + shrinker + `get_pages` plumbing; CMA (`drm_gem_cma_create/mmap/vm_ops`, 272 lines, present) gives contiguous scanout-friendly BOs and matches a render-only driver with no SMMU. IOVA window via `drm_mm`+`iova.c` (both present) |
| 6 | D2+D3 fence+resv | **minimal in-tree fence/resv shim** (~1–1.5k LOC, not the full 18822+14849+19322+9273 B upstream) | needed by scheduler + syncobj; seed from `drivers/staging/android/sync.c` semantics + `dma-buf.c` ops; hardest API surface against 3.10 scheduler/mm |
| 7 | D4 syncobj | **subset: binary syncobj only** for SUBMIT `in_syncs/out_sync` | full v5.2 `drm_syncobj.c` (32803 B) includes timeline work 3.10 cannot use; WAIT_BO ioctl can cover simple waits first |
| 8 | D1 scheduler | **last; thin single-queue shim first, full `sched_main.c` (20702 B) only if needed** | depends on D2; panfrost_job.c (564 lines) is written against `drm_gpu_scheduler` run/dependency callbacks — a minimal FIFO that preserves the callback shape unblocks bring-up, full preemption/priority later |

Critical path: D2 → D3 → D4 → D1 (fence → resv → syncobj → sched), ~5–6k LOC
of new core code if ported whole; the driver itself (~3k LOC) is the easy part.

## 4. Recommended smallest viable target

**Port panfrost v5.2 UAPI generation verbatim** (6 ioctls, `GPU_PROD_ID`-only
`GET_PARAM`, no `JM_CTX_CREATE`/`MADVISE`/`PERFCNT`), with internals adapted:

* GEM backing: 3.10 `drm_gem_cma_helper` instead of `drm_gem_shmem_helper`.
* MMU: tested M0 4-level LPAE encoder instead of `io-pgtable-arm`
  (32792 B) + `io-pgtable.c` (1687 B); swap later — the format is proven equal.
* PM/reset: delete `dev_pm_domain_attach/device_link/reset_control` paths;
  rely on `pm_domains.c` notifier + `GPU_CMD` reset + existing `pd-g3d`/`vdd_g3d`.
* DVFS: kbase Exynos7580 tables → devfreq/OPP, not upstream `panfrost_devfreq.c`
  verbatim (v5.2 file is 221 lines but assumes modern OPP clkname API absent here).
* Keep `panfrost_drm.h` byte-identical so stock Mesa `panfrost_dri.so`
  (already packaged for Alpine aarch64) is the unmodified userspace.

Why v5.2 and not newer: every later generation only *adds* absent
dependencies (`JM_CTX_CREATE`/`DESTROY`, `MADVISE`, perfcnt, timeline syncobj,
`devm_drm_dev_alloc`, atomic modeset coupling). v5.2 is the earliest
T720-capable generation with the smallest ioctl surface (6 vs 9+) and the
smallest helper surface (`sched_main.c` 20702 B vs later multi-file scheduler,
no perfcnt file at all). Why not older than v5.2: panfrost only merged in 5.1
and T720 support + shmem-GEM stabilised in the 5.2 window; going earlier buys
nothing on this tree.

## 5. Conflicts / cutover note

kbase (`drivers/gpu/arm/t7xx/r15p0`, `CONFIG_MALI_MIDGARD=y`) owns
`mali@0x11400000`, the six `*_g3d*` clocks, and `pd-g3d` today. The two drivers
cannot share the block (AS_TRANSTAB, 3 job slots, GPU reset). Cutover means
**`CONFIG_MALI_MIDGARD=n`** plus the DT compatible/clock-name delta documented
in `code/userspace/gpu/panfrost/design.md` §8 — recorded here as a constraint,
not applied (NEVER modify DT payloads: S-Boot hangs on rebuilt `dt.img`).

## 6. Milestone-ordered backport list

Order respects the D2→D3→D4→D1 dependency chain; each step is testable on
device without the next.

1. **B0 — tree + config**: `CONFIG_DRM=y`, `CONFIG_DRM_PANFROST=m` (temp; production built-in — shipping config has `# CONFIG_MODULES is not set:217`, `# CONFIG_DRM is not set:2263`), keep `CONFIG_MALI_MIDGARD=y` until B5; private `/tmp` `O=` dir only.
2. **B1 — DRM core adaptation**: `drm_platform_init`-style registration, legacy `drm_ioctl_desc` table with the 6 v5.2 ioctls, local `readl_poll_timeout` helper, `DEFINE_*_FOPS` equivalent, `GET_PARAM`/`GET_BO_OFFSET` first (no fence/sched needed).
3. **B2 — GEM on CMA**: `CREATE_BO`/`MMAP_BO` over `drm_gem_cma_create/mmap/vm_ops` + `drm_mm`/`iova.c` IOVA window; dma-buf import/export via present `drm_gem_prime_*`; `WAIT_BO` polling stub.
4. **B3 — MMU**: M0 encoder → coherent table pool, IOVA allocator, `AS_TRANSTAB/TRANSFG` programming, MMU IRQ/fault decode, GPU-side page read test (M1 in `gpu-panfrost.md`).
5. **B4 — fence/resv/syncobj shim** (D2+D3+D4): minimal fence + resv ops on BOs, binary-syncobj subset for SUBMIT `in_syncs/out_sync`; stress against staging `sync` semantics.
6. **B5 — scheduler + SUBMIT** (D1): thin FIFO behind `drm_gpu_scheduler` callback shape → full `sched_main.c` port if needed; wire `SUBMIT`; **cutover `CONFIG_MALI_MIDGARD=n`** here, not before.
7. **B6 — DVFS + hardening**: kbase freq/ASV tables → devfreq/OPP, perfcnt later (not in v5.2), fault recovery, suspend/resume, Mesa `panfrost_dri.so` headless render test (llvmpipe is today's only 3D path per `gpu-userspace` report).

LoC order-of-magnitude (upstream bytes for scale): fence 18822 + resv 14849 +
`dma-fence.h` 19322 + `reservation.h` 9273 + syncobj 32803 + `sched_main` 20702
+ shmem 16398 (**not** ported — replaced by present 272-line CMA helper) +
`io-pgtable-arm` 32792 (**not** ported — replaced by tested ~350-line encoder).
