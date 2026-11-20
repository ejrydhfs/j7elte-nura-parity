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

#include <linux/delay.h>
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

/* Encode LOCKADDR for [iova, iova+size): page-aligned base with the
 * pow2-rounded page count folded into bits 5:0 as (10 + fls(pages)).
 * Byte-identical twin of pf7580_lock_region() in mmu/pf7580_as.h and
 * mali_lock_region() in userspace mali_mmu.h (one encoder, F-MMU1/4).
 * Saturates at width 63 for ranges above 2^53 bytes (the LOCK(0,~0UL)
 * full-TLB shape included). */
static unsigned int p7580_fls32(u32 x)
{
	return x ? 32 - __builtin_clz(x) : 0;
}

static u64 p7580_lock_region(u64 iova, u64 size)
{
	u64 pages, region;
	unsigned int w;

	if (!size) {
		pages = 1;
	} else {
		pages = (size + 4095ULL) >> 12;
		if (!pages)
			pages = ~0ULL;
	}

	if (pages > (1ULL << (P7580_LOCK_MAX_W - 10))) {
		w = P7580_LOCK_MAX_W;
	} else {
		u32 p = (u32)pages;

		w = 10 + p7580_fls32(p);
		if (p != (1u << (w - 11)))
			w += 1;
		if (w < P7580_LOCK_MIN_W)
			w = P7580_LOCK_MIN_W;
		if (w > P7580_LOCK_MAX_W)
			w = P7580_LOCK_MAX_W;
	}

	region = iova & ~4095ULL;
	region |= w;
	return region;
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
	p7580_sync_pt(dev, mmu->root_dma);
	p7580_kunmap_dma(tbl);

	return 0;
}

void p7580_mmu_free_table(struct device *dev, dma_addr_t table_dma)
{
	struct page *p;

	if (!dev || !table_dma)
		return;
	p = pfn_to_page(PFN_DOWN(table_dma));
	dma_unmap_page(dev, table_dma, PAGE_SIZE, DMA_BIDIRECTIONAL);
	__free_page(p);
}

void p7580_mmu_free(struct p7580_mmu *mmu)
{
	/* Root only. Caller MUST have emptied the context first via
	 * p7580_mmu_teardown() (recursive unmap+free walk, F-MMU5),
	 * matching kbase's pool-free teardown. Leaks otherwise. */
	if (!mmu || !mmu->dev || !mmu->root_dma)
		return;
	p7580_mmu_free_table(mmu->dev, mmu->root_dma);
	mmu->dev = NULL;
	mmu->root_dma = 0;
}

/* Recursive teardown helper: empty the level-`level` table at `dma`.
 * Levels 0..2 hold PTEs to child tables: snapshot the child address,
 * clear the parent entry first (no reader ever sees a dangling
 * pointer), then recurse. Level-3 entries are leaves (ATE): only
 * cleared — BO pages belong to GEM, never to the table pool. Frees
 * every table except the root (level 0), which stays owned by
 * p7580_mmu_free(). */
static void p7580_teardown_level(struct device *dev, dma_addr_t dma,
				 unsigned int level)
{
	u64 *cpu;
	unsigned int i;

	cpu = p7580_kmap_dma(dma);
	for (i = 0; i < P7580_MMU_ENTRIES; i++) {
		u64 e = cpu[i];
		dma_addr_t child;

		if ((e & P7580_PTE_TYPE_MASK) != P7580_PTE_TYPE_PTE)
			continue;
		child = e & P7580_ADDR_MASK;
		cpu[i] = P7580_PTE_TYPE_INVAL;
		p7580_sync_pt(dev, dma);
		p7580_kunmap_dma(cpu);
		p7580_teardown_level(dev, child, level + 1);
		cpu = p7580_kmap_dma(dma);
	}
	p7580_sync_pt(dev, dma);
	p7580_kunmap_dma(cpu);
	if (level > 0)
		p7580_mmu_free_table(dev, dma);
}

void p7580_mmu_teardown(struct p7580_mmu *mmu)
{
	/* Caller holds the MMU/table lock with the AS already unbound
	 * (pf7580_as_disable / TRANSTAB 0 + UPDATE, waiting !ACTIVE),
	 * so no PTW walk races this free. Wire to file-close/remove;
	 * follow with p7580_mmu_free() for the root. */
	if (!mmu || !mmu->dev || !mmu->root_dma)
		return;
	p7580_teardown_level(mmu->dev, mmu->root_dma, 0);
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

/* Poll AS_STATUS until no command is active (kbase wait_ready,
 * mali_kbase_mmu_hw_direct.c:63-86; no kbdev here so the caller logs
 * the -ETIMEDOUT and escalates to GPU reset). */
static int p7580_as_wait_ready(void __iomem *as)
{
	/* Stepped poll (mmu/pf7580_as.h twin, F-MMU3): 10 us between
	 * reads. The old tight loop held the caller in a hot readl
	 * spin for ~10^5 iterations; on a wedged slot that is a
	 * seconds-long atomic-context-style stall on the caller CPU. */
	unsigned int i;

	for (i = 0; i < P7580_AS_INACTIVE_MAX_LOOPS; i++) {
		if (!(readl(as + P7580_AS_STATUS) & P7580_AS_STATUS_ACTIVE))
			return 0;
		udelay(P7580_AS_WAIT_STEP_US);
	}
	return -ETIMEDOUT;
}

int p7580_mmu_invalidate(void __iomem *gpu_regs, unsigned int as_nr,
			 u64 vpfn, u32 nr, int sync)
{
	void __iomem *as;
	u64 lock_addr;
	int ret;

	if (!gpu_regs || !nr)
		return -EINVAL;
	as = gpu_regs + P7580_MMU_AS_BASE + as_nr * P7580_MMU_AS_STRIDE;

	/* Caller holds the MMU-HW lock (see header contract). Sequence is
	 * kbase_mmu_hw_do_operation (mali_kbase_mmu_hw_direct.c:298-348):
	 * LOCK region -> op -> wait-ready -> UNLOCK. LOCKADDR carries the
	 * width-encoded region for the whole [vpfn, vpfn+nr) range
	 * (F-MMU1); wmb() makes it visible before LOCK (F-MMU2). */
	lock_addr = p7580_lock_region(vpfn << PAGE_SHIFT,
				      (u64)nr << PAGE_SHIFT);
	writel((u32)(lock_addr & 0xFFFFFFFFUL), as + P7580_AS_LOCKADDR_LO);
	writel((u32)((lock_addr >> 32) & 0xFFFFFFFFUL),
	       as + P7580_AS_LOCKADDR_HI);
	/* LOCKADDR must be visible before the LOCK command latches it
	 * (table/memattr discipline in p7580_mmu_program_as above). */
	wmb();
	writel(P7580_AS_CMD_LOCK, as + P7580_AS_COMMAND);
	writel(sync ? P7580_AS_CMD_FLUSH_MEM : P7580_AS_CMD_FLUSH_PT,
	       as + P7580_AS_COMMAND);
	ret = p7580_as_wait_ready(as);
	/* UNLOCK flushes the MMU/uTLB so updated entries are re-read
	 * (kbase BASE_HW_ISSUE_9630 note at :326-344). */
	writel(P7580_AS_CMD_UNLOCK, as + P7580_AS_COMMAND);
	return ret;
}

/* Single fault decoder (F-MMU4), byte-identical with pf7580_fault_exc/
 * access in mmu/pf7580_as.h and mali_fault_exc/access_name in
 * userspace mali_mmu.h: low byte 0xC0-0xC7 translation / 0xC8
 * permission / 0xD0-0xD7 transtab bus / 0xD8 access flag. The old
 * short-code mask (bits 5:3, 0x0..0x5 classes incl. ADDRESS_SIZE and
 * MEMORY_ATTRIBUTES) described a different regmap naming and mislabelled
 * silicon faults; it is retired. */
const char *p7580_mmu_fault_code(u32 status)
{
	u32 e = status & P7580_FAULT_CODE_MASK;

	if (e >= 0xC0u && e <= 0xC7u)
		return "TRANSLATION_FAULT";
	if (e == 0xC8u)
		return "PERMISSION_FAULT";
	if (e >= 0xD0u && e <= 0xD7u)
		return "TRANSTAB_BUS_FAULT";
	if (e == 0xD8u)
		return "ACCESS_FLAG";
	return "UNKNOWN";
}

/* Bits 9:8 access type; ATOMIC names UNKNOWN on this LPAE build
 * (kbase access_type_name without CONFIG_MALI_GPU_MMU_AARCH64). */
const char *p7580_mmu_fault_atype(u32 status)
{
	switch (status & P7580_FAULT_ATYPE_MASK) {
	case (0x1u << 8):
		return "EXECUTE";
	case (0x2u << 8):
		return "READ";
	case (0x3u << 8):
		return "WRITE";
	default:
		return "UNKNOWN";
	}
}
