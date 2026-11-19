# GPU31 IOMMU path — decision and patch (Mali-T720, Exynos 7580, 3.10.108)

Status: **decision made and compile-checked.** Device live state could not be
re-read from this slice (the shared `gpsdev.lock` was held by the GPS/FM
workstreams during this window; no reboot, no flash, no device command was
issued — live bring-up state is owned by `G31_Report`/`gpu31-master.md`).
All tree citations below are file:line in the shared Nura tree (KT);
no kernel tree was modified — patches live under `code/gpu31/iommu/`.

Related: `docs/reports/gpu-panfrost.md` (M0 + audit), `docs/reports/gpu-drm.md`
(M1–M3 + `patches/drm-backport`), `patches/iommu-dma/README.md` (CMA/DMA +
PRIME analysis this slice builds on), `docs/reports/gpu31-target.md`
(sibling slice; upstream v5.2 `panfrost_mmu.c/.h` listed as adapted-from
reference — boundary agreed: target owns no `.c` files, this slice owns
`drivers/gpu/drm/panfrost/panfrost7580_mmu.c/.h`).

## 1. Decision

**Operate panfrost with its own GPU MMU + DMA-API direct mappings
(`dma_map_page`, identity `dma == phys`); do NOT use the IOMMU API over an
exynos-sysmmu adapter.**

No `iommu_domain_alloc`/`iommu_attach_device`/`iommu_map`, no
`drivers/iommu/exynos-*` calls, no new DT `iommus` property on the GPU node,
no `CONFIG_*IOMMU*` change. Rationale, each point grounded:

1. The T720's MMU *is* its IOMMU. Address-space programming is
   `AS_TRANSTAB_LO/HI + AS_MEMATTR_LO/HI (+ AS_TRANSCFG_LO)` then
   `AS_COMMAND_UPDATE` (`mali_kbase_mmu_hw_direct.c:242-288`, non-AArch64
   branch). kbase proves the whole path on this exact silicon (§2).
2. The GPU has no sysmmu in DT: `mali@0x11400000` in
   `arch/arm64/boot/dts/exynos7580.dtsi:1066-1075` carries `reg / interrupts /
   clocks / samsung,power-domain` and **no** `iommus`/`iommu` property; the
   four `iommu-domain_*` buses cover fimc-is/decon/scaler-jpeg/mfc only
   (`:1455/:1535/:1560/:1587`), each with its own `sysmmu@...` node.
   There is no `sysmmu@114xxxxx` to bind, and wiring one would be a DT +
   driver invention with no hardware counterpart (stock `dt.img` is never
   rebuilt per project rule).
3. The exynos-sysmmu page format does not match the GPU PTW: sysmmu-v6 uses
   1 MiB/64 KiB sections + 4 KiB small pages (`exynos-iommu.h:26-45`,
   `NUM_LV1ENTRIES 4096`), while the Mali walker is 4×512×9-bit 4 KiB
   granule with `ATE=1/PTE=3` leaves and AF/RD/WR/SH/XN bits
   (`mali_kbase_mmu_mode_lpae.c:25-35,76-103,174-189`). An IOMMU-API adapter
   would translate between two unrelated formats for zero benefit.
4. The proven kbase chain already does exactly the chosen design:
   per-context 4-level tables from `kbase_mem_pool` pages
   (`mali_kbase_mmu.c:386-428`), per-update `dma_sync_single_for_device`
   under `COHERENCY_NONE` (`mali_kbase_mmu.c:78-87`), `TRANSTAB = pgd &
   mask | TABLE | READ_INNER` with legacy `transcfg = 0`
   (`mali_kbase_mmu_mode_lpae.c:76-103`), leaf entries via
   `entry_set_ate/pte/invalidate` (`:174-189`). The delivered helper is a
   line-by-line mirror of these four sites, so behaviour on this SoC is
   inherited, not invented.
5. Rejected alternative (sysmmu adapter): would need (a) a fictitious GPU
   sysmmu DT node + stream-ID mapping that does not exist in hardware,
   (b) double translation (sysmmu IOVA → phys → GPU PTE) with two TLBs to
   keep coherent, (c) `CONFIG_EXYNOS_IOMMU_V6` + IOVMM churn on the GPU
   fault path. Cost with no correctness or performance gain; adds the very
   DT rebuild the project forbids.

## 2. What the patch provides

`code/gpu31/iommu/` (reviewable sources + integrator-ready series):

| File | Content |
|---|---|
| `panfrost7580_mmu.h` (116 lines) | AS register offsets/strides, TRANSTAB/MEMATTR/TRANSCCFG constants, both memattr tables (kbase `0x8D884D4F48` default + upstream `0x888D88` for A/B), fault-code decode, `struct p7580_mmu` + map/unmap/program API |
| `panfrost7580_mmu.c` (301 lines) | `p7580_mmu_alloc/free` (identity-checked `dma_map_page`, `dma_mapping_error` handling), `p7580_mmu_map/unmap` (4-level walk, `dma_sync_single_for_device` after every parent/leaf write), `p7580_mmu_program_as` (TRANSTAB/MEMATTR/TRANSCCFG + `wmb()` + `UPDATE`), `p7580_mmu_fault_code/atype` |
| `0001-panfrost7580-mmu-helper.patch` | New files → `drivers/gpu/drm/panfrost/panfrost7580_mmu.{c,h}` (byte-identical to staged sources, `cmp` clean) |
| `0002-panfrost7580-build-wiring.patch` | `drivers/gpu/drm/{Makefile,Kconfig}` + new `drivers/gpu/drm/panfrost/{Kconfig,Makefile}` behind `CONFIG_DRM_PANFROST7580` (default n, `depends on DRM && ARM64 && MMU`) |

Deliberately NOT in this slice: GEM object binding, dma-buf/sg import into
GPU PTEs, MMU IRQ registration, fault-recovery policy, DVFS/OPP, any
`CONFIG_MALI_MIDGARD` cutover (kbase keeps the block until the M2/M3 driver
lands — both drivers program `AS_TRANSTAB` and cannot coexist).

## 3. Compile evidence (host-side, read-only tree)

- `aarch64-linux-gnu-gcc -fsyntax-only -std=gnu89 -nostdinc` + real `-c`
  object compile vs actual 3.10.108 headers (`arch/arm64/include`,
  `arch/arm64/include/generated`, `include`, `include/uapi`,
  `arch/arm64/include/uapi`, `__KERNEL__`, `linux/kconfig.h`): **both PASS,
  zero errors** (only pre-existing in-tree header warnings under `-Wall`).
- Object `panfrost7580_mmu.o` 11840 B exports exactly
  `p7580_mmu_{alloc,free,map,unmap,program_as,fault_code,fault_atype}`;
  undefined refs are kernel-provided (`dma_ops`, `dev_warn`,
  `alloc/free_pages`, `contig_page_data`, `memstart_addr`) — all resolved
  at kernel link, same class as the `drm-backport` series objects.
- `git apply --check` PASS for 0001+0002 individually and as a series in a
  private tree seeded from the shared tree's `drivers/gpu/drm/{Makefile,
  Kconfig}`; post-apply files byte-identical to staged sources.
- NOT run (integrator-owned): full `Image` link, boot, `/dev/dri` or
  GPU-read tests. Next hardware signal (M1 fault-injection path per
  `gpu-panfrost.md` §4): program AS0 with the helper, map one page,
  read `AS_FAULTSTATUS`/`AS_FAULTADDRESS` — expect `TRANSLATION_FAULT`
  on unmapped access, silence on mapped.

## 4. Coherency + memattr rules the M2/M3 driver MUST follow

- `system_coherency` is forced `COHERENCY_NONE`
  (`mali_kbase_core_linux.c:3577`); the only override is a DT
  `system-coherency` property (`:3579-3601`) which the mali node lacks.
  So: `coherent_walk = false`, explicit `dma_sync_*` everywhere
  (page tables via `p7580_sync_pt`; GEM payloads via the
  `mali_kbase_mem.c:986-1023` / `mali_kbase_cache_policy.c:48-63`
  discipline), never set `AS_TRANSTAB_LPAE_SHARE_OUTER`
  (`mali_kbase_mmu_hw_direct.c:260` ACE branch stays dropped).
- `AS_MEMATTR` default `0x8D884D4F48` (kbase table,
  `mali_midg_regmap.h:506-514`, setup at `mali_kbase_mmu_mode_lpae.c:76-87`).
  Upstream `0x888D88` differs (risk R4, `gpu-panfrost.md` §3.3); keep the
  kbase value on this silicon and validate with a GPU-read test at M1
  before Mesa runs.
- VA width comes from `MMU_FEATURES[7:0]` (live value 33 bits per
  `gpu-verify.md` kbase props: `mmu_features 0x2821`); size the IOVA window
  from silicon (`panfrost7580_mmu.h` leaves policy to the caller — the
  helper maps single pages and enforces 4 K alignment + `memattr_idx ≤ 7`).

## 5. Risks

- **R-A (identity assumption):** `dma_map_page == page_to_phys` holds on
  this tree today (kbase `WARN_ON`s it, `mali_kbase_mem_pool.c:211`) and
  the helper fails closed (`-EIO` + unmap + free) if it ever breaks
  (swiotlb/CMA pressure, future `dma_ops` change). Residual: re-verify the
  assertion on first M1 boot; if it fires, the fallback is `sg`-walked
  PTE fills, not a sysmmu.
- **R-B (R4 memattr):** wrong cache policy shows up as GPU-read garbage
  under load, not as a fault — the dangerous kind. Mitigation: M1 GPU-read
  test with both tables before Mesa; keep `P7580_MEMATTR_PANFROST`
  defined for the A/B run.
- **R-C (R5 PTW visibility):** a missed `dma_sync` gives random,
  irreproducible faults. The helper syncs every parent and leaf write;
  audit any future fast-path against `kbase_mmu_sync_pgd` (`:78-87`).
- **R-D (AS lifetime):** free path does not walk children (context-teardown
  contract, like kbase's pool free). The M2/M3 driver must unmap all before
  `p7580_mmu_free` or table pages leak — add a `WARN_ON` mapped-count in
  the GEM layer, not here.
- **R-E (coexistence):** cutover is `CONFIG_MALI_MIDGARD=n` at M2/M3;
  probing this helper's AS programming while kbase owns the block hangs
  the GPU. The build symbol defaults to n; never enable alongside kbase.

## 6. Integrator commands (NOT run — build/flash owned by integrator)

```sh
git apply --check code/gpu31/iommu/0001-panfrost7580-mmu-helper.patch \
  code/gpu31/iommu/0002-panfrost7580-build-wiring.patch
# then: apply, enable CONFIG_DRM_PANFROST7580=m in a test config,
# serialised Image build (code/kernel/build.sh), boot, M1 fault test §3.
# Never: make in the shared tree from a slice; flash without go-ahead;
# reboot the phone (GPS/FM share it); write big files outside /data.
```
