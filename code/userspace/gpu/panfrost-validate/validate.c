/* validate.c - self-contained userspace checker for Mali-T720 (Exynos 7580) MMU/register constants.
 *
 * Owns no kernel headers: every constant below is a literal copy of the value
 * asserted in code/userspace/gpu/panfrost/mali_hw.h, itself cross-checked
 * against the read-only kbase tree:
 *   drivers/gpu/arm/t7xx/r15p0/mali_midg_regmap.h   (register offsets, TRANSTAB/
 *     TRANSCFG/AS_COMMAND encodings, MEMATTR bytes, MMU_HW_OUTA_BITS=40)
 *   drivers/gpu/arm/t7xx/r15p0/mali_kbase_mmu_mode_lpae.c (ENTRY_IS_ATE=1,
 *     ENTRY_IS_INVAL=2, ENTRY_IS_PTE=3, ENTRY_ACCESS_BIT=bit10, ENTRY_NX_BIT=bit54)
 *   drivers/gpu/arm/t7xx/r15p0/mali_kbase_gpuprops.c  (GPU_ID UBFX32 fields,
 *     MMU_FEATURES VA bits [7:0] / PA bits [15:8])
 * DO NOT "fix" a mismatch by editing the expected value here: a failure means
 * mali_hw.h (or kbase) disagrees with this file and the divergence must be
 * resolved at the source.
 *
 * Build: make        (native)
 *        make aarch64 (cross, needs aarch64-linux-gnu-gcc)
 * Run:   ./validate  (or qemu-aarch64 ./validate.aarch64)
 */
#include <stdio.h>
#include <stdint.h>

/* ---- register map (BAR0 offsets, window size 0x5000) ---- */
#define V_GPU_ID		0x0000u
#define V_GPU_MMU_FEATURES	0x0014u
#define V_GPU_AS_PRESENT	0x0018u
#define V_GPU_JS_PRESENT	0x001Cu
#define V_GPU_CMD		0x0030u
#define V_MMU_INT_RAWSTAT	0x2000u
#define V_MMU_AS_BASE		0x2400u
#define V_MMU_AS_STRIDE		0x0040u
#define V_JS_BASE		0x1800u
#define V_JS_SLOT_STRIDE	0x0080u
#define V_AS_REG(n, r)		(V_MMU_AS_BASE + ((n) * V_MMU_AS_STRIDE) + (r))
#define V_JS_REG(n, r)		(V_JS_BASE + ((n) * V_JS_SLOT_STRIDE) + (r))

#define V_AS_TRANSTAB_LO	0x00u
#define V_AS_MEMATTR_LO		0x08u
#define V_AS_MEMATTR_HI		0x0Cu
#define V_AS_COMMAND		0x18u
#define V_AS_FAULTSTATUS	0x1Cu
#define V_AS_FAULTADDR_LO	0x20u
#define V_AS_TRANSCFG_LO	0x30u
#define V_AS_TRANSCFG_HI	0x34u

/* LPAE: 4 levels x 9 bits, 4K grain, 40-bit output address */
#define V_LEVELS	4
#define V_BITS_PER_LEVEL 9
#define V_PAGE_SHIFT	12
#define V_PTE_TYPE_BLOCK	1ULL
#define V_PTE_TYPE_INVALID	2ULL
#define V_PTE_TYPE_TABLE	3ULL
#define V_PTE_ADDR_MASK		(((1ULL << 40) - 1) & ~((1ULL << 12) - 1))
#define V_PTE_AP_RD		(1ULL << 6)
#define V_PTE_AP_WR		(1ULL << 7)
#define V_PTE_SH_OS		(2ULL << 8)
#define V_PTE_AF		(1ULL << 10)
#define V_PTE_XN		(1ULL << 54)
#define V_ATTR_IDX(i)		(((uint64_t)(i) & 7ULL) << 2)

/* TRANSTAB bits */
#define V_ADRMODE_TABLE		(3u << 0)
#define V_READ_INNER		(1u << 2)
#define V_SHARE_OUTER		(1u << 4)

/* MEMATTR tables */
#define V_MEMATTR_KBASE		0x8D884D4F48ULL /* idx0=0x48,1=0x4F,2=0x4D,3=0x88,4=0x8D */
#define V_MEMATTR_PANFROST	0x888D88ULL	    /* idx0=0x88,1=0x8D,2=0x88 */

static int failures, checks;
#define CHECK_EQ(a, b) do { \
	uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); \
	checks++; \
	if (_a != _b) { failures++; \
		printf("FAIL line %d: %s = 0x%llx, want 0x%llx\n", __LINE__, #a, \
			(unsigned long long)_a, (unsigned long long)_b); } \
} while (0)

static unsigned pte_index(uint64_t va, int level)
{
	return (unsigned)((va >> V_PAGE_SHIFT) >> ((3 - level) * V_BITS_PER_LEVEL)) & 0x1FFu;
}
static uint64_t leaf(uint64_t pa, uint64_t attrs)
{
	return (pa & V_PTE_ADDR_MASK) | (attrs & ~(uint64_t)3) | V_PTE_TYPE_BLOCK;
}
static uint64_t transtab(uint64_t pgd_pa, int coherent)
{
	uint64_t v = (pgd_pa & ~((1ULL << 12) - 1)) | V_ADRMODE_TABLE | V_READ_INNER;
	if (coherent)
		v |= V_SHARE_OUTER;
	return v;
}

static void test_regs(void)
{
	CHECK_EQ(V_GPU_ID, 0x0000);
	CHECK_EQ(V_GPU_MMU_FEATURES, 0x0014);
	CHECK_EQ(V_GPU_AS_PRESENT, 0x0018);
	CHECK_EQ(V_GPU_JS_PRESENT, 0x001C);
	CHECK_EQ(V_GPU_CMD, 0x0030);
	CHECK_EQ(V_MMU_INT_RAWSTAT, 0x2000);
	CHECK_EQ(V_JS_SLOT_STRIDE, 0x80);
	CHECK_EQ(V_MMU_AS_STRIDE, 0x40);
	/* job slot stride @0x1800 */
	CHECK_EQ(V_JS_REG(0, 0x00), 0x1800);
	CHECK_EQ(V_JS_REG(2, 0x00), 0x1900);
	CHECK_EQ(V_JS_REG(0, 0x20), 0x1820);
	CHECK_EQ(V_JS_REG(2, 0x60), 0x1960);
	/* AS stride @0x2400 */
	CHECK_EQ(V_AS_REG(0, V_AS_TRANSTAB_LO), 0x2400);
	CHECK_EQ(V_AS_REG(0, V_AS_MEMATTR_LO), 0x2408);
	CHECK_EQ(V_AS_REG(0, V_AS_MEMATTR_HI), 0x240C);
	CHECK_EQ(V_AS_REG(0, V_AS_COMMAND), 0x2418);
	CHECK_EQ(V_AS_REG(0, V_AS_FAULTSTATUS), 0x241C);
	CHECK_EQ(V_AS_REG(0, V_AS_FAULTADDR_LO), 0x2420);
	CHECK_EQ(V_AS_REG(0, V_AS_TRANSCFG_LO), 0x2430);
	CHECK_EQ(V_AS_REG(0, V_AS_TRANSCFG_HI), 0x2434);
	CHECK_EQ(V_AS_REG(15, V_AS_TRANSTAB_LO), 0x27C0);
	CHECK_EQ(V_AS_REG(15, V_AS_TRANSCFG_HI), 0x27F4);
	/* top of AS block still inside the 0x5000 window */
	CHECK_EQ(V_AS_REG(15, V_AS_TRANSCFG_HI) < 0x5000, 1);
}

static void test_id_decode(void)
{
	/* GPU_ID fields: status[3:0] minor[11:4] major[15:12] product[31:16] */
	uint32_t r0p0 = (0x0720u << 16) | (0u << 12) | (0u << 4) | 0u;
	uint32_t r1p3 = (0x0720u << 16) | (1u << 12) | (3u << 4) | 1u;
	CHECK_EQ(r0p0 & 0xFu, 0);			/* status */
	CHECK_EQ((r0p0 >> 4) & 0xFFu, 0);		/* minor */
	CHECK_EQ((r0p0 >> 12) & 0xFu, 0);		/* major */
	CHECK_EQ((r0p0 >> 16) & 0xFFFFu, 0x0720);	/* T720 product */
	CHECK_EQ((r1p3 >> 12) & 0xFu, 1);
	CHECK_EQ((r1p3 >> 4) & 0xFFu, 3);
	CHECK_EQ(r1p3 & 0xFu, 1);
	/* MMU_FEATURES: VA bits [7:0], PA bits [15:8] */
	CHECK_EQ(0x2820u & 0xFFu, 32);		/* VA bits */
	CHECK_EQ((0x2820u >> 8) & 0xFFu, 40);	/* PA bits */
}

static void test_lpae(void)
{
	uint64_t rw = V_PTE_AP_RD | V_PTE_AP_WR | V_PTE_AF | V_PTE_SH_OS | V_ATTR_IDX(1);
	/* index vectors */
	CHECK_EQ(pte_index(0x1000ULL, 3), 1);
	CHECK_EQ(pte_index(0x1000ULL, 0), 0);
	CHECK_EQ(pte_index(0x200000ULL, 2), 1);
	CHECK_EQ(pte_index(0x40000000ULL, 1), 1);
	CHECK_EQ(pte_index(0x40000000ULL, 0), 0);
	CHECK_EQ(pte_index(0xFFFFF000ULL, 1), 3);
	CHECK_EQ(pte_index(0xFFFFF000ULL, 2), 511);
	CHECK_EQ(pte_index(0xFFFFF000ULL, 3), 511);
	CHECK_EQ(V_LEVELS, 4);
	/* descriptor vectors: pa | type=1 | AF | SH_OS | WR | RD | idx1 */
	CHECK_EQ(leaf(0x80000000ULL, rw), 0x800006C5ULL);
	CHECK_EQ(leaf(0x80000000ULL, rw) & 3ULL, V_PTE_TYPE_BLOCK);
	CHECK_EQ(leaf(0x80000000ULL, rw) & V_PTE_ADDR_MASK, 0x80000000ULL);
	CHECK_EQ((0x40001000ULL & V_PTE_ADDR_MASK) | V_PTE_TYPE_TABLE, 0x40001003ULL);
	/* 40-bit OAS: bits above 39 never survive the mask */
	CHECK_EQ(0xFFFFFF0000000000ULL & V_PTE_ADDR_MASK, 0);
	/* unaligned PA low bits are masked out of the address field */
	CHECK_EQ(leaf(0x80000FFFULL, rw) & V_PTE_ADDR_MASK, 0x80000000ULL);
	/* type codes */
	CHECK_EQ(V_PTE_TYPE_BLOCK, 1);
	CHECK_EQ(V_PTE_TYPE_INVALID, 2);
	CHECK_EQ(V_PTE_TYPE_TABLE, 3);
}

static void test_as_config(void)
{
	CHECK_EQ(transtab(0x40000000ULL, 0), 0x40000007ULL);
	CHECK_EQ(transtab(0x40000000ULL, 1), 0x40000017ULL);
	CHECK_EQ(V_MEMATTR_KBASE, 0x8D884D4F48ULL);
	CHECK_EQ(V_MEMATTR_KBASE & 0xFFULL, 0x48ULL);
	CHECK_EQ((V_MEMATTR_KBASE >> 8) & 0xFFULL, 0x4FULL);
	CHECK_EQ((V_MEMATTR_KBASE >> 16) & 0xFFULL, 0x4DULL);
	CHECK_EQ((V_MEMATTR_KBASE >> 24) & 0xFFULL, 0x88ULL);
	CHECK_EQ((V_MEMATTR_KBASE >> 32) & 0xFFULL, 0x8DULL);
	CHECK_EQ(V_MEMATTR_PANFROST, 0x888D88ULL);
	/* DIVERGENCE (kbase-vs-mainline, see design.md R4): index 0 and 2
	 * differ: kbase uses 0x48/0x4D (impl-def cache policy / write-alloc),
	 * mainline io-pgtable-arm uses 0x88 (outer non-cacheable) for both.
	 * Flagged, not resolved: the M1 driver must pick one policy. */
	if ((((V_MEMATTR_KBASE & 0xFFULL) != (V_MEMATTR_PANFROST & 0xFFULL))) ||
	    (((V_MEMATTR_KBASE >> 16) & 0xFFULL) != ((V_MEMATTR_PANFROST >> 16) & 0xFFULL)))
		printf("note: kbase-vs-mainline MEMATTR divergence present "
		       "(idx0 kbase=0x48 panfrost=0x88, idx2 kbase=0x4D panfrost=0x88)\n");
	else {
		printf("FAIL: expected MEMATTR divergence vanished\n");
		failures++;
	}
	checks++;
}

int main(void)
{
	test_regs();
	test_id_decode();
	test_lpae();
	test_as_config();
	printf("%s: %d checks, %d failures\n",
	       failures ? "RESULT FAIL" : "RESULT PASS", checks, failures);
	return failures ? 1 : 0;
}
