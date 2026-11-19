/*
 * mali_mmu.h - Midgard MMU address-space lifecycle + invalidation model.
 *
 * Host-side model of the exact register writes panfrost must issue on the
 * T720 MP2 in j7elte. Every sequence mirrors kbase (r15p0, LPAE build) and
 * mainline panfrost v5.2 field for field; provenance is cited per item.
 *
 * AS lifecycle on this SoC (COHERENCY_NONE, LPAE, transcfg legacy 0):
 *
 *   enable:  TRANSTAB_LO/HI <- pgd_pa | TABLE(3) | READ_INNER(4)
 *            MEMATTR_LO/HI  <- 0x884D4F48 / 0x00008D88 (kbase LPAE table)
 *            TRANSCFG_LO    <- 0 (LEGACY); TRANSCFG_HI <- 0
 *            wmb, then AS_COMMAND <- UPDATE, gated on !AS_ACTIVE before
 *            and after (kbase_mmu_hw_configure + wait_ready; panfrost
 *            panfrost_mmu_enable + write_cmd).
 *   disable: TRANSTAB/MEMATTR <- 0, then AS_COMMAND <- UPDATE
 *            (kbase disable_as; panfrost mmu_disable).
 *   map:     write PTEs, dma/cache-sync the table pages, then
 *            LOCK(iova,size) + FLUSH_PT + wait !ACTIVE (+ optional UNLOCK).
 *   unmap:   same shape with FLUSH_PT (panfrost panfrost_mmu_unmap;
 *            kbase mmu_hw_do_operation).
 *   full-TLB: LOCK(0,~0UL) + FLUSH_MEM + wait !ACTIVE (panfrost
 *            mmu_tlb_inv_context_s1).
 *
 * 33-bit VA (measured mmu_features 0x2821: VA 33, PA 40): IOVA window is
 * [0, 2^33); L0 index is always 0 (it decodes VA bits 39+, all zero
 * here), L1 spans entries 0..7.
 * T720 r1p0 carries no 9630-class double-UNLOCK erratum (issues list
 * t72x_r1p0: 6402/9435/10471/10649/10684/10797/10821/10883/10946/11042/
 * 11051/11054/T76X_1909/1963/3964/TMIX_8438 - 9630 absent), so the extra
 * double-UNLOCK after LOCK+FLUSH is NOT part of the sequence here.
 *
 * Fault decode: MMU_INT_STAT bit i = page fault in AS i, bit i+16 = bus
 * error in AS i (page fault takes the bit when both fire). AS_FAULTSTATUS
 * low byte = exception class (0xC0+ translation, 0xC8 permission, 0xD0+
 * transtab bus, 0xD8 access flag on LPAE), bits 9:8 = access type, bit 10 =
 * decoder(1)/slave(0), bits 31:16 = source ID. Fault address is a 64-bit
 * VA from AS_FAULTADDR_HI/LO. IRQ handling: mask MMU IRQs, drain fault
 * HI/LO + STATUS per set AS, clear via MMU_INT_CLEAR, re-enable mask.
 *
 * This header is freestanding (no libc/kernel includes beyond mali_hw.h,
 * which supplies uint32_t/uint64_t in both worlds).
 */
#ifndef MALI_MMU_H
#define MALI_MMU_H

/* AS count / selection                                                */
/* ------------------------------------------------------------------ */

/* Number of address-space slots the IP can carry (compile-time ceiling;
 * kbase BASE_MAX_NR_AS 16, mali_kbase_defs.h:142; MMU irq pairing covers
 * exactly 16 AS: MMU_PAGE_FAULT_FLAGS 16). */
#define MALI_MAX_NR_AS	16

/* AS slots present on this unit: popcount(AS_PRESENT). T720 MP2 reports
 * 0xFF (8 AS) per gpu-verify.md; panfrost binds its single mm_struct to
 * AS 0 (panfrost_mmu_enable(pfdev, 0); mmu_tlb ops use AS 0). */
static inline unsigned int mali_as_count(uint32_t as_present)
{
	unsigned int n = 0;

	while (as_present) {
		n += as_present & 1u;
		as_present >>= 1;
	}
	return n;
}

/* Slot usable by the driver: must be within the ceiling and marked
 * present. Panfrost always picks 0; any other slot needs the same check. */
static inline int mali_as_select_ok(uint32_t as_present, unsigned int as_nr)
{
	if (as_nr >= MALI_MAX_NR_AS)
		return 0;
	return (as_present & (1u << as_nr)) != 0;
}

/* ------------------------------------------------------------------ */
/* 33-bit VA window                                                    */
/* ------------------------------------------------------------------ */

/* IOVA limit for a va_bits-wide GPU VA space: [0, 2^va_bits).
 * Measured on this unit: va_bits 33 (mmu_features 0x2821), so 8 GiB. */
static inline uint64_t mali_va_limit(unsigned int va_bits)
{
	if (va_bits >= 64)
		return ~0ULL;
	if (va_bits == 0)
		return 0ULL;
	return 1ULL << va_bits;
}

static inline int mali_va_in_range(uint64_t iova, unsigned int va_bits)
{
	return iova < mali_va_limit(va_bits);
}

/* L0 entries touched by a va_bits-wide space. L0 decodes VA bits 39+
 * (vpfn >> 27), so any space narrower than 40 bits lives in L0[0]
 * alone; wider spaces fan out 2^(va_bits-39) top entries (max 512).
 * At the measured 33 bits: L0[0] only, L1 entries 0..7.
 * (The M0 test's "L0 stays 0 for any 32-bit VA" is the va_bits < 40
 * case of this rule.) */
static inline unsigned int mali_l0_entries(unsigned int va_bits)
{
	if (va_bits <= 39)
		return 1;
	if (va_bits >= 48)
		return MALI_LPAE_ENTRIES;
	return 1u << (va_bits - 39);
}

/* ------------------------------------------------------------------ */
/* LOCK region encoding                                                */
/* ------------------------------------------------------------------ */

/* fls: 1-based index of the top set bit, 0 for 0. */
static inline unsigned int mali_fls(uint32_t x)
{
	unsigned int n = 0;

	while (x) {
		n++;
		x >>= 1;
	}
	return n;
}

/* Encode the LOCKADDR value for [iova, iova+size): base vpfn with the
 * pow2-rounded page count folded into bits 5:0 as (10 + fls(pages)).
 * Saturates at the 63 ceiling for ranges above 2^53 bytes (including
 * the LOCK(0, ~0UL) full-TLB shape).
 * [kb] mali_kbase_mmu_hw_direct.c:28-58 ; [pf] panfrost_mmu.c:57-83. */
static inline uint64_t mali_lock_region(uint64_t iova, uint64_t size)
{
	uint64_t pages;
	unsigned int width;
	uint64_t region;

	if (size == 0) {
		pages = 1;
	} else {
		pages = (size + ((1ULL << MALI_LPAE_PAGE_SHIFT) - 1)) >>
			MALI_LPAE_PAGE_SHIFT;
		if (pages == 0)
			pages = ~0ULL;	/* rounded past 64 bits: saturate */
	}

	if (pages > (1ULL << (MALI_LOCK_REGION_MAX_SIZE - 10))) {
		width = MALI_LOCK_REGION_MAX_SIZE;
	} else {
		uint32_t p = (uint32_t)pages;

		width = 10 + mali_fls(p);
		if (p != (1u << (width - 11)))
			width += 1;	/* not pow2: round up */
		if (width < MALI_LOCK_REGION_MIN_SIZE)
			width = MALI_LOCK_REGION_MIN_SIZE;
		if (width > MALI_LOCK_REGION_MAX_SIZE)
			width = MALI_LOCK_REGION_MAX_SIZE;
	}

	region = iova & ~((1ULL << MALI_LPAE_PAGE_SHIFT) - 1);
	region |= width;
	return region;
}

/* ------------------------------------------------------------------ */
/* AS config register values                                           */
/* ------------------------------------------------------------------ */

/* TRANSTAB for a pgd page: physical base | TABLE | READ_INNER on this
 * non-coherent SoC (SHARE_OUTER only under COHERENCY_ACE, absent here).
 * [kb] mali_kbase_mmu_mode_lpae.c:89-97 ; [pf] io-pgtable-arm MALI path. */
static inline uint64_t mali_as_transtab(uint64_t pgd_pa)
{
	return mali_transtab_encode(pgd_pa, 0);
}

/* AS disable value: TRANSTAB unmapped (ADRMODE 0), MEMATTR 0, then UPDATE.
 * [kb] mali_kbase_mmu_mode_lpae.c:107-118 ; [pf] mmu_disable. */
static inline uint64_t mali_as_transtab_disabled(void)
{
	return MALI_AS_TRANSTAB_ADRMODE_UNMAPPED;
}

/* MEMATTR pair the driver programs: the kbase LPAE table
 * (0x8D884D4F48) matches the shipping driver on this exact silicon;
 * the panfrost table (0x888D88) is the mainline alternative (R4). */
static inline uint64_t mali_as_memattr_kbase(void)
{
	return MALI_MEMATTR_KBASE_LPAE;
}

/* ------------------------------------------------------------------ */
/* Fault decode                                                       */
/* ------------------------------------------------------------------ */

/* Exception class of a raw AS_FAULTSTATUS (LPAE build names). */
static inline const char *mali_fault_exc_name(uint32_t status)
{
	uint32_t e = status & MALI_AS_FAULTSTATUS_EXC_MASK;

	if (e >= MALI_EXC_TRANSLATION_FAULT_BASE &&
	    e < MALI_EXC_TRANSLATION_FAULT_BASE + 8)
		return "TRANSLATION_FAULT";
	if (e == MALI_EXC_PERMISSION_FAULT)
		return "PERMISSION_FAULT";
	if (e >= MALI_EXC_TRANSTAB_BUS_FAULT_BASE &&
	    e < MALI_EXC_TRANSTAB_BUS_FAULT_BASE + 8)
		return "TRANSTAB_BUS_FAULT";
	if (e == MALI_EXC_ACCESS_FLAG)
		return "ACCESS_FLAG";
	return "UNKNOWN";
}

static inline const char *mali_fault_access_name(uint32_t status)
{
	switch (status & MALI_AS_FAULTSTATUS_ACCESS_MASK) {
	case MALI_AS_FAULTSTATUS_ACCESS_ATOMIC:
		return "UNKNOWN";	/* ATOMIC only names on AARCH64-MMU */
	case MALI_AS_FAULTSTATUS_ACCESS_EXEC:
		return "EXECUTE";
	case MALI_AS_FAULTSTATUS_ACCESS_READ:
		return "READ";
	case MALI_AS_FAULTSTATUS_ACCESS_WRITE:
		return "WRITE";
	default:
		return "UNKNOWN";
	}
}

static inline int mali_fault_is_decoder(uint32_t status)
{
	return (status & MALI_AS_FAULTSTATUS_DEC) != 0;
}

static inline uint32_t mali_fault_source_id(uint32_t status)
{
	return (status & MALI_AS_FAULTSTATUS_SRC_MASK) >> 16;
}

/* IRQ pairing: bit i = page fault in AS i, bit i+16 = bus error in AS i.
 * Bus error wins a slot that reports both (kbase_mmu_interrupt). */
static inline int mali_irq_is_page_fault(uint32_t irq_stat, unsigned int as)
{
	uint32_t bit = MALI_MMU_PAGE_FAULT(as);

	if (irq_stat & MALI_MMU_BUS_ERROR(as))
		return 0;
	return (irq_stat & bit) != 0;
}

static inline int mali_irq_is_bus_error(uint32_t irq_stat, unsigned int as)
{
	return (irq_stat & MALI_MMU_BUS_ERROR(as)) != 0;
}

/* 64-bit fault VA from the HI/LO pair. */
static inline uint64_t mali_fault_addr(uint32_t hi, uint32_t lo)
{
	return (((uint64_t)hi) << 32) | lo;
}

#endif /* MALI_MMU_H */
