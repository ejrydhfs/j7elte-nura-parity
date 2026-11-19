/* SPDX-License-Identifier: GPL-2.0 */
/*
 * panfrost7580_mmu.c - Mali-T720 GPU-MMU programming, Exynos 7580 / 3.10.108.
 *
 * No exynos-sysmmu / IOMMU-API use: the Mali MMU *is* the IOMMU for GPU
 * virtual addresses. Pages are dma_map_page()d (identity dma==phys on this
 * arm64 tree) and their DMA addresses are written into the GPU's own LPAE
 * tables; each update is followed by dma_sync_single_for_device() because
 * the GPU is observed non-coherent (COHERENCY_NONE).
 *
 * Mirrors the proven kbase paths:
 *  alloc/free  <- kbase_mmu_alloc_pgd (mali_kbase_mmu.c:386-428)
 *  maintenance <- kbase_mmu_sync_pgd  (mali_kbase_mmu.c:78-87)
 *  AS program  <- kbase_mmu_hw_configure + mmu_get_as_setup
 *                 (mali_kbase_mmu_hw_direct.c:242-288,
 *                  mali_kbase_mmu_mode_lpae.c:76-103, ACE branch dropped)
 *  entries     <- entry_set_ate/pte/invalidate
 *                 (mali_kbase_mmu_mode_lpae.c:174-189)
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include "panfrost7580_mmu.h"

#define P7580_IDX_MASK		0x1FFu
#define P7580_ADDR_MASK		(~(u64)0xFFFULL)

static u64 p7580_index(u64 iova, unsigned int level)
{
	return (iova >> (P7580_MMU_ADDR_SHIFT +
			 (P7580_MMU_LEVELS - 1 - level) * 9)) & P7580_IDX_MASK;
}

static u64 *p7580_kmap_dma(dma_addr_t dma)
{
	return kmap(pfn_to_page(PFN_DOWN(dma)));
}

static void p7580_kunmap_dma(void *cpu)
{
	kunmap(cpu);
}

/* Sync one page-table page so the non-coherent GPU PTW reads fresh data. */
static void p7580_sync_pt(struct device *dev, dma_addr_t dma)
{
	dma_sync_single_for_device(dev, dma, PAGE_SIZE, DMA_TO_DEVICE);
}

int p7580_mmu_alloc(struct device *dev, struct p7580_mmu *mmu)
{
	struct page *p;
	u64 *tbl;
	unsigned int i;

	if (!dev || !mmu)
		return -EINVAL;

	p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!p)
		return -ENOMEM;

	mmu->root_dma = dma_map_page(dev, p, 0, PAGE_SIZE,
				     DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, mmu->root_dma)) {
		__free_page(p);
		return -ENOMEM;
	}

	/* Same guarantee kbase relies on (mali_kbase_mem_pool.c:211:
	 * WARN_ON(dma_addr != page_to_phys(p))): an identity mapping, so
	 * the GPU PTE can carry the DMA address. Page tables written back
	 * below are also covered by this; fail loudly if it ever moves. */
	if (mmu->root_dma != (dma_addr_t)page_to_phys(p)) {
		dev_warn(dev, "panfrost7580: DMA address is not identity-mapped\n");
		dma_unmap_page(dev, mmu->root_dma, PAGE_SIZE,
			       DMA_BIDIRECTIONAL);
		__free_page(p);
		return -EIO;
	}

	mmu->dev = dev;
	tbl = p7580_kmap_dma(mmu->root_dma);
	/* Mark every entry invalid; 0x2 == ENTRY_IS_INVAL (lpae.c:186-189). */
	for (i = 0; i < P7580_MMU_ENTRIES; i++)
		tbl[i] = P7580_PTE_TYPE_INVAL;
	mmu->root_cpu = tbl;
	p7580_sync_pt(dev, mmu->root_dma);
	p7580_kunmap_dma(tbl);
	mmu->root_cpu = NULL;

	return 0;
}

void p7580_mmu_free(struct p7580_mmu *mmu)
{
	struct page *p;

	if (!mmu || !mmu->dev || !mmu->root_dma)
		return;

	/* Caller must have unmapped all mappings (no recursive table walk
	 * here: tables are a per-context resource freed at context
	 * teardown, matching kbase's pool-free path). */
	p = pfn_to_page(PFN_DOWN(mmu->root_dma));
	dma_unmap_page(mmu->dev, mmu->root_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
	__free_page(p);
	mmu->dev = NULL;
	mmu->root_dma = 0;
}

/* Allocate + attach one child table; table page identity-checked as root. */
static int p7580_get_child(struct device *dev, u64 *entry, dma_addr_t *child)
{
	struct page *p;
	u64 *tbl;
	unsigned int i;
	dma_addr_t dma;

	if ((*entry & P7580_PTE_TYPE_MASK) == P7580_PTE_TYPE_PTE) {
		*child = (*entry) & P7580_ADDR_MASK;
		return 0;
	}

	p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!p)
		return -ENOMEM;

	dma = dma_map_page(dev, p, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, dma)) {
		__free_page(p);
		return -ENOMEM;
	}
	if (dma != (dma_addr_t)page_to_phys(p)) {
		dev_warn(dev, "panfrost7580: child PT not identity-mapped\n");
		dma_unmap_page(dev, dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
		__free_page(p);
		return -EIO;
	}

	tbl = p7580_kmap_dma(dma);
	for (i = 0; i < P7580_MMU_ENTRIES; i++)
		tbl[i] = P7580_PTE_TYPE_INVAL;
	p7580_sync_pt(dev, dma);
	p7580_kunmap_dma(tbl);

	*entry = (dma & P7580_ADDR_MASK) | P7580_PTE_TYPE_PTE;
	*child = dma;
	return 1; /* parent entry changed -> caller syncs parent page */
}

int p7580_mmu_map(struct p7580_mmu *mmu, u64 iova, phys_addr_t paddr,
		  unsigned int prot, unsigned int memattr_idx)
{
	dma_addr_t cur = 0;
	unsigned int l;

	if (!mmu || !mmu->dev || !mmu->root_dma)
		return -EINVAL;
	if ((iova & ~PAGE_MASK) || (paddr & ~PAGE_MASK))
		return -EINVAL;
	if (memattr_idx > 7)
		return -EINVAL;

	/* Walk levels 0..2, allocating child tables on demand. Each level
	 * writes at most one entry into its parent page and syncs that
	 * parent page so the non-coherent PTW sees it (kbase_mmu_sync_pgd
	 * discipline, mali_kbase_mmu.c:78-87). */
	cur = mmu->root_dma;
	for (l = 0; l < P7580_MMU_LEVELS - 1; l++) {
		u64 *pcpu = p7580_kmap_dma(cur);
		u64 ei = p7580_index(iova, l);
		u64 e = pcpu[ei];
		dma_addr_t child = 0;
		int rc = p7580_get_child(mmu->dev, &e, &child);

		if (rc < 0) {
			p7580_kunmap_dma(pcpu);
			return rc;
		}
		if (rc > 0) {
			pcpu[ei] = e;
			p7580_sync_pt(mmu->dev, cur);
		}
		p7580_kunmap_dma(pcpu);
		cur = child;
	}

	/* Level 3 leaf: 4 KiB ATE (ENTRY_IS_ATE, lpae.c:174-179). */
	{
		u64 *cpu = p7580_kmap_dma(cur);
		u64 idx = p7580_index(iova, P7580_MMU_LEVELS - 1);
		u64 leaf = (paddr & P7580_ADDR_MASK) | P7580_PTE_TYPE_ATE |
			((u64)(memattr_idx & 0x7) << P7580_PTE_ATTR_SHIFT) |
			P7580_PTE_AF_BIT;

		if (prot & P7580_PROT_RD)
			leaf |= P7580_PTE_RD_BIT;
		if (prot & P7580_PROT_WR)
			leaf |= P7580_PTE_WR_BIT;
		if (prot & P7580_PROT_XN)
			leaf |= P7580_PTE_NX_BIT;
		cpu[idx] = leaf;
		p7580_sync_pt(mmu->dev, cur);
		p7580_kunmap_dma(cpu);
	}

	return 0;
}

void p7580_mmu_unmap(struct p7580_mmu *mmu, u64 iova)
{
	dma_addr_t cur;
	unsigned int l;
	u64 *cpu;

	if (!mmu || !mmu->dev || !mmu->root_dma)
		return;
	if (iova & ~PAGE_MASK)
		return;

	cur = mmu->root_dma;
	for (l = 0; l < P7580_MMU_LEVELS - 1; l++) {
		u64 e;

		cpu = p7580_kmap_dma(cur);
		e = cpu[p7580_index(iova, l)];
		p7580_kunmap_dma(cpu);
		if ((e & P7580_PTE_TYPE_MASK) != P7580_PTE_TYPE_PTE)
			return; /* nothing mapped on this path */
		cur = e & P7580_ADDR_MASK;
	}

	cpu = p7580_kmap_dma(cur);
	cpu[p7580_index(iova, P7580_MMU_LEVELS - 1)] = P7580_PTE_TYPE_INVAL;
	p7580_sync_pt(mmu->dev, cur);
	p7580_kunmap_dma(cpu);
}

void p7580_mmu_program_as(void __iomem *gpu_regs, unsigned int as_nr,
			  dma_addr_t pgd_dma, u32 memattr_lo, u32 memattr_hi)
{
	void __iomem *as = gpu_regs + P7580_MMU_AS_BASE +
		as_nr * P7580_MMU_AS_STRIDE;
	u32 transtab_lo = (pgd_dma & P7580_TRANSTAB_ADDR_MASK) |
		P7580_TRANSTAB_FLAGS;
	u32 transtab_hi = (u32)((u64)pgd_dma >> 32);

	/* Order matches kbase_mmu_hw_configure (non-AArch64 branch):
	 * TRANSTAB, MEMATTR, then AS_COMMAND_UPDATE. transcfg legacy 0. */
	writel(transtab_lo, as + P7580_AS_TRANSTAB_LO);
	writel(transtab_hi, as + P7580_AS_TRANSTAB_HI);
	writel(memattr_lo, as + P7580_AS_MEMATTR_LO);
	writel(memattr_hi, as + P7580_AS_MEMATTR_HI);
	writel(P7580_TRANSCFG_LEGACY, as + P7580_AS_TRANSCFG_LO);
	/* Ensure table/memattr writes are visible before UPDATE. */
	wmb();
	writel(P7580_AS_CMD_UPDATE, as + P7580_AS_COMMAND);
}

const char *p7580_mmu_fault_code(u32 status)
{
	switch (status & P7580_FAULT_CODE_MASK) {
	case (0x0 << 3):
		return "TRANSLATION_FAULT";
	case (0x1 << 3):
		return "PERMISSION_FAULT";
	case (0x2 << 3):
		return "TRANSTAB_BUS_FAULT";
	case (0x3 << 3):
		return "ACCESS_FLAG";
	case (0x4 << 3):
		return "ADDRESS_SIZE_FAULT";
	case (0x5 << 3):
		return "MEMORY_ATTRIBUTES_FAULT";
	default:
		return "UNKNOWN";
	}
}

const char *p7580_mmu_fault_atype(u32 status)
{
	switch (status & P7580_FAULT_ATYPE_MASK) {
	case (0x0 << 8):
		return "ATOMIC";
	case (0x1 << 8):
		return "EXECUTE";
	case (0x2 << 8):
		return "READ";
	case (0x3 << 8):
		return "WRITE";
	default:
		return "UNKNOWN";
	}
}
