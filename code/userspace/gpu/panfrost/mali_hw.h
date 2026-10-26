/*
 * mali_hw.h - Mali Midgard (T7xx) register map, GPU-ID and MMU-feature
 *             decoding, and LPAE descriptor encodings shared between the
 *             userspace test harness and the 3.10 kernel milestone driver.
 *
 * Every constant carries its provenance:
 *   [kb]      drivers/gpu/arm/t7xx/r15p0/mali_midg_regmap.h
 *             drivers/gpu/arm/t7xx/r15p0/mali_kbase_gpu_id.h
 *             drivers/gpu/arm/t7xx/r15p0/mali_kbase_mmu_mode_lpae.c
 *   [pf]      linux-7.3/drivers/gpu/drm/panfrost/panfrost_regs.h
 *             linux-7.3/drivers/iommu/io-pgtable-arm.c
 *
 * The two sources agree on the register map; where their *policies* differ
 * (memattr tables) both are provided and the difference is asserted in the
 * test so it cannot rot silently.
 *
 * This file is freestanding: no libc, no kernel headers beyond the scalar
 * types.  Compiles both as userspace C99 and as kernel code.
 */
#ifndef MALI_HW_H
#define MALI_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* ------------------------------------------------------------------ */
/* GPU control block (registers are offsets into BAR0, size 0x5000)    */
/* [kb] mali_midg_regmap.h:28-197 ; [pf] panfrost_regs.h:11-100        */
/* ------------------------------------------------------------------ */
#define MALI_GPU_ID			0x0000
#define MALI_GPU_L2_FEATURES		0x0004
#define MALI_GPU_CORE_FEATURES		0x0008
#define MALI_GPU_TILER_FEATURES		0x000C
#define MALI_GPU_MEM_FEATURES		0x0010
#define MALI_GPU_MMU_FEATURES		0x0014
#define MALI_GPU_AS_PRESENT		0x0018
#define MALI_GPU_JS_PRESENT		0x001C
#define MALI_GPU_INT_RAWSTAT		0x0020
#define MALI_GPU_INT_CLEAR		0x0024
#define MALI_GPU_INT_MASK		0x0028
#define MALI_GPU_INT_STAT		0x002C
#define MALI_GPU_CMD			0x0030
#define MALI_GPU_STATUS			0x0034
#define MALI_GPU_LATEST_FLUSH_ID	0x0038
#define MALI_GPU_FAULT_STATUS		0x003C
#define MALI_GPU_FAULT_ADDR_LO		0x0040
#define MALI_GPU_FAULT_ADDR_HI		0x0044
#define MALI_GPU_PWR_KEY		0x0050
#define MALI_GPU_PWR_OVERRIDE0		0x0054
#define MALI_GPU_PWR_OVERRIDE1		0x0058

#define MALI_GPU_IRQ_FAULT		(1u << 0)
#define MALI_GPU_IRQ_MULTIPLE_FAULT	(1u << 7)
#define MALI_GPU_IRQ_RESET_COMPLETED	(1u << 8)
#define MALI_GPU_IRQ_POWER_CHANGED	(1u << 9)
#define MALI_GPU_IRQ_POWER_CHANGED_ALL	(1u << 10)
#define MALI_GPU_IRQ_PERFCNT_DONE	(1u << 16)
#define MALI_GPU_IRQ_CLEAN_CACHES_DONE	(1u << 17)

#define MALI_GPU_CMD_SOFT_RESET		0x01
#define MALI_GPU_CMD_HARD_RESET		0x02
#define MALI_GPU_CMD_CLEAN_CACHES	0x07
#define MALI_GPU_CMD_CLEAN_INV_CACHES	0x08

/* Core power control.  Present/enable registers are 32-bit low halves at
 * 0x100/0x180 for shaders, 0x110/0x190 for the tiler, 0x120/0x1a0 for L2.
 * [kb] lines 117-191 ; [pf] panfrost_regs.h 100-200 (identical layout)
 */
#define MALI_GPU_SHADER_PRESENT_LO	0x0100
#define MALI_GPU_SHADER_READY_LO	0x0140
#define MALI_GPU_SHADER_PWRON_LO	0x0180
#define MALI_GPU_SHADER_PWROFF_LO	0x01C0
#define MALI_GPU_SHADER_PWRTRANS_LO	0x0200

#define MALI_GPU_TILER_PRESENT_LO	0x0110
#define MALI_GPU_TILER_READY_LO		0x0150
#define MALI_GPU_TILER_PWRON_LO		0x0190
#define MALI_GPU_TILER_PWROFF_LO	0x01D0
#define MALI_GPU_TILER_PWRTRANS_LO	0x0210

#define MALI_GPU_L2_PRESENT_LO		0x0120
#define MALI_GPU_L2_READY_LO		0x0160
#define MALI_GPU_L2_PWRON_LO		0x01A0
#define MALI_GPU_L2_PWROFF_LO		0x01E0
#define MALI_GPU_L2_PWRTRANS_LO		0x0220

/* Job control block: 3 slots on Midgard.
 * [kb] JOB_CONTROL_BASE 0x1000, JOB_SLOT0 0x800, stride 0x80 (lines 199-265)
 * [pf] JS_BASE 0x1800, JS_SLOT_STRIDE 0x80 (panfrost_regs.h:244-263)
 */
#define MALI_JS_BASE			0x1800
#define MALI_JS_SLOT_STRIDE		0x0080
#define MALI_JS_HEAD_LO(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x00)
#define MALI_JS_HEAD_HI(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x04)
#define MALI_JS_TAIL_LO(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x08)
#define MALI_JS_TAIL_HI(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x0C)
#define MALI_JS_AFFINITY_LO(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x10)
#define MALI_JS_CONFIG(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x18)
#define MALI_JS_COMMAND(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x20)
#define MALI_JS_STATUS(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x24)
#define MALI_JS_HEAD_NEXT_LO(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x40)
#define MALI_JS_HEAD_NEXT_HI(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x44)
#define MALI_JS_AFFINITY_NEXT_LO(n)	(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x50)
#define MALI_JS_CONFIG_NEXT(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x58)
#define MALI_JS_COMMAND_NEXT(n)		(MALI_JS_BASE + ((n) * MALI_JS_SLOT_STRIDE) + 0x60)

/* MMU block. [kb] MEMORY_MANAGEMENT_BASE 0x2000, MMU_AS0 0x400, stride 0x40
 * [pf] MMU_BASE 0x2400, MMU_AS_SHIFT 6 (identical addresses)
 */
#define MALI_MMU_INT_RAWSTAT		0x2000
#define MALI_MMU_INT_CLEAR		0x2004
#define MALI_MMU_INT_MASK		0x2008
#define MALI_MMU_INT_STAT		0x200C

#define MALI_MMU_AS_BASE		0x2400
#define MALI_MMU_AS_STRIDE		0x0040
#define MALI_MMU_AS_REG(n, r)		(MALI_MMU_AS_BASE + ((n) * MALI_MMU_AS_STRIDE) + (r))

#define MALI_AS_TRANSTAB_LO		0x00
#define MALI_AS_TRANSTAB_HI		0x04
#define MALI_AS_MEMATTR_LO		0x08
#define MALI_AS_MEMATTR_HI		0x0C
#define MALI_AS_LOCKADDR_LO		0x10
#define MALI_AS_LOCKADDR_HI		0x14
#define MALI_AS_COMMAND			0x18
#define MALI_AS_FAULTSTATUS		0x1C
#define MALI_AS_FAULTADDR_LO		0x20
#define MALI_AS_FAULTADDR_HI		0x24
#define MALI_AS_STATUS			0x28
#define MALI_AS_TRANSCFG_LO		0x30
#define MALI_AS_TRANSCFG_HI		0x34

#define MALI_AS_TRANSTAB_ADDR_MASK	0xFFFFF000u
#define MALI_AS_TRANSTAB_ADRMODE_UNMAPPED (0u << 0)
#define MALI_AS_TRANSTAB_ADRMODE_IDENTITY (1u << 1)
#define MALI_AS_TRANSTAB_ADRMODE_TABLE	(3u << 0)
#define MALI_AS_TRANSTAB_READ_INNER	(1u << 2)
#define MALI_AS_TRANSTAB_SHARE_OUTER	(1u << 4)

#define MALI_AS_TRANSCFG_ADRMODE_LEGACY		(0u << 0)
#define MALI_AS_TRANSCFG_ADRMODE_UNMAPPED	(1u << 0)

#define MALI_AS_COMMAND_NOP		0x00
#define MALI_AS_COMMAND_UPDATE		0x01
#define MALI_AS_COMMAND_LOCK		0x02
#define MALI_AS_COMMAND_UNLOCK		0x03
#define MALI_AS_COMMAND_FLUSH_PT	0x04
#define MALI_AS_COMMAND_FLUSH_MEM	0x05

#define MALI_AS_STATUS_AS_ACTIVE	0x01

#define MALI_AS_FAULTSTATUS_EXC_MASK		(0x7u << 3)
#define MALI_AS_FAULTSTATUS_ACCESS_MASK		(0x3u << 8)
#define MALI_AS_FAULTSTATUS_ACCESS_ATOMIC	(0x0u << 8)
#define MALI_AS_FAULTSTATUS_ACCESS_EXEC		(0x1u << 8)
#define MALI_AS_FAULTSTATUS_ACCESS_READ		(0x2u << 8)
#define MALI_AS_FAULTSTATUS_ACCESS_WRITE	(0x3u << 8)

/* ------------------------------------------------------------------ */
/* MMU_FEATURES decoding                                               */
/* [kb] mali_kbase_gpuprops.c:303-304  KBASE_UBFX32(raw, 0, 8/8, 8)    */
/* [pf] panfrost_regs.h:19-20 (VA bits [7:0], PA bits [15:8])          */
/* ------------------------------------------------------------------ */
#define MALI_MMU_VA_BITS(x)	((uint32_t)((x) & 0xFFu))
#define MALI_MMU_PA_BITS(x)	((uint32_t)(((x) >> 8) & 0xFFu))

/* ------------------------------------------------------------------ */
/* GPU_ID decoding                                                     */
/* [kb] mali_kbase_gpu_id.h:21-38                                      */
/* ------------------------------------------------------------------ */
#define MALI_GPU_ID_STATUS(x)		((uint32_t)((x) & 0xFu))
#define MALI_GPU_ID_MINOR(x)		((uint32_t)(((x) >> 4) & 0xFFu))
#define MALI_GPU_ID_MAJOR(x)		((uint32_t)(((x) >> 12) & 0xFu))
#define MALI_GPU_ID_PRODUCT(x)		((uint32_t)(((x) >> 16) & 0xFFFFu))

#define MALI_PRODUCT_T60X	0x6956u
#define MALI_PRODUCT_T62X	0x0620u
#define MALI_PRODUCT_T72X	0x0720u
#define MALI_PRODUCT_T76X	0x0750u
#define MALI_PRODUCT_T82X	0x0820u
#define MALI_PRODUCT_T83X	0x0830u
#define MALI_PRODUCT_T86X	0x0860u
#define MALI_PRODUCT_TFRX	0x0880u

/* ------------------------------------------------------------------ */
/* LPAE descriptor encoding (Mali MMU, 4 levels, 9 bits/level, 4K grain) */
/* [kb] mali_kbase_mmu_mode_lpae.c:25-41, 175-190                       */
/* [pf] io-pgtable-arm.c:138-143, 377-390, 560-576                     */
/* ------------------------------------------------------------------ */
#define MALI_LPAE_LEVELS	4
#define MALI_LPAE_BITS_PER_LEVEL 9
#define MALI_LPAE_PAGE_SHIFT	12
#define MALI_LPAE_ENTRIES	512

#define MALI_PTE_TYPE_MASK	0x3ULL
#define MALI_PTE_TYPE_BLOCK	0x1ULL	/* kbase ENTRY_IS_ATE / ARM_LPAE_PTE_TYPE_BLOCK */
#define MALI_PTE_TYPE_INVALID	0x2ULL	/* kbase ENTRY_IS_INVAL */
#define MALI_PTE_TYPE_TABLE	0x3ULL	/* kbase ENTRY_IS_PTE */

/* Output address bits 39:12 - the Midgard MMU has 40 address lines
 * (kbase MMU_HW_OUTA_BITS 40, mali_midg_regmap.h:334). */
#define MALI_PTE_ADDR_MASK	(((1ULL << 40) - 1) & \
				 ~((1ULL << MALI_LPAE_PAGE_SHIFT) - 1))
#define MALI_PTE_MEMATTR_SHIFT	2
#define MALI_PTE_MEMATTR_MASK	(0x7ULL << MALI_PTE_MEMATTR_SHIFT)
#define MALI_PTE_AP_RD		(1ULL << 6)
#define MALI_PTE_AP_WR		(1ULL << 7)
#define MALI_PTE_SH_OS		(2ULL << 8)
#define MALI_PTE_SH_IS		(3ULL << 8)
#define MALI_PTE_SH_MASK	(3ULL << 8)
#define MALI_PTE_AF		(1ULL << 10)	/* kbase ENTRY_ACCESS_BIT */
#define MALI_PTE_XN		(1ULL << 54)	/* kbase ENTRY_NX_BIT */

/* Every non-address, non-type bit a leaf descriptor may carry. */
#define MALI_PTE_FLAGS_MASK	(MALI_PTE_MEMATTR_MASK | MALI_PTE_AP_RD | \
				 MALI_PTE_AP_WR | MALI_PTE_SH_MASK | \
				 MALI_PTE_AF | MALI_PTE_XN)

/* Attribute index used in bits 4:2 (kbase KBASE_REG_MEMATTR_VALUE). */
#define MALI_PTE_ATTR_IDX(idx)	(((uint64_t)(idx) & 0x7ULL) << MALI_PTE_MEMATTR_SHIFT)

/* Ready-made attribute sets for the common Midgard cases: GPU-readable,
 * GPU-writable, cached under the "cache" memattr index, outer-shareable
 * (the non-coherent default on Exynos 7580: no system-coherency property
 * in DT, so kbase runs COHERENCY_NONE - mali_kbase_core_linux.c:3577).
 */
#define MALI_ATTR_GPU_RO \
	(MALI_PTE_AP_RD | MALI_PTE_AF | MALI_PTE_SH_OS | \
	 MALI_PTE_ATTR_IDX(MALI_MAIR_IDX_CACHE))
#define MALI_ATTR_GPU_RW \
	(MALI_ATTR_GPU_RO | MALI_PTE_AP_WR)
#define MALI_ATTR_GPU_RW_NX \
	(MALI_ATTR_GPU_RW | MALI_PTE_XN)

/* ------------------------------------------------------------------ */
/* MEMATTR tables                                                      */
/*                                                                     */
/* kbase (LPAE build) [kb] mali_kbase_mmu_mode_lpae.c:77-87 and         */
/* mali_midg_regmap.h:506-531: index 0 = 0x48, 1 = 0x4f, 2 = 0x4d,      */
/* 3 = 0x88, 4 = 0x8d.                                                 */
/*                                                                     */
/* mainline io-pgtable-arm for ARM_MALI_LPAE [pf] lines 142-143,        */
/* 1244-1250: index 0 (NC) = 0x88, 1 (CACHE) = 0x8d, 2 (DEV) = 0x88.   */
/* The two policies are NOT identical; both are encoded here and the    */
/* test asserts both, so a future change to either cannot go unnoticed. */
/* ------------------------------------------------------------------ */
#define MALI_MEMATTR_KBASE_LPAE \
	(0x48ULL | (0x4FULL << 8) | (0x4DULL << 16) | (0x88ULL << 24) | \
	 (0x8DULL << 32))

#define MALI_MEMATTR_PANFROST_MALI \
	(0x88ULL | (0x8DULL << 8) | (0x88ULL << 16))

/* io-pgtable-arm attribute indices (only these three are populated). */
#define MALI_MAIR_IDX_NC	0
#define MALI_MAIR_IDX_CACHE	1
#define MALI_MAIR_IDX_DEV	2

/* ------------------------------------------------------------------ */
/* TRANSTAB encoding                                                   */
/* [kb] mmu_get_as_setup(): pgd | ADRMODE_TABLE | READ_INNER            */
/*      + SHARE_OUTER when system_coherency == COHERENCY_ACE            */
/* [pf] io-pgtable-arm.c:1260-1264: same, gated on coherent_walk        */
/* ------------------------------------------------------------------ */
static inline uint64_t mali_transtab_encode(uint64_t pgd_pa, int coherent)
{
	uint64_t v = (pgd_pa & ~((1ULL << MALI_LPAE_PAGE_SHIFT) - 1)) |
		     MALI_AS_TRANSTAB_ADRMODE_TABLE |
		     MALI_AS_TRANSTAB_READ_INNER;
	if (coherent)
		v |= MALI_AS_TRANSTAB_SHARE_OUTER;
	return v;
}

#endif /* MALI_HW_H */
