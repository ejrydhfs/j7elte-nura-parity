# GPU path 2 — Panfrost/DRM port feasibility (Mali-T720, Exynos 7580, j7elte)

Scope: can the Mali-T720 in the Nura 3.10.108 kernel be driven by a Panfrost-class
DRM driver, what exactly has to be backported or written, and what can be built
today without silicon?

Deliverables: `parity/userspace/gpu/panfrost/` (shared register/MMU code + tests + M0
kernel module + `design.md`) and this report. **No hardware was available; nothing
here was run on a j7elte, and no working driver is claimed.**

Base facts reused: `parity/reports/gpu.md` (kbase is complete and at parity),
`parity/reports/gpu-userspace.md` (today's only Linux 3D path is Mesa llvmpipe).

---

## 1. Verdict

| Question | Answer |
|---|---|
| Can panfrost be enabled on the current 3.10 kernel? | **No.** Not a Kconfig problem: `drm_sched`, `dma-fence`, `dma-resv`, `drm_syncobj`, `drm_gem_shmem_helper`, `io-pgtable`, `drm_atomic_helper` are all absent; so are `drm_dev_alloc`, `drm_gem_object_funcs`, `readl_poll_timeout`, `device_link_add`, `dev_pm_domain_attach*`. |
| Are the *GPU-side* prerequisites present? | **Yes, and they match mainline exactly.** Register map (0x5000 window), 4-level/9-bit/4 KiB-granule LPAE tables, descriptor bit layout, GPU-ID and MMU_FEATURES decoding, reset/power sequences are identical between kbase r15p0 and mainline panfrost. |
| Is an Exynos 7580 integration blocker hidden in DT/power? | **No, one pleasant surprise.** The vendor `arch/arm64/mach-exynos/pm_domains.c` attaches any platform device with `samsung,power-domain = <&pd_g3d>` on `BUS_NOTIFY_BIND_DRIVER`, so panfrost's absent `dev_pm_domain_attach_by_name()`/`device_link_add()` path can simply be dropped. The GPU has no system MMU node and needs none (it uses its own MMU). |
| Effort | Kernel only, no display: **80–125 person-days (4–6 PM)**. With a DRM/KMS DECON+DSIM+panel driver (which does not exist on 3.10): **+30–50 d**, i.e. **~7–10 PM** total. Hardware access is required for everything after M0. |
| What was actually built | A tested MMU/register core (114 checks, x86-64 **and** aarch64/qemu) and an M0 kernel module that **compiles and links against 3.10.108** (`vermagic 3.10.108-gb5e363a1 SMP preempt mod_unload aarch64`). The module resets, powers, reads GPU_ID/features and self-tests the encoder; it creates no DRM node and submits nothing. |

---

## 2. What is missing in 3.10, measured

Line counts are the mainline (`linux-7.3`) files that would have to be backported or
re-implemented. Absence was checked by file existence and symbol grep in the Nura tree.

| Unit | Mainline size | In 3.10 tree | Note |
|---|---|---|---|
| `dma-fence` + `dma-fence-array` + `dma-fence-chain` | 1221 + 332 + 288 = **1841 LOC** | **absent** (`include/linux/dma-fence*.h`, `drivers/dma-buf/` do not exist) | foundation for everything below |
| `dma-resv` | **818 LOC** | **absent** | 3.10 dma-buf ops have no `resv`; needs an ops-level shim |
| `drm_syncobj` | ~**800 LOC** (basic subset) | **absent** (`drm_syncobj.c/.h`) | panfrost `SUBMIT` takes syncobj handles |
| `drm_sched` | 1355 + 668 + 243 + 418 = **2684 LOC** | **absent** (`drivers/gpu/drm/scheduler/`, `gpu_scheduler.h`) | depends on dma-fence |
| `io-pgtable` (`-arm` + core + header) | 1301 + 97 + 333 = **1731 LOC** | **absent** | **not ported here**: a 4-level encoder is 350 LOC and is delivered + tested |
| `drm_gem_shmem_helper` | **1059 LOC** | **absent** | 3.10 has `drm_gem_cma_helper` (CMA paddr, not sg_table) |
| `drm_atomic_helper` | — | **absent** | render-only driver does not need it, but no KMS on the same core |
| `drm_dev_alloc` / `drm_dev_register` split | — | **absent** | 3.10 embeds `drm_device` in the driver struct |
| `drm_gem_object_funcs`, `drm_gem_object_put_locked` | — | **absent** | GEM layering must be adapted |
| `readl(_relaxed)_poll_timeout` | — | **absent** | M0 ships a local poll helper |
| `device_link_add`, `dev_pm_domain_attach*` | — | **absent** | not needed, see §3.2 |
| `drm_mm` | — | **present, incl. `color_adjust`** (`drm_mm.h:79`) | panfrost IOVA colouring works unchanged |
| `iova.c` (IOVA allocator) | — | **present** (`drivers/iommu/iova.c`) | reusable for the MMU window |
| `dma-buf` | — | **present** (3.10 API) | ok with the resv shim |
| `PM_DEVFREQ` | — | **`=y`** (config:3264) | ok |
| `CONFIG_MODULES` | — | **not set** in `config-nura-j7elte` / `lineageos_j7elte_defconfig` | port must be built-in; module build needs a temporary config (used for verification only) |

Consequence: `# CONFIG_DRM is not set` (config:2262) is the least of the problems. Even
with DRM enabled, `panfrost_drv.c` fails to compile on its first dozen lines. Any
"enable DRM and build panfrost" plan is not viable.

The critical path is **dma-fence → dma-resv → drm_syncobj → drm_sched** (~5–6k LOC of
new core code that interacts with 3.10's scheduler and with Samsung's Android `sync`
framework). The panfrost driver itself is the *easy* part relative to that.

---

## 3. Exynos 7580 integration — what a panfrost driver needs

### 3.1 Register map: kbase and panfrost are the same hardware description

Cross-checked constant by constant (`mali_midg_regmap.h` vs mainline
`panfrost_regs.h`) and asserted in `mali_lpae_test.c`:

| | kbase (`drivers/gpu/arm/t7xx/r15p0`) | panfrost (mainline) | Value |
|---|---|---|---|
| GPU_ID / MMU_FEATURES / AS_PRESENT / JS_PRESENT | 0x000/0x014/0x018/0x01C | same | asserted |
| GPU_CMD / GPU_INT_STAT / PWR_KEY | 0x030/0x02C/0x050 | same | asserted |
| job slot n | `0x1000+0x800+n*0x80` | `JS_BASE 0x1800 + n*0x80` | 0x1800+n*0x80 |
| address space n | `0x2000+0x400+n*0x40` | `MMU_BASE 0x2400 + n*0x40` | 0x2400+n*0x40 |
| AS_TRANSTAB/MEMATTR/COMMAND/TRANSCFG | +0x00/+0x08/+0x18/+0x30 | same | asserted |
| L2/SHADER/TILER PWRON | 0x1A0/0x180/0x190 | same | asserted |
| slot/AS stride | 0x80 / 0x40 | 0x80 / 0x40 | asserted |

Job slots: kbase's ceiling is 3 (`BASE_JM_MAX_NR_SLOTS`, `mali_kbase_defs.h:134`);
panfrost uses `hweight32(JS_PRESENT)`. No conflict — only a value to read on silicon.

### 3.2 Power, clocks, regulator, reset

- **pd-g3d needs no backport.** `arch/arm64/mach-exynos/pm_domains.c` registers a
  platform-bus notifier; on `BUS_NOTIFY_BIND_DRIVER` it reads `samsung,power-domain`
  and calls `__pm_genpd_of_add_device()` (`pm_domains.c:401-430`). The DT already has
  `samsung,power-domain = <&pd_g3d>`. So a platform driver for `mali@0x11400000` is
  put into the power domain automatically; runtime PM then gates it. Panfrost's
  `dev_pm_domain_attach_by_name()` + `device_link_add()` (both absent in 3.10) can be
  deleted from the backport rather than reproduced.
- **Clocks.** kbase's functional clock is `aclk_g3d` (`gpu_exynos7580.c:507`, enabled
  at `:260`); `aclk_g3d_400` is the 400 MHz branch (`:422`); `fin_pll`,
  `fout_g3d_pll`, `mout_g3d`, `dout_aclk_g3d` are DVFS plumbing. Mainline panfrost's
  `clk_get(dev, NULL)` + optional `"bus"` does not match these names, so either the
  DT is renamed at cutover or an Exynos platform-data shim resolves
  `"aclk_g3d"`/`"aclk_g3d_400"` (M0 does the latter).
- **Regulator.** `vdd_g3d` is BUCK4 (600–1300 mV, `regulator-always-on`), requested by
  *name*, not by a DT supply: `regulator_get(NULL, "vdd_g3d")`
  (`gpu_exynos7580.c:711`). M0 matches this and tolerates its absence.
- **Reset.** No reset controller in DT. Reset is `GPU_CMD = SOFT_RESET`, poll
  `GPU_INT_RAWSTAT & RESET_COMPLETED` (kbase `mali_kbase_pm_driver.c:1414-1425`;
  panfrost `panfrost_gpu.c:62-95` with a `HARD_RESET` fallback). M0 implements exactly
  that.
- **Power-up order.** L2 → shaders → tiler, `PWRON` then `READY` poll
  (`panfrost_gpu.c:452-481`); kbase's policy engine is more elaborate but drives the
  same registers in the same dependency order.

### 3.3 MMU

- Four levels × 512 entries × 9 bits per level, 4 KiB granule, index
  `vpfn >> ((3-level)*9)`: kbase `mali_kbase_mmu.c:434-449`; mainline
  `io-pgtable-arm.c:1232-1236` keeps a full 4-level table **regardless of IAS**.
- Leaf = type 1 (`ATE`/`BLOCK`), table = type 3 (`PTE`/`TABLE`); for the Mali format
  even a 4 KiB page is `PTE_TYPE_BLOCK` (`io-pgtable-arm.c:377-390`).
- AF bit 10, RD bit 6, WR bit 7, SH bits 9:8 (OS for this format), XN bit 54,
  cache index bits 4:2, address bits 39:12 (40-bit output address).
- VA/PA widths come from `MMU_FEATURES[7:0]`/`[15:8]` (kbase
  `mali_kbase_gpuprops.c:303-304`; panfrost `panfrost_regs.h:19-20`). The T720's actual
  values must be read on hardware; they define the IOVA window (panfrost reserves
  32 MB..4 GB today).
- `TRANSTAB` = pgd_pa | TABLE(3) | READ_INNER(4), plus SHARE_OUTER(0x10) only for an
  ACE-coherent system. Exynos 7580 has no `system-coherency`/`dma-coherent` property →
  kbase runs `COHERENCY_NONE` (`mali_kbase_core_linux.c:3577`), so the backport must
  use `coherent_walk = false` and do explicit page-table cache maintenance.
- `AS_TRANSCFG = ADRMODE_LEGACY (0)`; both sides agree.
- **Divergence found (risk R4):** the `AS_MEMATTR` tables are *not* the same. kbase
  LPAE: index 0..4 = `0x48, 0x4F, 0x4D, 0x88, 0x8D` → register `0x8D884D4F48`.
  Mainline `ARM_MALI_LPAE`: index 0..2 = `0x88, 0x8D, 0x88` → register `0x888D88`.
  Upstream panfrost works with its own table, but any code that mixes kbase-allocated
  and panfrost-mapped memory would apply different cache policies. The unit test
  asserts both values, so a change on either side is caught.

### 3.4 What a driver must take from kbase, and what it cannot reuse

Take: register map, GPU-ID/product decode, MMU geometry/descriptor bits, memattr
tables, reset/power sequences, coherency rules — all encoded in `mali_hw.h` with
per-constant provenance.

Cannot reuse: kbase exposes **no kernel API** for GPU memory/VM or submission
(`KBASE_EXPORT_TEST_API` compiles to nothing without `MALI_KERNEL_TEST_API`), and the
two drivers cannot share the block (both program `AS_TRANSTAB`, own the job slots and
reset it). The cutover is `CONFIG_MALI_MIDGARD=n`; there is no shim path.

---

## 4. Design and effort (summary; full text in `design.md`)

Design shape: keep **mainline panfrost's UAPI byte-identical**
(`include/uapi/drm/panfrost_drm.h`) so that stock Mesa `panfrost_dri.so` — already
packaged for Alpine aarch64 — is the userspace unmodified. Internally, adapt
panfrost's `device/gpu/mmu/gem/job` to 3.10 with all compatibility shims isolated in
one place, and replace the generic `io-pgtable` layer with the tested 4-level encoder
(the format is proven identical, so a later swap is local).

| Milestone | Content | Effort | Status |
|---|---|---|---|
| **M0** | register/power/MMU bring-up, feature readout, encoder self-test | 4–6 d | **code delivered** (hardware part untested) |
| **M1** | coherent table pool, IOVA allocator, AS programming, MMU IRQ/fault decode, GPU-side page read test | 8–12 d | not started |
| **M2** | DRM device + GEM + `CREATE_BO`/`MMAP_BO`/`GET_PARAM` + dma-buf import/export | 20–30 d | not started |
| **M3** | dma-fence/dma-resv/drm_syncobj + drm_sched + JM submission | 25–40 d | not started |
| **M4** | DVFS (port kbase's Exynos7580 freq/ASV tables to OPP/devfreq), perfcnt ioctls | 8–12 d | not started |
| **M5** | hardening, fault recovery, suspend/resume, stress | 15–25 d | not started |
| **(D)** | DRM/KMS DECON 7580 + DSIM + ea8061 (separate gap; needed for on-screen GL) | 30–50 d | out of scope |

Kernel-only: **80–125 person-days (4–6 PM)**. With display: **~7–10 PM**. This is
lower than the ">12 person-months" figure in `gpu-userspace.md` because that estimate
included porting the generic `io-pgtable` framework, the full modern DRM core and the
display driver; the reduction comes from (a) the purpose-built encoder, (b) deleting
panfrost's genpd/device-link path in favour of the vendor notifier, (c) quoting KMS
separately. Uncertainty ±40 %; hardware access assumed.

Recommended next step (cheap, high information): run M0 on a device to read
`GPU_ID`, `MMU_FEATURES` and `JS_PRESENT` (decides IOVA width), then build M1's
fault-injection path — the first signal that the MMU is programmed correctly.

---

## 5. Risks

Full list with mitigations in `design.md` §5. The ones that matter:

- **R1 (schedule):** dma-fence/resv/syncobj/sched backport is the critical path, not
  the GPU driver.
- **R4 (correctness/perf):** memattr divergence (§3.3); validate with a GPU-read test
  at M1, before Mesa.
- **R5 (correctness):** non-coherent PTW — page-table cache maintenance must be
  explicit or faults are random and irreproducible.
- **R3 (functionality):** actual T720 VA bits may be < 32, shrinking the IOVA window.
- **R7 (product):** panfrost is render-only; without the KMS gap there is no on-screen
  3D, only headless EGL.
- **R8 (deployment):** `CONFIG_MODULES` is unset in the shipping config; production
  must build the driver in-tree.
- **R10 (evidence):** no hardware; only compilation and CPU-side tests are verified.

---

## 6. Delivered artifacts

```
parity/userspace/gpu/panfrost/
├── mali_hw.h              register map, GPU-ID/MMU-feature decode, LPAE bits,
│                          both memattr tables, TRANSTAB encoder (provenance per constant)
├── mali_lpae.h            4-level page-table builder (map/walk/unmap, allocator ops)
├── mali_lpae_test.c       114 checks (index math, descriptors, registers, decode,
│                          map/walk/unmap/leak, memattr divergence)
├── Makefile               `make check` -> native + aarch64/qemu
├── design.md              full design, milestone plan, effort table, risks, DT deltas
└── kmod/
    ├── panfrost7580.c     M0: probe, clocks/regulator, soft/hard reset, power on/off,
    │                      feature readout, in-kernel encoder self-test, debugfs
    ├── Kbuild, Makefile   out-of-tree build against the prepared 3.10 tree
    └── panfrost7580.ko    built artifact (aarch64 relocatable, 3.10.108 vermagic)

parity/patches/userspace/gpu-panfrost/
└── 0001-arm64-dts-exynos7580-add-mali-t720-compatible.patch
```

The patch adds `"arm,mali-t720"` as the **second** compatible string on
`mali@0x11400000` and is verified to apply (`git apply --check` and
`patch --dry-run` both clean). kbase still matches `"arm,mali"`, so there is no
behaviour change until a panfrost driver exists; the full cutover DT delta (clock
names, `power-domains`) is documented in `design.md` §8 as a diff, not applied.

### 6.1 Build and test evidence

Unit tests (host + cross):

```
$ cd parity/userspace/gpu/panfrost && make check
== native ==     114 checks, 0 failure(s)
== aarch64 (qemu) == 114 checks, 0 failure(s)
```

Kernel module against the 3.10 tree (build tree outside the kernel source; the shared
tree was not modified):

```
CC [M]  .../kmod/panfrost7580.o
MODPOST 1 modules
LD [M]  .../kmod/panfrost7580.ko        # ELF64 aarch64 relocatable
vermagic=3.10.108-gb5e363a1 SMP preempt mod_unload aarch64
```

Two environment facts were needed and are worth recording:

1. 3.10 has **no `modules_prepare`** target; `make prepare` + `make scripts` is the
   equivalent. `make scripts` fails on modern GCC (`dtc`: multiple definition of
   `yylloc`), fixed with `HOSTCFLAGS="-fcommon"`.
2. The shipping config has **`CONFIG_MODULES` unset**, so a loadable module cannot be
   built against it at all (`struct module` has no members). Verification used a
   temporary `CONFIG_MODULES=y` in the build directory only. Production should build
   the driver in-tree.

One compiler warning (`-Wmissing-attributes` on `module_init` aliases) originates in
3.10's `include/linux/init.h` under GCC 10 and is suppressed via `KCFLAGS` for the
build; it is not in the delivered code. Every undefined symbol in the `.ko` resolves
to an `EXPORT_SYMBOL` in the tree (`dev_err`/`dev_warn`/`_dev_info` are exported
through the `define_dev_printk_level` macro, `drivers/base/core.c:2032`).

### 6.2 What the module does *not* do

No DRM device, no GEM, no scheduler, no submission, no display. It maps registers,
claims clocks/regulator, resets and powers the block, decodes features, and runs the
page-table encoder in-kernel. It refuses to probe unless `force=1` or kbase is
disabled, because kbase owns the same hardware. It is a bring-up milestone, not a
driver.

---

## 7. Verification status

| Claim | Verified how |
|---|---|
| 3.10 lacks drm_sched / dma-fence / dma-resv / drm_syncobj / drm_gem_shmem / io-pgtable / drm_atomic_helper | file existence + symbol grep in the Nura tree |
| 3.10 lacks `drm_dev_alloc`, gem object funcs, `readl_poll_timeout`, `device_link_add`, `dev_pm_domain_attach*` | symbol grep |
| `drm_mm` (with `color_adjust`) and `iova.c` are present | symbol/file check |
| Register map, descriptor bits, ID/feature decode equal in kbase and mainline | 114 checks with literal expected values and file:line provenance |
| memattr tables differ | asserted as distinct constants |
| DT node contents and the vendor genpd attach path | read from `exynos7580.dtsi` and `arch/arm64/mach-exynos/pm_domains.c` |
| Module compiles/links against 3.10.108 | actual build, vermagic and symbol check |
| Encoder correct on 64-bit ARM | aarch64 static binary under `qemu-aarch64` |
| **GPU behaviour on j7elte** | **not tested — no hardware** |

Open questions that only hardware can answer: `GPU_ID` revision, `MMU_FEATURES`
VA/PA widths, `JS_PRESENT`, shader/tiler/L2 present bitmaps, whether the memattr
choice (R4) is stable under load, and whether `pd-g3d`/`vdd_g3d` sequencing from the
generic driver is as forgiving as kbase's DVFS handler.
