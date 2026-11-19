/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pf7580_as.h - T720 MP2 address-space lifecycle + TLB invalidate + fault
 * decode helper for the panfrost-class port on Exynos 7580 / Nura 3.10.108.
 *
 * Data-free static-inline header: the driver owns its AS bookkeeping and
 * iomem mapping; this file contributes only the exact register sequences,
 * proven against kbase r15p0 (LPAE build, COHERENCY_NONE) and mainline
 * panfrost v5.2. Register offsets/values match code/userspace/gpu/panfrost/
 * mali_hw.h + mali_mmu.h, whose host unit tests (185 checks, native+qemu)
 * are the verification for every value below.
 *
 * AS lifecycle (AS 0 on this port; any present slot uses the same shape):
 *
 *   enable:  TRANSTAB <- pgd | TABLE(3) | READ_INNER(4)   (never SHARE_OUTER
 *              here: system is COHERENCY_NONE, no ACE coherency property)
 *            MEMATTR  <- 0x884D4F48 / 0x00008D88 (kbase LPAE table)
 *            TRANSCFG <- 0 (LEGACY; LPAE build leaves it 0)
 *            wmb, then AS_COMMAND <- UPDATE, gated on !AS_ACTIVE before
 *            and after.
 *   disable: TRANSTAB <- 0 (ADRMODE_UNMAPPED), MEMATTR <- 0, then UPDATE.
 *   map:     write PTEs + sync table pages for the non-coherent PTW, then
 *            LOCK(iova,size) + FLUSH_PT + wait !ACTIVE (+ optional UNLOCK).
 *   unmap:   same shape with FLUSH_PT.
 *   full-TLB: LOCK(0,~0UL) + FLUSH_MEM + wait !ACTIVE.
 *
 * Register order inside enable matches kbase_mmu_hw_configure (non-AARCH64
 * branch: TRANSTAB, MEMATTR, then AS_COMMAND_UPDATE) and panfrost
 * panfrost_mmu_enable (TRANSTAB, MEMATTR, UPDATE). FLUSH_PT (0x04) covers a
 * locked range; FLUSH_MEM (0x05) is the full-context invalidate the
 * io-pgtable tlb_flush_all path uses.
 *
 * T720 r1p0 carries no 9630-class double-UNLOCK erratum (issues list for
 * t72x_r1p0 has no BASE_HW_ISSUE_9630), so the extra double-UNLOCK after
 * LOCK+FLUSH is NOT issued here. (Do not copy it from kbase: that branch
 * is keyed off kbase_hw_has_issue(9630), false on this GPU.)
 *
 * Fault decode: MMU_INT_STAT bit i = page fault in AS i, bit i+16 = bus
 * error in AS i (bus error wins a slot reporting both). AS_FAULTSTATUS
 * low byte: 0xC0-0xC7 translation, 0xC8 permission, 0xD0-0xD7 transtab
 * bus, 0xD8 access flag (LPAE build names); bits 9:8 access type
 * (ATOMIC names UNKNOWN on LPAE), bit 10 decoder(1)/slave(0), bits 31:16
 * source ID; fault VA from AS_FAULTADDR_HI/LO.
 */

#ifndef PF7580_AS_H
#define PF7580_AS_H

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/errno.h>

/* Register block: MMU base 0x2000, AS0 0x400, stride 0x40 (kbase
 * mali_midg_regmap.h:255-301; mainline panfrost_regs.h MMU_BASE 0x2400). */
#define PF7580_AS_BASE		0x2400u
#define PF7580_AS_STRIDE	0x40u
#define PF7580_AS_REG(n, r)	(PF7580_AS_BASE + ((n) * PF7580_AS_STRIDE) + (r))

#define PF7580_AS_TRANSTAB_LO	0x00u
#define PF7580_AS_TRANSTAB_HI	0x04u
#define PF7580_AS_MEMATTR_LO	0x08u
#define PF7580_AS_MEMATTR_HI	0x0Cu
#define PF7580_AS_LOCKADDR_LO	0x10u
#define PF7580_AS_LOCKADDR_HI	0x14u
#define PF7580_AS_COMMAND	0x18u
#define PF7580_AS_FAULTSTATUS	0x1Cu
#define PF7580_AS_FAULTADDR_LO	0x20u
#define PF7580_AS_FAULTADDR_HI	0x24u
#define PF7580_AS_STATUS	0x28u
#define PF7580_AS_TRANSCFG_LO	0x30u
#define PF7580_AS_TRANSCFG_HI	0x34u

#define PF7580_MMU_INT_CLEAR	0x2004u
#define PF7580_MMU_INT_MASK	0x2008u
#define PF7580_MMU_INT_STAT	0x200Cu

#define PF7580_TRANSTAB_TABLE	(3u << 0)
#define PF7580_TRANSTAB_INNER	(1u << 2)
#define PF7580_AS_CMD_UPDATE	0x01u
#define PF7580_AS_CMD_LOCK	0x02u
#define PF7580_AS_CMD_UNLOCK	0x03u
#define PF7580_AS_CMD_FLUSH_PT	0x04u
#define PF7580_AS_CMD_FLUSH_MEM	0x05u
#define PF7580_AS_ACTIVE	0x01u

/* Inactive poll: kbase spins KBASE_AS_INACTIVE_MAX_LOOPS (100k,
 * mali_kbase_defs.h:1682); panfrost polls 10us x 100. Same tens-of-ms
 * contract either way. */
#define PF7580_AS_WAIT_LOOPS	100000
#define PF7580_AS_WAIT_STEP_US	10

/* kbase LPAE MEMATTR table (mali_midg_regmap.h:506-514,
 * mali_kbase_mmu_mode_lpae.c:76-87): the value programmed on this unit.
 * The mainline 0x888D88 alternative is the R4 divergence, not used here. */
#define PF7580_MEMATTR_LO	0x884D4F48u
#define PF7580_MEMATTR_HI	0x00008D88u

/* Region-lock width limits (mali_kbase_mmu_hw_direct.c lock_region). */
#define PF7580_LOCK_MIN_W	11u
#define PF7580_LOCK_MAX_W	63u

static inline unsigned int pf7580_fls32(u32 x)
{
	return x ? 32 - __builtin_clz(x) : 0;
}

/* Encode LOCKADDR for [iova, iova+size): page-aligned base with the
 * pow2-rounded count folded into bits 5:0 as (10 + fls(pages)).
 * Saturates at width 63 for ranges above 2^53 bytes (the LOCK(0,~0UL)
 * full-TLB shape included). */
static inline u64 pf7580_lock_region(u64 iova, u64 size)
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

	if (pages > (1ULL << (PF7580_LOCK_MAX_W - 10))) {
		w = PF7580_LOCK_MAX_W;
	} else {
		u32 p = (u32)pages;

		w = 10 + pf7580_fls32(p);
		if (p != (1u << (w - 11)))
			w += 1;
		if (w < PF7580_LOCK_MIN_W)
			w = PF7580_LOCK_MIN_W;
		if (w > PF7580_LOCK_MAX_W)
			w = PF7580_LOCK_MAX_W;
	}

	region = iova & ~4095ULL;
	region |= w;
	return region;
}

/* Wait for !AS_ACTIVE on slot as_nr. 0 on ready, -ETIMEDOUT when stuck
 * (caller logs GPU base + AS; kbase dev_errors "AS_ACTIVE bit stuck"). */
static inline int pf7580_as_wait_ready(void __iomem *regs,
				       unsigned int as_nr)
{
	unsigned int i;

	for (i = 0; i < PF7580_AS_WAIT_LOOPS; i++) {
		u32 s = readl(regs + PF7580_AS_REG(as_nr,
						   PF7580_AS_STATUS));

		if (!(s & PF7580_AS_ACTIVE))
			return 0;
		udelay(PF7580_AS_WAIT_STEP_US);
	}
	return -ETIMEDOUT;
}

static inline int pf7580_as_write_cmd(void __iomem *regs,
				      unsigned int as_nr, u32 cmd)
{
	int ret = pf7580_as_wait_ready(regs, as_nr);

	if (!ret)
		writel(cmd, regs + PF7580_AS_REG(as_nr, PF7580_AS_COMMAND));
	return ret;
}

/* Program slot as_nr for pgd_dma and broadcast with UPDATE. Caller ensures
 * the slot is present (AS_PRESENT bit) and holds any required locks
 * (kbase: mmu_hw_mutex + hwaccess_lock; panfrost: mmu->lock +
 * hwaccess_lock for the op path). */
static inline int pf7580_as_enable(void __iomem *regs, unsigned int as_nr,
				   u64 pgd_dma)
{
	u32 lo = (u32)((pgd_dma & 0xFFFFF000ULL) | PF7580_TRANSTAB_TABLE |
		      PF7580_TRANSTAB_INNER);
	u32 hi = (u32)(pgd_dma >> 32);
	int ret;

	writel(lo, regs + PF7580_AS_REG(as_nr, PF7580_AS_TRANSTAB_LO));
	writel(hi, regs + PF7580_AS_REG(as_nr, PF7580_AS_TRANSTAB_HI));
	writel(PF7580_MEMATTR_LO, regs + PF7580_AS_REG(as_nr,
						      PF7580_AS_MEMATTR_LO));
	writel(PF7580_MEMATTR_HI, regs + PF7580_AS_REG(as_nr,
						      PF7580_AS_MEMATTR_HI));
	writel(0, regs + PF7580_AS_REG(as_nr, PF7580_AS_TRANSCFG_LO));
	writel(0, regs + PF7580_AS_REG(as_nr, PF7580_AS_TRANSCFG_HI));
	/* Table/memattr writes must be visible before UPDATE. */
	wmb();
	ret = pf7580_as_write_cmd(regs, as_nr, PF7580_AS_CMD_UPDATE);
	if (!ret)
		ret = pf7580_as_wait_ready(regs, as_nr);
	return ret;
}

/* Unbind slot as_nr: TRANSTAB 0 (ADRMODE_UNMAPPED) + MEMATTR 0 + UPDATE
 * (kbase disable_as; panfrost mmu_disable). */
static inline int pf7580_as_disable(void __iomem *regs, unsigned int as_nr)
{
	writel(0, regs + PF7580_AS_REG(as_nr, PF7580_AS_TRANSTAB_LO));
	writel(0, regs + PF7580_AS_REG(as_nr, PF7580_AS_TRANSTAB_HI));
	writel(0, regs + PF7580_AS_REG(as_nr, PF7580_AS_MEMATTR_LO));
	writel(0, regs + PF7580_AS_REG(as_nr, PF7580_AS_MEMATTR_HI));
	wmb();
	return pf7580_as_write_cmd(regs, as_nr, PF7580_AS_CMD_UPDATE);
}

/* Invalidate a mapped range: LOCK(iova,size) + op + wait !ACTIVE.
 * op is FLUSH_PT (map/unmap path) or FLUSH_MEM. Mirrors kbase
 * mmu_hw_do_operation (minus its 9630 double-UNLOCK, absent on r1p0)
 * and panfrost mmu_hw_do_operation. Caller has synced table pages for
 * the non-coherent PTW (dma_sync_single_for_device per updated page). */
static inline int pf7580_as_invalidate(void __iomem *regs,
				       unsigned int as_nr, u64 iova,
				       u64 size, u32 op)
{
	u64 lock = pf7580_lock_region(iova, size);
	int ret;

	writel((u32)lock, regs + PF7580_AS_REG(as_nr,
					       PF7580_AS_LOCKADDR_LO));
	writel((u32)(lock >> 32), regs + PF7580_AS_REG(as_nr,
						       PF7580_AS_LOCKADDR_HI));
	ret = pf7580_as_write_cmd(regs, as_nr, PF7580_AS_CMD_LOCK);
	if (ret)
		return ret;
	ret = pf7580_as_write_cmd(regs, as_nr, op);
	if (ret)
		return ret;
	return pf7580_as_wait_ready(regs, as_nr);
}

/* MMU IRQ pairing: low 16 bits page faults, high 16 bus errors per AS.
 * A slot reporting both decodes as bus error (kbase_mmu_interrupt). */
#define PF7580_MMU_PF(n)	(1u << (n))
#define PF7580_MMU_BE(n)	(1u << ((n) + 16))

static inline int pf7580_irq_is_bus(unsigned int as_nr, u32 irq_stat)
{
	return (irq_stat & PF7580_MMU_BE(as_nr)) != 0;
}

static inline int pf7580_irq_is_page(unsigned int as_nr, u32 irq_stat)
{
	if (pf7580_irq_is_bus(as_nr, irq_stat))
		return 0;
	return (irq_stat & PF7580_MMU_PF(as_nr)) != 0;
}

/* Fault-class names (LPAE build; ATOMIC names UNKNOWN, matching kbase
 * access_type_name without CONFIG_MALI_GPU_MMU_AARCH64). Canonical
 * single decoder (F-MMU4): byte-identical with p7580_mmu_fault_code/
 * atype in iommu/panfrost7580_mmu.c and mali_fault_exc/access_name in
 * userspace mali_mmu.h. */
static inline const char *pf7580_fault_exc(u32 st)
{
	u32 e = st & 0xFFu;

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

static inline const char *pf7580_fault_access(u32 st)
{
	switch (st & (0x3u << 8)) {
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

#endif /* PF7580_AS_H */
