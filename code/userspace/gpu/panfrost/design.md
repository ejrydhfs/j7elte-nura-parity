# Panfrost / DRM feasibility and port design — Mali-T720 (Exynos 7580, j7elte)

Status: **design + milestone M0 delivered as buildable code.** No GPU driver exists yet;
nothing in this document was run on hardware.

Baseline: Nura kernel tree
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
(3.10.108, `b5e363a`), config `config-nura-j7elte`, mainline reference `linux-7.3`.
Companion reports: `parity/reports/gpu.md` (kbase parity), `parity/reports/gpu-userspace.md`
(no Linux userspace path today), `parity/reports/gpu-panfrost.md` (this work).

---

## 0. Verdict

1. **The 3.10 tree cannot host panfrost as-is.** Every one of panfrost's kernel
   prerequisites except `PM_DEVFREQ`, `drm_mm`, `dma-buf` and `drm_gem_cma_helper`
   is absent — including two that are architectural rather than cosmetic:
   **`dma-fence` does not exist at all in 3.10**, and **`drm_sched` does not exist**.
   `drm_sched` is built on `dma-fence`; panfrost's UAPI (`SUBMIT` with
   `in_syncs`/`out_sync` syncobj handles) is built on both. This is not a
   "enable CONFIG_DRM" problem (§1).
2. **The GPU itself is well understood and is not the risk.** kbase's register map,
   four-level LPAE page tables, GPU-ID/feature registers and power-up sequence match
   mainline panfrost field for field (§2). The MMU descriptor encoder is the piece that
   is cheap to build and test without silicon, and it is delivered here with 114
   passing checks on both x86-64 and aarch64 (§4, M0).
3. **The correct target is byte-identical mainline panfrost UAPI.** If the backport
   exposes `DRM_IOCTL_PANFROST_*` exactly as mainline does, stock Mesa
   (`panfrost_dri.so`, already packaged for Alpine aarch64) works unmodified. Every
   deviation from that UAPI turns the project into "write a Mesa driver too".
4. **Effort, kernel only, no display: 5–7 person-months.** With a DRM/KMS display
   driver (the 3.10 tree has no DECON/DSIM DRM driver at all): **7–10 person-months**
   plus hardware. The mainline-Exynos7580 route is 6–18 person-months but yields
   upstreamable code; the 3.10 route reuses the working display/power stack and is the
   only realistic option if Nura stays on 3.10 (§5).
5. **M0 (this delivery) is real code, not a stub**: the register/power/MMU bring-up
   module compiles and links against the 3.10 tree (`vermagic 3.10.108-gb5e363a1 SMP
   preempt mod_unload aarch64`) and the shared MMU encoder is unit-tested. It creates
   **no** DRM node and submits **no** jobs, and it says so.

---

## 1. 3.10 DRM core vs panfrost's prerequisites

### 1.1 What exists, what does not

Measured against `drivers/gpu/drm`, `drivers/iommu`, `drivers/dma-buf`,
`drivers/base` and `include/` in the 3.10 tree:

| Prerequisite (mainline `drivers/gpu/drm/panfrost/Kconfig`) | In 3.10 tree | Consequence |
|---|---|---|
| `DRM` | present, `# CONFIG_DRM is not set` (config:2262) | must be enabled; the display stack is fbdev (`CONFIG_EXYNOS_DECON_FB=y`) |
| `DRM_SCHED` | **absent** (`drivers/gpu/drm/scheduler/`, `include/drm/gpu_scheduler.h` missing) | write/backport 2.7k LOC |
| `DRM_GEM_SHMEM_HELPER` | **absent** (`drm_gem_shmem_helper.c` / `.h` missing) | `drm_gem_cma_helper` exists but is CMA-paddr based, not sg_table/shmem |
| `IOMMU_IO_PGTABLE_LPAE` | **absent** (`drivers/iommu/io-pgtable*.c/h` missing) | write/backport 1.7k LOC, or use the delivered encoder (§4) |
| `dma-fence` / `dma-resv` | **absent** (`include/linux/dma-fence*.h`, `dma-resv.h`, `drivers/dma-buf/dma-fence.c`, `dma-resv.c` missing) | foundation for drm_sched, syncobj, dma-buf import; 2.9k LOC |
| `drm_syncobj` | **absent** (`drm_syncobj.c`, `drm_syncobj.h`) | panfrost UAPI uses syncobj handles; ~0.8k LOC for the needed subset |
| `PM_DEVFREQ` | `CONFIG_PM_DEVFREQ=y` (config:3264) | ok |
| `drm_mm` | present (`include/drm/drm_mm.h`) **including `color_adjust`** (:79) | panfrost's IOVA allocation works unchanged |
| `dma-buf` | present as the original 3.10 API (`drivers/base/dma-buf.c`, ops without `resv`) | no `dma_resv` plumbing in `map_dma_buf` |
| `drm_dev_alloc`/`drm_dev_register` split | **absent** (3.10 embeds `struct drm_device` in the driver struct; `drm_dev_alloc` is a callback) | every panfrost `drv`/`gem` entry point needs adapting |
| `drm_gem_object_funcs`, `drm_gem_object_put_locked`, `drm_gem_object_get` | **absent** | GEM layering must be rewritten against 3.10 `drm_gem.c` callbacks |
| `readl_relaxed_poll_timeout()` | **absent** (not even `readl_poll_timeout`) | local poll helper (already implemented in M0) |
| `device_link_add()` | **absent** | panfrost's `pm_domain_links[]` must be dropped; see §2.4 |
| `dev_pm_domain_attach_by_name()` | **absent** (3.10 has the old genpd API: `__pm_genpd_of_add_device`, `pm_genpd_poweron`) | use the vendor `samsung,power-domain` path instead (§2.4) |
| `devm_clk_get_optional()`, `clk_bulk_get()` | **absent** | plain `devm_clk_get()` + error handling |
| `atomic`/`drm_atomic_helper` | **absent in 3.10** | not needed by a render-only driver, but blocks any KMS work on the same core |

Two structural blockers beyond the table:

- **`CONFIG_MODULES is not set`** in `config-nura-j7elte` (and in
  `lineageos_j7elte_defconfig`). Nothing can be loaded at runtime; a port must be
  built in-tree, or `CONFIG_MODULES=y` must be added (this is a config change outside
  this task's ownership and is therefore reported, not applied). The M0 module was
  verified against a *temporary* config with `CONFIG_MODULES=y` (§4).
- **`drm_sched` cannot be "stubbed out"** without changing the UAPI: panfrost's
  `SUBMIT` ioctl carries an array of syncobj handles and returns an out-sync; Mesa
  relies on that for implicit and explicit fencing. Either the backport implements
  the same semantics (dma-fence + syncobj), or Mesa must be modified. The design
  chooses the former.

### 1.2 Consequence for "just enable DRM and build panfrost"

`# CONFIG_DRM is not set` is the least of the problems: enabling it gives a 3.10 DRM
core that panfrost does not compile against (`drm_dev_alloc`, GEM object funcs,
`drm_gem_object_put_unlocked`, `dma_resv`, `drm_sched`, `io_pgtable` all missing).
The build would fail on the first line of `panfrost_drv.c`.

---

## 2. Exynos 7580 GPU integration

### 2.1 Device tree

`arch/arm64/boot/dts/exynos7580.dtsi:1066-1076`:

```
mali@0x11400000 {
        compatible = "arm,mali";
        reg = <0x0 0x11400000 0x0 0x5000>;
        interrupts = <0 282 0>, <0 283 0>, <0 281 0>;
        interrupt-names = "JOB", "MMU", "GPU";
        clocks = <&clock 1>, <&clock 10>, <&clock 160>,
                 <&clock 161>, <&clock 162>, <&clock 163>;
        clock-names = "fin_pll", "fout_g3d_pll", "aclk_g3d",
                      "mout_g3d", "dout_aclk_g3d", "aclk_g3d_400";
        samsung,power-domain = <&pd_g3d>;
};
```

Deltas a panfrost-class driver needs:

| Item | kbase (works today) | panfrost mainline expects | Action |
|---|---|---|---|
| compatible | `"arm,mali"` | `"arm,mali-t720"` | add the second string (patch delivered, order keeps kbase matching) |
| clocks | 6 named Exynos clocks | `core` (+ optional `bus`, `bus_ace`) | platform data/glue: core = `aclk_g3d` (id 160), bus = `aclk_g3d_400` (id 163) |
| power domain | `samsung,power-domain = <&pd_g3d>` | `power-domains = <&pd_g3d>` + `dev_pm_domain_attach` | keep the vendor property on 3.10 (§2.4); switch to `power-domains` only on mainline |
| regulator | none in DT; kbase requests `"vdd_g3d"` **by name** | `vdd` / `mali-supply` | keep the named lookup; `vdd_g3d` is BUCK4 in `s5m/s2mpu03` (600–1300 mV) |
| OPP table | none; kbase uses hard-coded Exynos7580 tables in `gpu_exynos7580.c` | OPP table for devfreq | port the tables into a devfreq/OPP shim (§4, M4) |
| `dma-coherent` | absent → `system_coherency = COHERENCY_NONE` (kbase core_linux.c:3577) | absent → `coherent_walk = false` | no change |

There is **no system MMU (sysmmu) node for the GPU** in the DT (`grep sysmmu` shows
ISP/MFC/DISP only). That is correct: the T720 translates through its own MMU, so
panfrost does not need `samsung,exynos-sysmmu`/`arm-smmu` at all. `io-pgtable` is a
*software* page-table library, not an IOMMU driver — one less subsystem to port.

### 2.2 Register map — kbase and panfrost are the same layout

Verified constant-by-constant, kbase `mali_midg_regmap.h` vs mainline
`panfrost_regs.h` (values asserted in `mali_lpae_test.c:test_registers`):

| Register | kbase | panfrost | Value |
|---|---|---|---|
| GPU_ID | `GPU_CONTROL_REG(GPU_ID)` | `GPU_ID` | 0x0000 |
| MMU_FEATURES | 0x014 | `GPU_MMU_FEATURES` | 0x0014 |
| AS_PRESENT / JS_PRESENT | 0x018 / 0x01C | same | 0x0018 / 0x001C |
| Job slot n | `0x1000 + 0x800 + n*0x80` | `JS_BASE 0x1800 + n*0x80` | 0x1800 + n*0x80 |
| MMU address space n | `0x2000 + 0x400 + n*0x40` | `MMU_BASE 0x2400 + n*0x40` | 0x2400 + n*0x40 |
| AS_TRANSTAB/AS_MEMATTR/AS_COMMAND | +0x00/+0x08/+0x18 | same | — |
| L2/SHADER/TILER PWRON | 0x1A0/0x180/0x190 | same | — |

Job slots: Midgard has at most 3 (`BASE_JM_MAX_NR_SLOTS 3`,
`mali_kbase_defs.h:134`); the actual set comes from `JS_PRESENT`. kbase's
`BASE_JM_MAX_NR_SLOTS` is a compile-time ceiling, panfrost uses `hweight32(js_present)`,
so there is no mismatch to fix — only an assumption to verify on silicon.

### 2.3 MMU / page tables

- Both implementations use **four levels of 512 entries, 9 bits per level, 4 KiB
  granule**, indexed by `vpfn >> ((3 - level) * 9)`.
  kbase: `mali_kbase_mmu.c:434-449`; mainline: `io-pgtable-arm.c:1232-1236`
  ("Mali seems to need a full 4-level table regardless of IAS"). For a 32-bit VA this
  wastes the top table (index 0 only) but is mandatory.
- Leaf descriptors are **type 1 ("BLOCK"/ATE)**, tables type 3 ("PTE"/TABLE) — note
  that for the Mali format even a 4 KiB mapping is `PTE_TYPE_BLOCK`, not
  `PTE_TYPE_PAGE` (`io-pgtable-arm.c:377-390`). The delivered encoder does exactly this.
- AF = bit 10, RD = 6, WR = 7, SH = 9:8, XN = 54, cache index = 4:2; address bits 39:12
  (40-bit output address). Identical in kbase and mainline.
- VA/PA width comes from `MMU_FEATURES[7:0]`/`[15:8]` (kbase
  `mali_kbase_gpuprops.c:303-304`, panfrost `panfrost_regs.h:19-20`). Panfrost feeds
  them straight into `io_pgtable_cfg.ias/.oas`. **The T720's actual values are not
  knowable statically** — M0 prints them, and they decide the IOVA range.
- `TRANSTAB` = pgd_pa | `ADRMODE_TABLE(3)` | `READ_INNER(4)`, plus `SHARE_OUTER(0x10)`
  iff the system is ACE-coherent. kbase adds it only for `COHERENCY_ACE`
  (`mali_kbase_mmu_hw_direct.c:276`), mainline only for `coherent_walk`
  (`io-pgtable-arm.c:1263`). Exynos 7580 has no coherency property → **no SHARE_OUTER**,
  and page tables need explicit cache maintenance.
- `AS_TRANSCFG` must be `ADRMODE_LEGACY` (0) for the Mali format; both sides agree.

**One genuine divergence** (risk R4): the `AS_MEMATTR` tables differ.

| index | kbase LPAE | panfrost `ARM_MALI_LPAE` |
|---|---|---|
| 0 | 0x48 (impl-def) | 0x88 (NC / outer impl-def) |
| 1 | 0x4F (force-to-cache-all) | 0x8D (write-alloc) |
| 2 | 0x4D (write-alloc) | 0x88 (device) |
| 3 | 0x88 (outer impl-def) | — |
| 4 | 0x8D (outer WA) | — |

Full registers: kbase `0x8D884D4F48`, panfrost `0x888D88`. Upstream panfrost lives
with this (its PTE cache index 1 = 0x8D), but a port that mixes kbase-created buffers
with panfrost-created PTEs would apply different cache policies. Since kbase is
disabled in the cutover, this is a **performance/coherency risk to validate**, not a
correctness blocker — and it is asserted in the unit test so it cannot silently drift.

### 2.4 Power, clocks, regulator, reset

- **Power domain.** kbase does not use generic genpd: `gpu_control.c:49-59` walks
  `for_each_compatible_node(..., "samsung,exynos-pd")` and compares `genpd.name` to
  `"pd-g3d"`. On arm64 the provider is `arch/arm64/mach-exynos/pm_domains.c`, which
  registers a **platform-bus notifier** (`exynos_pm_notifier_call`) and, on
  `BUS_NOTIFY_BIND_DRIVER`, reads `samsung,power-domain` and calls
  `__pm_genpd_of_add_device()` (`pm_domains.c:401-430`). Therefore **a panfrost-class
  driver needs no power-domain code at all on 3.10**: keep
  `samsung,power-domain = <&pd_g3d>`, bind as a platform driver, and the vendor glue
  puts the device into `pd-g3d`; runtime PM then gates it.
  This is why panfrost's `dev_pm_domain_attach_by_name()` + `device_link_add()` path
  (both absent in 3.10) can simply be dropped rather than backported.
- **Clocks.** kbase uses `aclk_g3d` as the functional clock (`clk_get(dev, "aclk_g3d")`,
  `gpu_exynos7580.c:507`, `clk_prepare_enable` at `:260`) and switches to
  `aclk_g3d_400` for the 400 MHz branch (`:422`). `fin_pll`, `fout_g3d_pll`, `mout_g3d`,
  `dout_aclk_g3d` are rate/DVFS plumbing. For M0: `aclk_g3d` (required) and
  `aclk_g3d_400` (optional) are sufficient.
- **Regulator.** `regulator_get(NULL, "vdd_g3d")` (`gpu_exynos7580.c:711`) — a global
  name lookup, not a DT supply. M0 matches that (and logs + continues if absent).
- **Reset.** The Exynos DT has no `resets` property and the GPU block has no reset
  controller binding; reset is the register-level soft reset:
  `GPU_CMD = SOFT_RESET`, poll `GPU_INT_RAWSTAT & RESET_COMPLETED` (kbase
  `mali_kbase_pm_driver.c:1414-1425`; panfrost `panfrost_gpu.c:62-95`, which polls
  `RAWSTAT` with a 10 ms/10 s timeout and falls back to `HARD_RESET`). M0 implements
  exactly this, with a local `udelay` poll because `readl_poll_timeout` is missing.
- **Core power-up order.** L2 → shaders → tiler, each `PWRON` then `READY` poll
  (panfrost `panfrost_gpu.c:452-481`). kbase's policy engine is far more elaborate
  (core stacks, always-on, DVFS) but the *hardware* sequence is the same; for a
  panfrost-class driver the simple sequence is sufficient.

### 2.5 What a panfrost-class driver needs from kbase (and what it must not reuse)

Reusable knowledge (already encoded in this delivery): register map, GPU-ID/product
decode, MMU geometry and descriptor bits, memattr tables, reset/power sequences,
`TRANSTAB`/`AS_TRANSCFG` programming, coherency rules.

**Not** reusable as code:

- kbase has **no kernel API for GPU memory/VM or job submission**
  (`KBASE_EXPORT_TEST_API` compiles to nothing without `MALI_KERNEL_TEST_API`), so a
  DRM "shim" over kbase (path E in `gpu-userspace.md`) is effectively a second driver
  and is rejected.
- The two drivers cannot share the GPU: both program `AS_TRANSTAB`, both own the job
  slots and both reset the block. The cutover is `CONFIG_MALI_MIDGARD=n` plus removal
  of the `"arm,mali"` match (or simply never loading kbase, which is impossible while
  it is built-in).

---

## 3. Design of the port

### 3.1 Shape

```
  Mesa (panfrost_dri.so, aarch64 musl, unmodified)
        |  DRM_IOCTL_PANFROST_{GET_PARAM,CREATE_BO,MMAP_BO,SUBMIT,
        |                     JM_CTX_CREATE,GET_BO_OFFSET,SET_LABELS,PERFCNT_*}
  drivers/gpu/drm/panfrost/            <- mainline driver, adapted to 3.10
        |-- panfrost_device  (clk/regulator/genpd, runtime PM, GPU_ID/features)
        |-- panfrost_gpu     (reset, power, IRQ, perfcnt)
        |-- panfrost_mmu     (AS allocation, page tables, faults)
        |-- panfrost_gem     (GEM objects, dma-buf import/export)
        |-- panfrost_job     (JM slots, job chain, fences)
        `-- exynos7580 glue (clock names, vdd_g3d, DVFS/OPP tables)
  backport layer (new, 3.10-only)
        |-- dma-fence (+ array, chain)
        |-- dma-resv
        |-- drm_sched  (or a reduced single-queue submitter with the same semantics)
        |-- drm_syncobj (basic subset)
        |-- GEM shmem helper (or a 3.10-native GEM with sg_table + vmap + mmap)
        `-- page-table encoder / io-pgtable-arm
```

Key decisions:

1. **UAPI identical to mainline.** Copy `include/uapi/drm/panfrost_drm.h` verbatim.
   This is the whole point: it is what makes stock Mesa (already in Alpine aarch64)
   the userspace, instead of writing one.
2. **Write our own page-table allocator, do not port `io-pgtable` wholesale.**
   `io-pgtable-arm`'s generic layer (fmt registry, TLB op abstraction, S2/VHE paths,
   concatenation) is dead weight for one GPU. The delivered `mali_lpae.h` is a
   4-level encoder in ~350 lines with unit tests; the driver adds an IOVA allocator
   (3.10 already has `drivers/iommu/iova.c`) and coherent table allocation.
   *If* upstream parity is later wanted, swapping in real `io-pgtable-arm` is a local
   change because the descriptor format is proven identical.
3. **Keep `drm_sched`.** It is 2.7k LOC but it is the piece that gives correct
   mid-job preemption/teardown semantics and the fence model Mesa expects. Replacing
   it with a hand-rolled queue is *less* code but more risk (job timeouts, reset
   races, `panfrost_jm_is_idle`). Milestone M3 makes this decision explicit with a
   measured comparison; the design default is "port drm_sched".
4. **No KMS in the GPU driver.** Panfrost is render-only. Display comes from fbdev
   today; a DRM/KMS DECON+DSIM driver is a separate gap (`parity/reports/display.md`).
   Without it, EGL works *headless* (`EGL_PLATFORM_SURFACELESS_MESA`) and with
   `kms_swrast`-style offscreen rendering; nothing appears on the panel.
   This is why "kernel only" and "with display" are quoted separately.
5. **Cutover is a config change, not a runtime one**: `CONFIG_MALI_MIDGARD=n`,
   `CONFIG_DRM=y`, `CONFIG_DRM_PANFROST=y`, `CONFIG_MODULES` irrelevant if built in.

### 3.2 Delivered in this milestone

| File | Lines | What it is |
|---|---|---|
| `mali_hw.h` | ~330 | register map, GPU-ID/MMU-feature decode, LPAE descriptor bits, both memattr tables, `TRANSTAB` encoder. Provenance-commented per constant. |
| `mali_lpae.h` | ~300 | four-level page-table builder (map/walk/unmap, allocator callbacks), kernel- and userspace-safe. |
| `mali_lpae_test.c` | ~420 | 114 checks: index math, descriptor encoding, register cross-check, ID/feature decode, map/walk/unmap/leak, memattr divergence. |
| `Makefile` | — | builds and runs the tests natively **and** under `qemu-aarch64`. |
| `kmod/panfrost7580.c` | ~680 | M0 kernel module: platform probe, `aclk_g3d`/`aclk_g3d_400`, `vdd_g3d`, soft/hard reset, feature readout, core power on/off, in-kernel MMU encoder self-test, debugfs. |
| `kmod/{Kbuild,Makefile}` | — | out-of-tree build against the prepared 3.10 tree. |

The module deliberately has **no** DRM device, **no** GEM, **no** submission. It is the
minimum that (a) proves the 3.10 headers accept the driver's needs and (b) lets an
operator read the real `GPU_ID`/`MMU_FEATURES`/`JS_PRESENT` on the device with a
two-line debugfs session, which is the input M1 needs.

---

## 4. Milestones

Effort is person-days for one experienced DRM/ARM-MMU developer **with device access**;
±40 %. "Buildable" means it compiles against the 3.10 tree here; only hardware time
proves behaviour.

| # | Milestone | Content | Acceptance | Effort | Status |
|---|---|---|---|---|---|
| **M0** | Register/power/MMU bring-up | this delivery: register map, encoder + tests, module that resets, powers and reports features, in-kernel encoder self-test | module builds + links against 3.10 (`vermagic` matches); tests pass on x86-64 and aarch64; on device: `GPU_ID`=0x0720xxx, sane `MMU_FEATURES`, `JS_PRESENT`≠0, selftest PASS | 4–6 d | **code delivered**; hardware part unverified |
| **M1** | MMU address spaces | coherent table pool (PA→VA lookup), IOVA allocator over `iova.c`, AS allocation/`AS_COMMAND` sequencing, MMU IRQ + fault decode, `TRANSTAB`/`MEMATTR`/`AS_TRANSCFG` programming, map/unmap + TLB ops | debugfs interface can create an AS, map a page, read it back through the GPU MMU (GPU-side read test via a tiny job) | 8–12 d | not started |
| **M2** | DRM device + GEM | DRM core shims (`drm_dev_alloc` split, GEM object funcs), `panfrost_drm.h` UAPI, `panfrost_device`/`gpu`/`gem`, `CREATE_BO`/`MMAP_BO`/`GET_PARAM`, dma-buf import/export with 3.10 dma-buf + a dma-resv shim | `modetest`-class probe; Mesa opens the render node, allocates and maps a BO; `dma-buf` round-trip with `v4l2`/`ion` exporter | 20–30 d | not started |
| **M3** | Submission | dma-fence (+array/chain), drm_sched **or** reduced submitter (decision point, measured), `drm_syncobj` subset, `panfrost_job` JM path, job-chain descriptors, timeouts/reset | Mesa `kmscube`-class offscreen job completes; `glmark2` headless; fence/syncobj semantics match mainline | 25–40 d | not started |
| **M4** | DVFS + perfcnt | OPP/devfreq from the Exynos7580 tables (`gpu_exynos7580.c`), voltage corners, `PERFCNT_*` ioctls | frequency/voltage sweep without faults; hwcnt dump matches kbase's numbers | 8–12 d | not started |
| **M5** | Hardening | error handling, fault recovery, shrinker, suspend/resume, upstream-style cleanup, docs | 24 h stress (ioctl fuzz + GL loop), no leaks, no GPU hangs | 15–25 d | not started |
| **(D)** | Display (separate gap) | DRM/KMS driver for DECON 7580 + DSIM + ea8061 panel; `power-domains`/genpd migration | on-screen GL | 30–50 d | out of scope here |

Kernel-only total: **80–125 person-days ≈ 4–6 person-months** (M0–M5), plus
**30–50 d** for display. The prior report's ">12 person-months" assumed porting the
full modern DRM core + io-pgtable + display from scratch; the reduction here comes from
(a) not porting `io-pgtable`'s generic layer, (b) reusing the vendor genpd notifier
instead of `dev_pm_domain_attach`/`device_link`, (c) using the existing fbdev display
for M0–M5 and quoting KMS separately.

### 4.1 Recommended immediate next step

M1, in this order, all verifiable without a full driver:

1. `GPU_ID`/`MMU_FEATURES`/`JS_PRESENT` on hardware via M0 (5 minutes, decides IOVA range).
2. Coherent table pool + AS programming + MMU fault IRQ (the driver can then take a
   fault and report it, which is the single most useful bring-up signal).
3. A "GPU reads my page" test: one address space, one page mapped, a trivial
   write-then-read via the tiler or a no-op job — this validates `MEMATTR` and the
   cache-policy divergence (R4) before any Mesa work.

---

## 5. Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| **R1** | `dma-fence`/`drm_sched`/`drm_syncobj` backport is the critical path (~5k LOC of new core code interacting with 3.10's scheduler and Android `sync` framework) | schedule | port in the order fence → resv → syncobj → sched; keep panfrost UAPI fixed; unit-test each layer with a fake driver before touching panfrost |
| **R2** | 3.10 DRM core differences (`drm_dev_alloc`, GEM object funcs, no `drm_atomic_helper`) leak into panfrost code as permanent divergence | maintainability | isolate every shim in one `drm_compat.h`/`drm_compat.c`; keep `panfrost_*.c` as close to mainline as possible so a future mainline move is a rebase, not a rewrite |
| **R3** | Actual T720 `MMU_FEATURES` may report fewer VA bits than Mesa/panfrost assume (panfrost reserves 32 MB..4 GB IOVA in `panfrost_mmu.c:805`) | functionality | read it in M0; if VA < 32 bits, shrink the IOVA window and note the GL limitation |
| **R4** | `AS_MEMATTR` divergence: kbase index 0 = 0x48, panfrost index 1 = 0x8D (see §2.3) | coherency / performance | M1's GPU-read test with both tables; assert the chosen table in the unit test (already done) |
| **R5** | Non-coherent PTW (no ACE, no `dma-coherent`): missing cache maintenance on page tables or buffers → random GPU faults | correctness | `coherent_walk = false`; explicit `clean_dcache_area` after PTE writes and before `AS_COMMAND_UPDATE`; reuse kbase's `kbase_mmu_sync_pgd` pattern |
| **R6** | Both drivers claim the same hardware; kbase is built-in | cannot bind | cutover is `CONFIG_MALI_MIDGARD=n`; M0 refuses to probe without `force=1` |
| **R7** | No DRM/KMS display driver and the DRM core lacks atomic helpers; `kms_swrast`/wayland unusable | no on-screen output | quoted separately (D); headless EGL works without it |
| **R8** | `CONFIG_MODULES is not set` in the shipping config; the module path is only a development vehicle | deployment | build the driver in-tree (`obj-y`) for production; the module build is verified against a temporary `CONFIG_MODULES=y` config |
| **R9** | Mesa's panfrost requires the `PANFROST_GET_PARAM` feature set (e.g. `GPU_FEATURES`, `GPU_ID`, `MMU_FEATURES`) to match exactly, and checks `panfrost_gpu_id` against its model table | Mesa refuses the device | the mainline driver already derives all of these; do not "simplify" the ioctl payloads |
| **R10** | No hardware in this session; nothing beyond compilation and CPU-side tests is verified | schedule/quality | first hardware session: M0 + M1 fault test; treat every "works" claim in this document as untested on silicon |
| **R11** | dvfs: kbase's DVFS is a Samsung-specific `gpu_dvfs_handler` with ASV tables, not devfreq-with-OPP | thermal/power | M4 ports the tables; until then run at one safe OPP with `vdd_g3d` at a fixed voltage |

---

## 6. Rejected alternatives (and why)

- **DRM shim translating panfrost ioctls onto kbase internals.** kbase exposes no
  kernel API; its MMU/staging/atom entry points are internal and the GPU is
  single-owner. This is a second driver with extra steps.
- **Porting mainline `io-pgtable` verbatim.** 1.7k LOC of generic framework for one
  format; the four-level encoder is ~350 lines and is already tested. Revisit only if
  upstreaming.
- **Userspace-only work (Mesa kbase KMD fork).** Targets modern numbered-ioctl kbase
  (r32p0+) with CS submission gated off; this kernel is legacy UK 10.6 on Midgard.
- **Android `libGLES_mali.so` via libhybris/chroot.** 32-bit bionic, Android 6
  framework deps; gives Nura applications nothing.
- **Enabling `CONFIG_DRM` and hoping.** The 3.10 DRM core cannot compile panfrost;
  see §1.2.

---

## 7. Verification status of this delivery

| Item | How it was verified | Result |
|---|---|---|
| page-table encoder | `make check` (native x86-64 + aarch64 under `qemu-aarch64`) | 114 checks, 0 failures, both architectures |
| register map / descriptor bits vs kbase and mainline | assertions with literal expected values, each traceable to a line in the two trees | pass |
| memattr divergence | asserted as two distinct constants | pass (documents R4) |
| kernel module compiles against 3.10.108 | `make -C <prepared tree> M=... ARCH=arm64` with the session's GCC 10.3 aarch64 toolchain | `panfrost7580.o` clean (one toolchain-vintage `-Wmissing-attributes` warning suppressed via `KCFLAGS`; it is emitted from `include/linux/init.h`, not from this code) |
| kernel module links | modpost + `.mod.o` | `panfrost7580.ko`, ELF64 aarch64, `vermagic=3.10.108-gb5e363a1 SMP preempt mod_unload aarch64` |
| undefined symbols | every `U` symbol resolves to an `EXPORT_SYMBOL` in the tree (`dev_err`/`dev_warn`/`_dev_info` via `define_dev_printk_level`, `drivers/base/core.c:2032`) | all resolvable |
| hardware behaviour | **not tested** — no j7elte available | unknown |

Reproduction (build tree outside the kernel source, so the shared tree is untouched):

```sh
K=<kernel tree>; B=<build dir>
make -C $K ARCH=arm64 CROSS_COMPILE=<aarch64-gcc> O=$B lineageos_j7elte_defconfig
# 3.10 has no modules_prepare; and this defconfig has CONFIG_MODULES unset,
# so for a *module* build only, enable it in $B/.config first:
#   CONFIG_MODULES=y CONFIG_MODULE_UNLOAD=y
make -C $K ARCH=arm64 CROSS_COMPILE=<aarch64-gcc> O=$B silentoldconfig
make -C $K ARCH=arm64 CROSS_COMPILE=<aarch64-gcc> O=$B prepare
make -C $K ARCH=arm64 CROSS_COMPILE=<aarch64-gcc> O=$B \
     HOSTCFLAGS="-fcommon -O2" scripts        # builds scripts/mod/modpost
cd parity/userspace/gpu/panfrost/kmod
make KDIR=$B CROSS_COMPILE=<aarch64-gcc> \
     KCFLAGS="-Wno-missing-attributes"
```

(`HOSTCFLAGS=-fcommon` is needed only because GCC 10 defaults to `-fno-common`, which
breaks the 3.10 `dtc` link; it does not affect target code.)

---

## 8. DT delta

Applied-safe now (delivered as `parity/patches/userspace/gpu-panfrost/0001-...`):
add `"arm,mali-t720"` as the **second** compatible string. kbase matches `"arm,mali"`
(unchanged), so behaviour is identical until a panfrost-class driver is enabled.

Required at M2 (documentation only, not applied — it would break kbase's clock
lookup semantics if applied without the cutover):

```diff
 	mali@0x11400000 {
-		compatible = "arm,mali", "arm,mali-t720";
+		compatible = "arm,mali-t720";
 		reg = <0x0 0x11400000 0x0 0x5000>;
 		interrupts = <0 282 0>, <0 283 0>, <0 281 0>;
 		interrupt-names = "JOB", "MMU", "GPU";
-		clocks = <&clock 1>, <&clock 10>, <&clock 160>,
-			<&clock 161>, <&clock 162>, <&clock 163>;
-		clock-names = "fin_pll", "fout_g3d_pll", "aclk_g3d",
-			"mout_g3d", "dout_aclk_g3d", "aclk_g3d_400";
+		clocks = <&clock 160>, <&clock 163>;
+		clock-names = "core", "bus";
 		samsung,power-domain = <&pd_g3d>;
+		/* on 3.10 the vendor notifier uses samsung,power-domain;
+		 * mainline uses power-domains = <&pd_g3d> */
 	};
```

and, for DVFS at M4, an `operating-points-v2` table derived from kbase's
`gpu_freq_table`/ASV data in `drivers/gpu/arm/t7xx/r15p0/platform/exynos/gpu_exynos7580.c`.
