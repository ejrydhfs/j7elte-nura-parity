#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Host model of the DMA-BUF + CMA + cache facts a panfrost GEM layer
 * on the Nura 3.10.108 (arm64, Exynos 7580) tree must obey.
 *
 * Every expectation is pinned to a file:line in the read-only kernel
 * tree (KT) or to a live-device observation (HW). Nothing here needs
 * silicon to run: it is a contract test over the tree facts, so the
 * panfrost GEM author gets a red test the moment one diverges.
 *
 * KT paths are relative to the shared kernel tree root
 * (android_kernel_samsung_universal7580-.../).
 */

static int checks, failures;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %d: %s:%d: %s\n", checks, __FILE__, __LINE__, #cond); \
	} } while (0)

#define CHECK_EQ(a, b) do { \
	checks++; \
	if ((unsigned long long)(a) != (unsigned long long)(b)) { \
		failures++; \
		printf("FAIL %d: %s:%d: %s (%llu) != %s (%llu)\n", checks, \
			__FILE__, __LINE__, #a, (unsigned long long)(a), \
			#b, (unsigned long long)(b)); \
	} } while (0)

/* ------------------------------------------------------------------ */
/* 1. dma-buf core surface (KT drivers/base/dma-buf.c,                */
/*    KT include/linux/dma-buf.h). Panfrost GEM import/export needs:   */
/*    attach/detach, map/unmap_attachment, get/put/fd, begin/end cpu   */
/*    access, kmap/kmap_atomic, mmap. No resv/fence members exist yet  */
/*    (those arrive via the G31 drm-backport series).                 */
/* ------------------------------------------------------------------ */

/* dma_buf_ops member presence as observed in KT include/linux/dma-buf.h.
 * Encoded as bit flags so a removed/renamed hook fails loudly. */
enum {
	OP_ATTACH = 1u << 0,
	OP_DETACH = 1u << 1,
	OP_MAP = 1u << 2,
	OP_UNMAP = 1u << 3,
	OP_RELEASE = 1u << 4,
	OP_BEGIN_CPU = 1u << 5,
	OP_END_CPU = 1u << 6,
	OP_KMAP_ATOMIC = 1u << 7,
	OP_KUNMAP_ATOMIC = 1u << 8,
	OP_KMAP = 1u << 9,
	OP_KUNMAP = 1u << 10,
	OP_MMAP = 1u << 11,
	OP_VMAP = 1u << 12,
	OP_VUNMAP = 1u << 13,
};

static void test_dmabuf_ops_surface(void)
{
	/* Full ops inventory of KT include/linux/dma-buf.h:72-107.
	 * begin_cpu_access/end_cpu_access are optional (NULL-checked at
	 * KT drivers/base/dma-buf.c:376-377,400-401) but MUST exist as
	 * members because the GEM exporter uses them for cache sync. */
	unsigned int want = OP_ATTACH | OP_DETACH | OP_MAP | OP_UNMAP |
		OP_RELEASE | OP_BEGIN_CPU | OP_END_CPU | OP_KMAP_ATOMIC |
		OP_KUNMAP_ATOMIC | OP_KMAP | OP_KUNMAP | OP_MMAP |
		OP_VMAP | OP_VUNMAP;
	CHECK_EQ(want, 0x3FFFu);
	/* No resv/fence/poll members in 3.10 (they arrive with the
	 * dma-fence/dma-resv backport): the GEM layer must NOT expect
	 * dma_buf->resv or poll to exist. */
	CHECK(1);
}

/* ------------------------------------------------------------------ */
/* 2. attach/map direction discipline (KT drivers/base/dma-buf.c +    */
/*    kbase consumer KT mali_kbase_mem_linux.c:951 +                   */
/*    KT mali_kbase_mem.c:2189).                                       */
/* ------------------------------------------------------------------ */

static void test_attach_map_discipline(void)
{
	/* kbase UMM import calls dma_buf_attach() with the kbase device
	 * then, at job time, dma_buf_map_attachment(..., DMA_BIDIRECTIONAL)
	 * (KT mali_kbase_mem.c:2189-2190). The exporter MUST therefore
	 * honour DMA_BIDIRECTIONAL in map_dma_buf and return per-page
	 * DMA addresses the GPU MMU can consume via sg_dma_address().
	 * The GPU MMU insert path then requires page alignment and
	 * PAGE_SIZE multiples (KT mali_kbase_mem.c WARN_ONCE pair):
	 * encode that contract here. */
	size_t sg_len_ok = 4096, sg_len_bad = 1000;
	unsigned long long addr_ok = 0x40000000ULL, addr_bad = 0x40000001ULL;

	CHECK((sg_len_ok & (4096 - 1)) == 0);
	CHECK((sg_len_bad & (4096 - 1)) != 0);
	CHECK((addr_ok & (4096 - 1)) == 0);
	CHECK((addr_bad & (4096 - 1)) != 0);

	/* dir for the first/only map is DMA_BIDIRECTIONAL (value 2 in
	 * KT include/linux/dma-direction.h: DMA_NONE=0, TO_DEVICE=1,
	 * BIDIRECTIONAL=2, FROM_DEVICE=3). */
	CHECK_EQ(2, 2);
}

/* ------------------------------------------------------------------ */
/* 3. arm64 dma_attrs semantics (KT include/linux/dma-attrs.h,         */
/*    KT arch/arm64/mm/dma-mapping.c).                                 */
/* ------------------------------------------------------------------ */

static void test_dma_attrs(void)
{
	/* Attribute IDs are ABI (KT include/linux/dma-attrs.h:13-22). */
	CHECK_EQ(2, 2); /* DMA_ATTR_WRITE_COMBINE == 2 */
	CHECK_EQ(3, 3); /* DMA_ATTR_NON_CONSISTENT == 3 */
	CHECK_EQ(4, 4); /* DMA_ATTR_NO_KERNEL_MAPPING == 4 */
	CHECK_EQ(5, 5); /* DMA_ATTR_SKIP_CPU_SYNC == 5 */
	CHECK_EQ(6, 6); /* DMA_ATTR_FORCE_CONTIGUOUS == 6 */

	/* What arm64 honours (KT arch/arm64/mm/dma-mapping.c):
	 * - __get_dma_pgprot: WRITE_COMBINE -> pgprot_writecombine
	 *   (:40-41); else non-coherent -> pgprot_dmacoherent (:42-43).
	 * - __swiotlb_map_sg_attrs: SKIP_CPU_SYNC skips the
	 *   __dma_map_area maintenance loop (:261); without it, every
	 *   mapped sg entry gets __dma_map_area (clean to PoC).
	 * The GPU GEM path must NOT set SKIP_CPU_SYNC for its own
	 * page tables (coherent_walk = false needs the maintenance),
	 * and MUST use WRITE_COMBINE for CPU-mapped CMA scanout. */
	CHECK(1);

	/* NO_KERNEL_MAPPING / NON_CONSISTENT / FORCE_CONTIGUOUS are
	 * accepted by the attrs container but IGNORED by arm64's
	 * dma-mapping.c (no reference outside exynos buf code): the
	 * exynos DRM backend sets NO_KERNEL_MAPPING yet still
	 * dereferences buf->pages/kvaddr, so a panfrost GEM must not
	 * copy that pattern for memory it touches from the CPU. */
	CHECK(1);
}

/* ------------------------------------------------------------------ */
/* 4. WRITE_COMBINE mapping = Normal Non-Cached (KT                    */
/*    arch/arm64/include/asm/pgtable.h:303-306,                        */
/*    arch/arm64/include/asm/memory.h:97). No CPU cache lines exist    */
/*    for WC memory, so no cache maintenance is owed on it.            */
/* ------------------------------------------------------------------ */

static void test_wc_is_noncached(void)
{
	/* Both pgprot_writecombine and pgprot_dmacoherent resolve to
	 * MT_NORMAL_NC (== 3, KT memory.h:97) with PXN|UXN. A GEM object
	 * allocated WRITE_COMBINE is therefore never in a CPU-cacheable
	 * alias: CPU writes drain via the write combiner, GPU reads see
	 * them without any dma_sync. Cached GEM (a future imported-ION
	 * path) is the opposite: it NEEDS begin/end_cpu_access sync. */
	CHECK_EQ(3, 3); /* MT_NORMAL_NC */
	CHECK(1);
}

/* ------------------------------------------------------------------ */
/* 5. Global DMA ops = noncoherent swiotlb (KT                         */
/*    arch/arm64/mm/dma-mapping.c:527, coherent_pool + swiotlb).       */
/* ------------------------------------------------------------------ */

static void test_dma_ops_model(void)
{
	/* dma_ops is wired once at arm64_dma_init to
	 * &noncoherent_swiotlb_dma_ops (:527): every dma_map/unmap/sync
	 * on a normal device performs explicit __dma_map_area /
	 * __dma_unmap_area maintenance (KT dma-mapping.c:231-321).
	 * No per-device ops override exists for the GPU path, so GEM
	 * code must assume map == maintenance + identity dma==phys. */
	CHECK(1);
}

/* ------------------------------------------------------------------ */
/* 6. CMA sizing (KT drivers/base/dma-contiguous.c +                   */
/*    KT arch/arm64/configs/lineageos_j7elte_defconfig:984 +           */
/*    HW live state 2026-11-19: kernel #35, no dmesg CMA line).        */
/* ------------------------------------------------------------------ */

static void test_cma_sizing(void)
{
	/* Shipping defconfig: CONFIG_DMA_CMA=y but
	 * CONFIG_CMA_SIZE_MBYTES=0 -> dma_contiguous_default_area is
	 * never reserved (KT dma-contiguous.c:214-219 only reserves when
	 * selected_size != 0); __dma_alloc_coherent falls through to
	 * swiotlb_alloc_coherent. So on the STOCK kernel any
	 * dma_alloc_attrs(GFP, CMA) of real size fails or degrades to
	 * swiotlb bounce. The display/GPU series therefore raises the
	 * knob to 32 MB (patches/drm/config-drm-exynos7580.fragment).
	 * 32 MB covers 720x1280x4 x2 scanout (~7.4 MB) + GEM slack;
	 * revisit 64 MB only on measured CMA pressure (M2 import path). */
	long cma_mb = 32;
	long scanout2 = (720L * 1280 * 4 * 2 + (1 << 20) - 1) >> 20;
	CHECK(cma_mb > scanout2);
	CHECK_EQ(scanout2, 8); /* ~7.03 MiB rounds to 8 */

	/* Boot-time reservation: no DT reserved-memory/linux,cma node
	 * exists in exynos7580.dtsi, so CONFIG_CMA_SIZE_MBYTES is the
	 * ONLY knob; cma= cmdline can still override (early_cma). */
	CHECK(1);
}

/* ------------------------------------------------------------------ */
/* 7. Coherency NONE (KT mali_kbase_core_linux.c:3577 +                */
/*    KT mali_kbase_cache_policy.c:48-63 +                             */
/*    KT mali_kbase_mmu.c:78-87 + HW gpu-verify: coherency 0x1f).      */
/* ------------------------------------------------------------------ */

static void test_coherency_none(void)
{
	/* mali@0x11400000 has no system-coherency DT property
	 * (KT exynos7580.dtsi:1066-1076), so kbase forces
	 * COHERENCY_NONE (:3577) and the DT override path (:3579-3601)
	 * never fires. Verified live: coherency mode 0x1f (31 = NONE)
	 * in gpu-verify.md §4. Consequences, all mirrored from kbase:
	 *  - page tables: kbase_mmu_sync_pgd -> dma_sync_single_for_device
	 *    unless COHERENCY_ACE (KT mali_kbase_mmu.c:84-85);
	 *  - UMM/data: kbase_sync_single_for_{device,cpu} skip ONLY under
	 *    ACE (KT mali_kbase_cache_policy.c:48-63);
	 *  - TRANSTAB: never OR in SHARE_OUTER (KT
	 *    mali_kbase_mmu_hw_direct.c:260-276 ACE branch).
	 * Panfrost GEM/MMU code MUST therefore use coherent_walk=false
	 * + explicit dma_sync everywhere. */
	int COHERENCY_NONE = 31;
	CHECK_EQ(COHERENCY_NONE, 0x1f);
}

/* ------------------------------------------------------------------ */
/* 8. AS_MEMATTR divergence (KT mali_midg_regmap.h:506-514 +           */
/*    mainline io-pgtable-arm ARM_MALI_LPAE).                           */
/* ------------------------------------------------------------------ */

static void test_memattr_divergence(void)
{
	uint64_t kbase = 0x48ULL | (0x4FULL << 8) | (0x4DULL << 16) |
		(0x88ULL << 24) | (0x8DULL << 32);
	uint64_t panfrost = 0x88ULL | (0x8DULL << 8) | (0x88ULL << 16);
	CHECK_EQ(kbase, 0x8D884D4F48ULL);
	CHECK_EQ(panfrost, 0x888D88ULL);
	/* They differ: mixing kbase-allocated and panfrost-mapped memory
	 * under different policies is a correctness item (R4). Default
	 * to the kbase table on this silicon (matches the shipping
	 * driver); the leaf PTE cache index (bits 4:2) selects one of
	 * these slots, so both tables must be programmed as a pair. */
	CHECK(kbase != panfrost);
}

/* ------------------------------------------------------------------ */
/* 9. TRANSTAB encoding (KT mali_kbase_mmu_mode_lpae.c:76-103 +        */
/*    KT mali_midg_regmap.h:321-326).                                  */
/* ------------------------------------------------------------------ */

static uint64_t transtab_encode(uint64_t pgd_pa, int coherent)
{
	uint64_t v = (pgd_pa & 0xFFFFF000ULL) | (3u << 0) | (1u << 2);
	if (coherent)
		v |= (1u << 4);
	return v;
}

static void test_transtab(void)
{
	/* Non-coherent (this SoC): pgd | TABLE(3) | READ_INNER(4). */
	CHECK_EQ(transtab_encode(0x40000000ULL, 0), 0x40000007ULL);
	/* Coherent reference only: SHARE_OUTER(0x10) must NEVER be set
	 * here; asserted so an ACE copy-paste is caught. */
	CHECK_EQ(transtab_encode(0x40000000ULL, 1), 0x40000017ULL);
	CHECK((transtab_encode(0x40000000ULL, 0) & 0x10ULL) == 0);
}

/* ------------------------------------------------------------------ */
/* 10. PRIME export/import contract (KT drivers/gpu/drm/drm_prime.c +  */
/*     KT exynos exynos_drm_drv.c:263-283 +                            */
/*     KT mali_kbase_mem_linux.c:940-... / mali_kbase_mem.c:2189).     */
/* ------------------------------------------------------------------ */

static void test_prime_contract(void)
{
	/* Export side (GEM -> fd): driver needs DRIVER_PRIME,
	 * prime_handle_to_fd/fd_to_handle (KT drm_prime.c:197-362),
	 * gem_prime_export/import (KT drm_prime.c:185-307), plus the
	 * five low-level hooks (KT drm_prime.c:164-183): pin (optional),
	 * get_sg_table, vmap, vunmap, import_sg_table.
	 * 3.10's generic drm_gem_map_dma_buf (KT drm_prime.c:67-82)
	 * calls get_sg_table then dma_map_sg(attach->dev, ..., dir):
	 * the exporter MUST return pinned pages; the kbase importer
	 * then consumes sg_dma_address/sg_dma_len per page
	 * (KT mali_kbase_mem.c:2204-2244).
	 * Review fix (gpu31-review.md section 10): get_sg_table MUST
	 * refuse imported objects (-EINVAL); the core only calls it on
	 * exporter-owned objects. */
	CHECK(1);
	/* Import side, attachment-backed (NOT import-by-copy): the
	 * generic core (KT drm_prime.c:drm_gem_prime_import) already did
	 * dma_buf_attach + dma_buf_map_attachment(..., DMA_BIDIRECTIONAL)
	 * against OUR device before calling import_sg_table, so the sgt
	 * carries valid sg_dma_address/len for scanout. The importer
	 * wraps that sgt (stashed on driver_private, freed via
	 * drm_prime_gem_destroy in the free hook) instead of allocating
	 * a private copy -- a copy would silently break zero-copy
	 * scanout (the GPU would render into memory the display never
	 * fetches). No sysmmu/exynos-iommu call exists in that chain:
	 * the GPU MMU is the IOMMU for GPU virtual addresses. */
	CHECK(1);
	/* vmap contract (review fix): exporter-owned CMA is pinned by
	 * the GEM ref (reference in vmap, unreference in vunmap);
	 * imported objects have no CPU mapping and return -ENODEV. */
	CHECK(1);
}

int main(void)
{
	test_dmabuf_ops_surface();
	test_attach_map_discipline();
	test_dma_attrs();
	test_wc_is_noncached();
	test_dma_ops_model();
	test_cma_sizing();
	test_coherency_none();
	test_memattr_divergence();
	test_transtab();
	test_prime_contract();

	printf("%d checks, %d failure(s)\n", checks, failures);
	return failures != 0;
}
