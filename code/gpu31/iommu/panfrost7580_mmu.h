/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_mmu.h - Mali-T720 GPU-MMU programming for Exynos 7580 (j7elte).
 *
 * Decision (gpu31-iommu slice): the GPU does NOT use the Exynos system MMU /
 * IOMMU API. The Mali-T720 has its own MMU; the driver programs
 * AS_TRANSTAB / AS_MEMATTR / AS_TRANSCFG / AS_COMMAND itself and maps
 * DMA-API addresses (dma_map_page identity: dma_addr == phys, asserted by
 * kbase WARN_ON) directly into GPU page tables. No DT iommus property, no
 * iommu_domain_alloc/attach, no drivers/iommu/exynos-* involvement.
 *
 * Provenance of every constant is cited as file:line in the Nura 3.10.108
 * tree (KT) or mainline panfrost behaviour (ML).
 */
#ifndef PANFROST7580_MMU_H
#define PANFROST7580_MMU_H

#include <linux/types.h>

struct device;
struct page;

/* 4 levels x 512 entries x 9 bits, 4 KiB granule (KT
 * drivers/gpu/arm/t7xx/r15p0/mali_kbase_mmu_mode_lpae.c:25-35,
 * mali_kbase_mmu.c:386-428 walk: vpfn >> ((3-level)*9), & 0x1FF). */
#define P7580_MMU_LEVELS	4
#define P7580_MMU_ENTRIES	512
#define P7580_MMU_ADDR_SHIFT	12

/* LPAE entry encoding (KT mali_kbase_mmu_mode_lpae.c:25-35). */
#define P7580_PTE_TYPE_MASK	3ULL
#define P7580_PTE_TYPE_ATE	1ULL
#define P7580_PTE_TYPE_INVAL	2ULL
#define P7580_PTE_TYPE_PTE	3ULL

#define P7580_PTE_ATTR_SHIFT	2	/* bits 4:2: memattr index */
#define P7580_PTE_RD_BIT	(1ULL << 6)
#define P7580_PTE_WR_BIT	(1ULL << 7)
#define P7580_PTE_SH_SHIFT	8	/* bits 9:8, 0 = non-shareable */
#define P7580_PTE_AF_BIT	(1ULL << 10)
#define P7580_PTE_NX_BIT	(1ULL << 54)

/* AS register block: kbase MMU base 0x2000 (mali_midg_regmap.h:255) +
 * AS0 0x400 (:263) == panfrost MMU_BASE 0x2400, stride 0x40 (:280). */
#define P7580_MMU_AS_BASE	0x2400
#define P7580_MMU_AS_STRIDE	0x40
#define P7580_AS_TRANSTAB_LO	0x00
#define P7580_AS_TRANSTAB_HI	0x04
#define P7580_AS_MEMATTR_LO	0x08
#define P7580_AS_MEMATTR_HI	0x0C
#define P7580_AS_LOCKADDR_LO	0x10
#define P7580_AS_LOCKADDR_HI	0x14
#define P7580_AS_COMMAND	0x18
#define P7580_AS_FAULTSTATUS	0x1C
#define P7580_AS_FAULTADDR_LO	0x20
#define P7580_AS_FAULTADDR_HI	0x24
#define P7580_AS_STATUS		0x28
#define P7580_AS_STATUS_ACTIVE	0x01
#define P7580_AS_TRANSCFG_LO	0x30

#define P7580_AS_CMD_UPDATE	0x01
#define P7580_AS_CMD_LOCK	0x02
#define P7580_AS_CMD_UNLOCK	0x03
#define P7580_AS_CMD_FLUSH_PT	0x04
#define P7580_AS_CMD_FLUSH_MEM	0x05
/* AS_ACTIVE poll bound (KT mali_kbase_defs.h:1682
 * KBASE_AS_INACTIVE_MAX_LOOPS). */
#define P7580_AS_INACTIVE_MAX_LOOPS 100000

/* TRANSTAB value (KT mali_kbase_mmu_mode_lpae.c:89-97,
 * mali_midg_regmap.h:321-326): pgd | TABLE(3) | READ_INNER(4).
 * SHARE_OUTER is set only for COHERENCY_ACE; this SoC is COHERENCY_NONE
 * (KT mali_kbase_core_linux.c:3577), so it is never set here. */
#define P7580_TRANSTAB_FLAGS	(3u | (1u << 2))
#define P7580_TRANSTAB_ADDR_MASK 0xfffff000u
/* transcfg legacy 0 (KT mali_kbase_mmu_mode_lpae.c:99-103, both sides
 * agree per gpu-panfrost.md 3.3; mainline panfrost writes LEGACY too). */
#define P7580_TRANSCFG_LEGACY	0u

/*
 * AS_MEMATTR tables diverge (risk R4, gpu-panfrost.md 3.3):
 * kbase LPAE indices 0..4 = 0x48,0x4F,0x4D,0x88,0x8D
 *   (KT mali_midg_regmap.h:506-514, mali_kbase_mmu_mode_lpae.c:76-87)
 * mainline ARM_MALI_LPAE indices 0..2 = 0x88,0x8D,0x88.
 * Default to the kbase table (matches the shipping driver on this
 * exact silicon); index 0 = implementation-defined caching, which is
 * also kbase's default region policy.
 */
#define P7580_MEMATTR_KBASE_LO	0x884D4F48u
#define P7580_MEMATTR_KBASE_HI	0x00008D88u
#define P7580_MEMATTR_PANFROST	0x00888D88u
#define P7580_MEMATTR_IDX_DEFAULT 0u

/* Fault status decode (KT mali_midg_regmap.h:341-353). */
#define P7580_FAULT_CODE_MASK	(0x7u << 3)
#define P7580_FAULT_ATYPE_MASK	(0x3u << 8)

/* Locking contract (mirrors kbase: mmu_lock serialises table walks,
 * mmu_hw_mutex serialises AS register programming):
 * - alloc/free/map/unmap: caller MUST hold the driver's MMU/table lock.
 *   No internal locking is provided (same as kbase_mmu_* paths, which
 *   assert lockdep_assert_held on entry).
 * - program_as/invalidate: caller MUST hold the driver's MMU-HW lock
 *   (serialises AS_LOCKADDR/LOCK/UNLOCK/FLUSH sequences against
 *   concurrent AS programming and IRQ-driven recovery).
 * One GPU address space binding. Page tables come from the kernel page
 * allocator via dma_map_page (identity on this arm64 tree: dma == phys);
 * explicit dma_sync_single_for_device after every update because the
 * system is COHERENCY_NONE (KT mali_kbase_mmu.c:78-87). */
struct p7580_mmu {
	struct device *dev;
	dma_addr_t root_dma;
};

/* prot bits for p7580_mmu_map(): */
#define P7580_PROT_RD		(1u << 0)
#define P7580_PROT_WR		(1u << 1)
#define P7580_PROT_XN		(1u << 2)

int p7580_mmu_alloc(struct device *dev, struct p7580_mmu *mmu);
/* Free one child table page (dma_unmap + __free_page) after its parent
 * entry has been cleared. Exported for the M2/M3 teardown walker; NOT
 * for general use. */
void p7580_mmu_free_table(struct device *dev, dma_addr_t table_dma);
/* Free the root only; caller MUST have unmapped every mapping AND freed
 * every child table via p7580_mmu_free_table (no recursive walk here,
 * matching kbase's pool-free teardown). Leaks otherwise. */
void p7580_mmu_free(struct p7580_mmu *mmu);
/* Map one 4 KiB page (both addresses 4K-aligned). memattr_idx 0..7.
 * May allocate up to 3 child tables (GFP_KERNEL, may sleep). */
int p7580_mmu_map(struct p7580_mmu *mmu, u64 iova, phys_addr_t paddr,
		  unsigned int prot, unsigned int memattr_idx);
void p7580_mmu_unmap(struct p7580_mmu *mmu, u64 iova);
/* Invalidate GPU TLBs for [vpfn, vpfn+nr) in address space as_nr.
 * LOCK region -> op (FLUSH_PT async / FLUSH_MEM sync+drain) -> wait-ready
 * -> UNLOCK, mirroring kbase_mmu_hw_do_operation
 * (mali_kbase_mmu_hw_direct.c:298-348). sync != 0 drains prior accesses
 * (kbase sync path); sync == 0 is the map/unmap fast path.
 * Returns 0 on ready, -ETIMEDOUT if the AS fails to go ready (caller
 * escalates to GPU reset, as kbase does at mali_kbase_mmu.c:873-886). */
int p7580_mmu_invalidate(void __iomem *gpu_regs, unsigned int as_nr,
			 u64 vpfn, u32 nr, int sync);
/* Program address space as_nr off a GPU register base (gpu regs + offsets
 * above); issues AS_COMMAND_UPDATE. Caller holds the MMU-HW lock. */
void p7580_mmu_program_as(void __iomem *gpu_regs, unsigned int as_nr,
			  dma_addr_t pgd_dma, u32 memattr_lo, u32 memattr_hi);
/* Human-readable fault status, for the MMU IRQ handler log path. */
const char *p7580_mmu_fault_code(u32 status);
const char *p7580_mmu_fault_atype(u32 status);

#endif /* PANFROST7580_MMU_H */
