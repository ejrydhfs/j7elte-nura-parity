/*
 * mali_lpae_test.c - host unit tests for the Midgard MMU page-table builder.
 *
 * These tests are the verification for the one part of the panfrost port
 * that can be checked without silicon: descriptor encoding, index math,
 * register offsets and feature decoding.  Every expected value in here is
 * derived from the two in-tree references named in mali_hw.h / mali_lpae.h,
 * not from a running GPU.
 *
 * Build and run:  make check
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mali_hw.h"
#include "mali_lpae.h"
#include "mali_mmu.h"

static int failures;
static int checks;

#define CHECK(cond) do {                                              \
	checks++;                                                     \
	if (!(cond)) {                                                \
		failures++;                                           \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	}                                                             \
} while (0)

#define CHECK_EQ(a, b) do {                                           \
	uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);              \
	checks++;                                                     \
	if (_a != _b) {                                               \
		failures++;                                           \
		printf("FAIL %s:%d: %s = 0x%llx, expected 0x%llx\n",  \
		       __FILE__, __LINE__, #a,                        \
		       (unsigned long long)_a, (unsigned long long)_b);\
	}                                                             \
} while (0)

/* ------------------------------------------------------------------ */
/* A tiny page allocator with a fake physical address space.           */
/* ------------------------------------------------------------------ */
#define HEAP_PAGES 64
#define HEAP_PA_BASE 0x40000000ULL

struct heap {
	void *va[HEAP_PAGES];
	uint64_t pa[HEAP_PAGES];
	int used;
	int allocs;
	int frees;
};

static int heap_alloc(void *cookie, struct mali_table *t)
{
	struct heap *h = cookie;
	int i;

	for (i = 0; i < HEAP_PAGES; i++) {
		if (h->va[i])
			continue;
		h->va[i] = aligned_alloc(4096, 4096);
		if (!h->va[i])
			return MALI_ENOMEM;
		memset(h->va[i], 0, 4096);
		h->pa[i] = HEAP_PA_BASE + (uint64_t)i * 4096;
		t->va = h->va[i];
		t->pa = h->pa[i];
		h->allocs++;
		h->used++;
		return MALI_OK;
	}
	return MALI_ENOMEM;
}

static void heap_free(void *cookie, struct mali_table *t)
{
	struct heap *h = cookie;
	int i;

	for (i = 0; i < HEAP_PAGES; i++) {
		if (h->va[i] == t->va) {
			free(h->va[i]);
			h->va[i] = NULL;
			h->pa[i] = 0;
			h->frees++;
			h->used--;
			return;
		}
	}
	printf("FAIL heap_free: page %p not owned\n", t->va);
	failures++;
}

static int heap_from_pa(void *cookie, uint64_t pa, struct mali_table *t)
{
	struct heap *h = cookie;
	int i;

	for (i = 0; i < HEAP_PAGES; i++) {
		if (h->pa[i] == pa && h->va[i]) {
			t->va = h->va[i];
			t->pa = pa;
			return MALI_OK;
		}
	}
	return MALI_EINVAL;
}

static void heap_publish(void *cookie)
{
	(void)cookie;
}

static const struct mali_pgtable_ops heap_ops = {
	.alloc = heap_alloc,
	.free = heap_free,
	.from_pa = heap_from_pa,
	.publish = heap_publish,
};

static struct heap heap;
static uint64_t pgd[512] __attribute__((aligned(4096)));

static void fixture_init(struct mali_pgtable *pt, unsigned int va_bits)
{
	memset(&heap, 0, sizeof(heap));
	memset(pgd, 0, sizeof(pgd));
	mali_pgtable_init(pt, pgd, HEAP_PA_BASE + 0x3FF000ULL, &heap_ops, &heap,
			  va_bits);
}

/* ------------------------------------------------------------------ */
/* 1. Index math: 4 levels * 9 bits, index = vpfn >> ((3-level)*9)      */
/* ------------------------------------------------------------------ */
static void test_index_math(void)
{
	/* A 4 KiB page at 0x1000: only the last level is non-zero. */
	CHECK_EQ(mali_pte_index(0x1000ULL, 0), 0);
	CHECK_EQ(mali_pte_index(0x1000ULL, 1), 0);
	CHECK_EQ(mali_pte_index(0x1000ULL, 2), 0);
	CHECK_EQ(mali_pte_index(0x1000ULL, 3), 1);

	/* 2 MiB boundary: L2 increments. */
	CHECK_EQ(mali_pte_index(0x200000ULL, 2), 1);
	CHECK_EQ(mali_pte_index(0x200000ULL, 3), 0);

	/* 1 GiB boundary: L1 increments; L0 stays 0 for any 32-bit VA, which
	 * is exactly why Midgard still needs a level 0 table. */
	CHECK_EQ(mali_pte_index(0x40000000ULL, 1), 1);
	CHECK_EQ(mali_pte_index(0x40000000ULL, 0), 0);

	/* Top of the 32-bit space. */
	CHECK_EQ(mali_pte_index(0xFFFFF000ULL, 1), 3);
	CHECK_EQ(mali_pte_index(0xFFFFF000ULL, 2), 511);
	CHECK_EQ(mali_pte_index(0xFFFFF000ULL, 3), 511);

	/* Level count is fixed at 4 for the Mali format even though a 32-bit
	 * VA only needs three (io-pgtable-arm.c:1232-1236). */
	CHECK_EQ(MALI_LPAE_LEVELS, 4);
	CHECK_EQ(MALI_LPAE_ENTRIES, 512);
}

/* ------------------------------------------------------------------ */
/* 2. Descriptor bit layout                                            */
/* ------------------------------------------------------------------ */
static void test_descriptors(void)
{
	uint64_t leaf = mali_pte_make_leaf(0x80000000ULL, MALI_ATTR_GPU_RW);

	/* pa | TYPE_BLOCK(1) | AF(0x400) | SH_OS(0x200) | WR(0x80) |
	 * RD(0x40) | attr index 1 (0x4).  Same value kbase's entry_set_ate()
	 * writes for flags = RD|WR|SHARE_BOTH with memattr 1. */
	CHECK_EQ(leaf, 0x800006C5ULL);
	CHECK_EQ(mali_pte_type(leaf), MALI_PTE_TYPE_BLOCK);
	CHECK_EQ(mali_pte_addr(leaf), 0x80000000ULL);

	/* Table descriptor is type 3 and keeps the address. */
	CHECK_EQ(mali_pte_make_table(0x40001000ULL), 0x40001003ULL);
	CHECK_EQ(mali_pte_type(0x40001003ULL), MALI_PTE_TYPE_TABLE);

	/* Read-only + NX. */
	CHECK_EQ(mali_pte_make_leaf(0x1000,
				    MALI_ATTR_GPU_RO | MALI_PTE_XN) &
		 (MALI_PTE_AP_WR | MALI_PTE_XN),
		 MALI_PTE_XN);

	/* Address bits above the 40-bit OAS never leak into a descriptor. */
	CHECK_EQ(mali_pte_addr(0xFFFFFF0000000000ULL), 0ULL);

	/* Type bits are kbase's: ATE=1, INVAL=2, PTE=3. */
	CHECK_EQ(MALI_PTE_TYPE_BLOCK, 1);
	CHECK_EQ(MALI_PTE_TYPE_INVALID, 2);
	CHECK_EQ(MALI_PTE_TYPE_TABLE, 3);
}

/* ------------------------------------------------------------------ */
/* 3. TRANSTAB / MEMATTR encodings                                     */
/* ------------------------------------------------------------------ */
static void test_as_config(void)
{
	/* kbase mmu_get_as_setup(): pgd | ADRMODE_TABLE(3) | READ_INNER(4). */
	CHECK_EQ(mali_transtab_encode(0x40000000ULL, 0), 0x40000007ULL);
	/* SHARE_OUTER (bit 4) only for an ACE-coherent system; Exynos 7580
	 * has no system-coherency property, so kbase runs COHERENCY_NONE. */
	CHECK_EQ(mali_transtab_encode(0x40000000ULL, 1), 0x40000017ULL);

	/* kbase LPAE memattr table (indices 0..4).
	 * 0x48, 0x4F, 0x4D, 0x88, 0x8D packed little-endian by index. */
	CHECK_EQ(MALI_MEMATTR_KBASE_LPAE, 0x8D884D4F48ULL);

	/* mainline io-pgtable-arm ARM_MALI_LPAE memattr:
	 * idx 0 = 0x88, idx 1 = 0x8D, idx 2 = 0x88.  Deliberately different
	 * from kbase - see design.md risk R4. */
	CHECK_EQ(MALI_MEMATTR_PANFROST_MALI, 0x888D88ULL);
	CHECK_EQ(MALI_MEMATTR_PANFROST_MALI & 0xFFULL, 0x88ULL);
	CHECK_EQ((MALI_MEMATTR_PANFROST_MALI >> 8) & 0xFFULL, 0x8DULL);
}

/* ------------------------------------------------------------------ */
/* 4. Register map cross-check (kbase vs mainline panfrost)            */
/* ------------------------------------------------------------------ */
struct regcheck {
	const char *name;
	unsigned int got;
	unsigned int want;
};

static void test_registers(void)
{
	static const struct regcheck regs[] = {
		{ "GPU_ID",            MALI_GPU_ID,            0x0000 },
		{ "GPU_MMU_FEATURES",  MALI_GPU_MMU_FEATURES,  0x0014 },
		{ "GPU_AS_PRESENT",    MALI_GPU_AS_PRESENT,    0x0018 },
		{ "GPU_JS_PRESENT",    MALI_GPU_JS_PRESENT,    0x001C },
		{ "GPU_CMD",           MALI_GPU_CMD,           0x0030 },
		{ "JS_HEAD_LO(0)",     MALI_JS_HEAD_LO(0),     0x1800 },
		{ "JS_HEAD_LO(2)",     MALI_JS_HEAD_LO(2),     0x1900 },
		{ "JS_COMMAND(0)",     MALI_JS_COMMAND(0),     0x1820 },
		{ "JS_COMMAND_NEXT(2)", MALI_JS_COMMAND_NEXT(2), 0x1960 },
		{ "MMU_INT_RAWSTAT",   MALI_MMU_INT_RAWSTAT,   0x2000 },
		{ "AS_TRANSTAB(0)",    MALI_MMU_AS_REG(0, MALI_AS_TRANSTAB_LO), 0x2400 },
		{ "AS_COMMAND(0)",     MALI_MMU_AS_REG(0, MALI_AS_COMMAND),     0x2418 },
		{ "AS_TRANSCFG(0)",    MALI_MMU_AS_REG(0, MALI_AS_TRANSCFG_LO), 0x2430 },
		{ "AS_TRANSTAB(15)",   MALI_MMU_AS_REG(15, MALI_AS_TRANSTAB_LO), 0x27C0 },
		{ "L2_PWRON_LO",       MALI_GPU_L2_PWRON_LO,   0x01A0 },
		{ "SHADER_PWRON_LO",   MALI_GPU_SHADER_PWRON_LO, 0x0180 },
		{ "TILER_READY_LO",    MALI_GPU_TILER_READY_LO, 0x0150 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		CHECK_EQ(regs[i].got, regs[i].want);

	/* Slot stride and address-space stride are both fixed by the IP. */
	CHECK_EQ(MALI_JS_SLOT_STRIDE, 0x80);
	CHECK_EQ(MALI_MMU_AS_STRIDE, 0x40);
	/* The whole register window the DT declares is 0x5000 bytes. */
	CHECK(MALI_MMU_AS_REG(15, MALI_AS_TRANSCFG_HI) < 0x5000);
}

/* ------------------------------------------------------------------ */
/* 5. GPU_ID and MMU_FEATURES decoding                                 */
/* ------------------------------------------------------------------ */
static void test_id_decode(void)
{
	/* A T720 at r0p0 (product 0x0720, major 0, minor 0, status 0). */
	uint32_t t720 = (MALI_PRODUCT_T72X << 16) | (0u << 12) | (0u << 4);

	CHECK_EQ(MALI_GPU_ID_PRODUCT(t720), 0x0720);
	CHECK_EQ(MALI_GPU_ID_MAJOR(t720), 0);
	CHECK_EQ(MALI_GPU_ID_MINOR(t720), 0);
	CHECK_EQ(MALI_GPU_ID_STATUS(t720), 0);

	/* r1p3, status 0x1 (a plausible real-world encoding). */
	t720 = (MALI_PRODUCT_T72X << 16) | (1u << 12) | (3u << 4) | 1u;
	CHECK_EQ(MALI_GPU_ID_MAJOR(t720), 1);
	CHECK_EQ(MALI_GPU_ID_MINOR(t720), 3);
	CHECK_EQ(MALI_GPU_ID_STATUS(t720), 1);

	/* The product field is independent of the revision nibbles. */
	CHECK_EQ(MALI_GPU_ID_PRODUCT(t720), MALI_PRODUCT_T72X);

	/* MMU_FEATURES: VA bits [7:0], PA bits [15:8] - kbase
	 * mali_kbase_gpuprops.c:303-304.  A 32-bit VA / 40-bit PA part. */
	CHECK_EQ(MALI_MMU_VA_BITS(0x2820u), 32);
	CHECK_EQ(MALI_MMU_PA_BITS(0x2820u), 40);
	/* Fields must not bleed into each other. */
	CHECK_EQ(MALI_MMU_VA_BITS(0x2840u), 64);
	CHECK_EQ(MALI_MMU_PA_BITS(0x2840u), 40);
}

/* ------------------------------------------------------------------ */
/* 6. Map / walk / unmap                                              */
/* ------------------------------------------------------------------ */
static void test_map_walk(void)
{
	struct mali_pgtable pt;
	uint64_t pa = 0, attrs = 0;
	int i;
	static const uint64_t iovas[] = {
		0x00000000ULL, 0x00001000ULL, 0x00200000ULL,
		0x40000000ULL, 0x7FF00000ULL, 0xFFFFF000ULL,
	};

	fixture_init(&pt, 32);

	for (i = 0; i < (int)(sizeof(iovas) / sizeof(iovas[0])); i++) {
		uint64_t phys = 0x90000000ULL + (uint64_t)i * 0x10000ULL;
		int ret = mali_pgtable_map(&pt, iovas[i], phys, MALI_ATTR_GPU_RW);

		CHECK_EQ(ret, MALI_OK);
		ret = mali_pgtable_walk(&pt, iovas[i], &pa, &attrs);
		CHECK_EQ(ret, MALI_OK);
		CHECK_EQ(pa, phys);
		CHECK_EQ(attrs, MALI_ATTR_GPU_RW);
	}

	/* Mapping a second page in an already-populated sub-tree must reuse the
	 * tables (this is what exercises from_pa in the builder). */
	CHECK_EQ(mali_pgtable_map(&pt, 0x3000ULL, 0x91000000ULL,
				 MALI_ATTR_GPU_RO), MALI_OK);
	CHECK_EQ(mali_pgtable_walk(&pt, 0x3000ULL, &pa, &attrs), MALI_OK);
	CHECK_EQ(pa, 0x91000000ULL);
	CHECK_EQ(attrs, MALI_ATTR_GPU_RO);

	/* Unmapped page. */
	CHECK_EQ(mali_pgtable_walk(&pt, 0x5000ULL, &pa, &attrs), MALI_ENOENT);

	/* Double map. */
	CHECK_EQ(mali_pgtable_map(&pt, 0x3000ULL, 0x91000000ULL,
				 MALI_ATTR_GPU_RO), MALI_EEXIST);

	/* Misaligned and out-of-range arguments. */
	CHECK_EQ(mali_pgtable_map(&pt, 0x1234ULL, 0x90000000ULL,
				 MALI_ATTR_GPU_RW), MALI_EINVAL);
	CHECK_EQ(mali_pgtable_map(&pt, 0x1000ULL, 0x90000123ULL,
				 MALI_ATTR_GPU_RW), MALI_EINVAL);
	CHECK_EQ(mali_pgtable_map(&pt, 0x100000000ULL, 0x90000000ULL,
				 MALI_ATTR_GPU_RW), MALI_EINVAL);
	CHECK_EQ(mali_pgtable_map(&pt, 0x1000ULL, 0x10000000000ULL,
				 MALI_ATTR_GPU_RW), MALI_EINVAL);

	/* Every map above should have allocated three tables per 1 GiB
	 * sub-tree, never more than the heap. */
	CHECK(heap.allocs > 0 && heap.allocs <= HEAP_PAGES);
}

static void test_unmap(void)
{
	struct mali_pgtable pt;
	uint64_t pa;
	int allocs;

	fixture_init(&pt, 32);

	CHECK_EQ(mali_pgtable_map(&pt, 0x1000ULL, 0x90000000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK_EQ(mali_pgtable_map(&pt, 0x2000ULL, 0x90001000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK_EQ(heap.used, 3);	/* L1, L2, L3 for the first 1 GiB */

	allocs = heap.allocs;
	/* First unmap leaves the sibling mapped: no table may be freed. */
	CHECK_EQ(mali_pgtable_unmap(&pt, 0x1000ULL, 1), MALI_OK);
	CHECK_EQ(heap.frees, 0);
	CHECK_EQ(mali_pgtable_walk(&pt, 0x1000ULL, &pa, NULL), MALI_ENOENT);
	CHECK_EQ(mali_pgtable_walk(&pt, 0x2000ULL, &pa, NULL), MALI_OK);

	/* Second unmap tears the whole sub-tree down. */
	CHECK_EQ(mali_pgtable_unmap(&pt, 0x2000ULL, 1), MALI_OK);
	CHECK_EQ(heap.frees, 3);
	CHECK_EQ(heap.used, 0);
	CHECK_EQ(heap.allocs, allocs);

	/* Unmapping something that is not there. */
	CHECK_EQ(mali_pgtable_unmap(&pt, 0x1000ULL, 1), MALI_ENOENT);

	/* Unmap without free_empty keeps the tables (still walkable page count
	 * is zero, but the table allocation survives). */
	CHECK_EQ(mali_pgtable_map(&pt, 0x3000ULL, 0x90002000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK_EQ(mali_pgtable_unmap(&pt, 0x3000ULL, 0), MALI_OK);
	CHECK_EQ(heap.used, 3);
}

/* ------------------------------------------------------------------ */
/* 7. Attribute index independence                                     */
/* ------------------------------------------------------------------ */
static void test_attr_indices(void)
{
	struct mali_pgtable pt;
	uint64_t pa = 0, attrs = 0;

	fixture_init(&pt, 32);
	CHECK_EQ(mali_pgtable_map(&pt, 0, 0x90000000ULL,
				  (MALI_ATTR_GPU_RW &
				   ~MALI_PTE_MEMATTR_MASK) |
				  MALI_PTE_ATTR_IDX(MALI_MAIR_IDX_DEV)),
		 MALI_OK);
	CHECK_EQ(mali_pgtable_walk(&pt, 0, &pa, &attrs), MALI_OK);
	CHECK_EQ(attrs & MALI_PTE_MEMATTR_MASK,
		 MALI_PTE_ATTR_IDX(MALI_MAIR_IDX_DEV));
}
/* ------------------------------------------------------------------ */
/* 8. AS count / selection: popcount(AS_PRESENT), slot gating          */
/* ------------------------------------------------------------------ */
static void test_as_select(void)
{
	/* T720 MP2: 8 address spaces (gpu-verify.md: AS_PRESENT 0xFF). */
	CHECK_EQ(mali_as_count(0xFFu), 8);
	CHECK_EQ(mali_as_count(0x07u), 3);
	CHECK_EQ(mali_as_count(0x00u), 0);
	CHECK_EQ(mali_as_count(0xFFFFu), 16);

	/* Panfrost binds AS 0; any present slot passes the same gate. */
	CHECK(mali_as_select_ok(0xFFu, 0));
	CHECK(mali_as_select_ok(0xFFu, 7));
	CHECK(!mali_as_select_ok(0xFFu, 8));
	CHECK(!mali_as_select_ok(0xFFu, 16));
	CHECK(!mali_as_select_ok(0x00u, 0));

	/* Register math: AS15 TRANSCFG_HI stays inside the 0x5000 window. */
	CHECK(MALI_MMU_AS_REG(15, MALI_AS_TRANSCFG_HI) < 0x5000);
	CHECK_EQ(MALI_MMU_AS_REG(1, MALI_AS_COMMAND) -
		 MALI_MMU_AS_REG(0, MALI_AS_COMMAND),
		 MALI_MMU_AS_STRIDE);
}

/* ------------------------------------------------------------------ */
/* 9. 33-bit VA window (measured mmu_features 0x2821: VA 33, PA 40)    */
/* ------------------------------------------------------------------ */
static void test_va_window(void)
{
	struct mali_pgtable pt;
	uint64_t pa = 0;

	/* 8 GiB window: last byte maps, first byte above does not. */
	CHECK_EQ(mali_va_limit(33), 0x200000000ULL);
	CHECK_EQ(mali_va_limit(32), 0x100000000ULL);
	CHECK(mali_va_in_range(0x1FFFFFFFFULL, 33));
	CHECK(!mali_va_in_range(0x200000000ULL, 33));
	CHECK(!mali_va_in_range(0x100000000ULL, 32));

	/* L0 decodes VA bits 39+: any VA below 2^40 lives in L0[0]; the
	 * 33-bit space spans L1 entries 0..7 under it. */
	CHECK_EQ(mali_l0_entries(32), 1);
	CHECK_EQ(mali_l0_entries(33), 1);
	CHECK_EQ(mali_l0_entries(40), 2);
	CHECK_EQ(mali_l0_entries(48), 512);
	CHECK_EQ(mali_pte_index(0x1FFFFFFFFULL, 0), 0);
	CHECK_EQ(mali_pte_index(0x100000000ULL, 0), 0);
	CHECK_EQ(mali_pte_index(0xFFFFFFFFULL, 0), 0);
	CHECK_EQ(mali_pte_index(0x1FFFFFFFFULL, 1), 7);
	CHECK_EQ(mali_pte_index(0x100000000ULL, 1), 4);

	/* The page-table builder enforces the window end to end. */
	fixture_init(&pt, 33);
	CHECK_EQ(mali_pgtable_map(&pt, 0x1FFFFF000ULL, 0x90000000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK_EQ(mali_pgtable_walk(&pt, 0x1FFFFF000ULL, &pa, NULL), MALI_OK);
	CHECK_EQ(pa, 0x90000000ULL);
	CHECK_EQ(mali_pgtable_map(&pt, 0x200000000ULL, 0x90000000ULL,
				  MALI_ATTR_GPU_RW), MALI_EINVAL);
	CHECK_EQ(mali_pgtable_walk(&pt, 0x200000000ULL, &pa, NULL),
		 MALI_EINVAL);
}

/* ------------------------------------------------------------------ */
/* 10. LOCK region encoding (kbase lock_region / pf lock_region)       */
/* ------------------------------------------------------------------ */
static void test_lock_region(void)
{
	/* One page: width 11, base aligned down. */
	CHECK_EQ(mali_lock_region(0x12345000ULL, 0x1000ULL), 0x1234500BULL);
	/* Zero size means one page (kbase gracefully handles 0). */
	CHECK_EQ(mali_lock_region(0x12345000ULL, 0ULL), 0x1234500BULL);
	/* 8 pages (pow2): width 10 + fls(8)=4, exact pow2, no +1. */
	CHECK_EQ(mali_lock_region(0xABCDE000ULL, 0x8000ULL), 0xABCDE00EULL);
	/* 3 pages round up to 4: width 10 + fls(4) + 1 = 13. */
	CHECK_EQ(mali_lock_region(0x1000ULL, 0x3000ULL), 0x100DULL);
	/* 4 pages exact: width 10 + fls(4)=3, no round-up. */
	CHECK_EQ(mali_lock_region(0x1000ULL, 0x4000ULL), 0x100DULL);
	/* Unaligned IOVA aligns down to the page. */
	CHECK_EQ(mali_lock_region(0x1234ULL, 0x1000ULL), 0x100BULL);
	/* Full-TLB shape: LOCK(0, ~0UL) saturates at the 63 ceiling. */
	CHECK_EQ(mali_lock_region(0ULL, ~0ULL) & 0x3FULL,
		 MALI_LOCK_REGION_MAX_SIZE);
}

/* ------------------------------------------------------------------ */
/* 11. AS config values: TRANSTAB / MEMATTR / TRANSCFG / disable       */
/* ------------------------------------------------------------------ */
static void test_as_values(void)
{
	/* Enable TRANSTAB: pgd base | TABLE(3) | READ_INNER(4), no
	 * SHARE_OUTER on this COHERENCY_NONE SoC. */
	CHECK_EQ(mali_as_transtab(0x40000000ULL), 0x40000007ULL);
	CHECK_EQ(mali_as_transtab(0x40000000ULL) &
		 MALI_AS_TRANSTAB_SHARE_OUTER, 0);
	/* PGD base is page-aligned: low 12 bits never leak in. */
	CHECK_EQ(mali_as_transtab(0x40000FFFULL), 0x40000007ULL);
	/* Disable: TRANSTAB 0 (ADRMODE_UNMAPPED), then UPDATE. */
	CHECK_EQ(mali_as_transtab_disabled(),
		 MALI_AS_TRANSTAB_ADRMODE_UNMAPPED);
	/* MEMATTR: kbase LPAE table, the value programmed on this unit. */
	CHECK_EQ(mali_as_memattr_kbase(), MALI_MEMATTR_KBASE_LPAE);
	CHECK_EQ(mali_as_memattr_kbase() & 0xFFULL, 0x48ULL);
	/* TRANSCFG legacy: 0 on LPAE (no PTW_MEMATTR/PTW_SH bits set). */
	CHECK_EQ(MALI_AS_TRANSCFG_ADRMODE_LEGACY, 0);
	CHECK_EQ(MALI_AS_TRANSCFG_ADRMODE_MASK, 0xFu);
	/* AS_ACTIVE poll ceiling is kbase's 100k-loop contract. */
	CHECK_EQ(MALI_AS_INACTIVE_MAX_LOOPS, 100000);
}

/* ------------------------------------------------------------------ */
/* 12. Fault decode: class / access / decoder / source / IRQ pairing   */
/*     Single decoder (F-MMU4): byte-identical with p7580_mmu_fault_   */
/*     code/atype (iommu), pf7580_fault_exc/access (mmu/pf7580_as.h)   */
/*     and pf7580_fault_mmu/lpae_* (fault/pf7580_fault.h) after the    */
/*     short-code (bits 5:3) naming is retired — it mislabelled        */
/*     silicon faults. ATOMIC names UNKNOWN on this LPAE build.        */
/* ------------------------------------------------------------------ */
static void test_fault_decode(void)
{
	/* Exception classes (low byte of AS_FAULTSTATUS). */
	CHECK_EQ(mali_fault_exc_name(0xC0u)[0], 'T');	/* translation */
	CHECK_EQ(mali_fault_exc_name(0xC4u)[0], 'T');	/* level 4 too */
	CHECK_EQ(mali_fault_exc_name(0xC7u)[0], 'T');	/* top of range */
	CHECK_EQ(mali_fault_exc_name(0xC8u)[0], 'P');	/* permission */
	CHECK_EQ(mali_fault_exc_name(0xD0u)[0], 'T');	/* transtab bus */
	CHECK_EQ(mali_fault_exc_name(0xD2u)[0], 'T');	/* transtab bus */
	CHECK_EQ(mali_fault_exc_name(0xD7u)[0], 'T');	/* top of range */
	CHECK_EQ(mali_fault_exc_name(0xD8u)[0], 'A');	/* access flag */
	CHECK_EQ(mali_fault_exc_name(0x00u)[0], 'U');	/* unknown */
	CHECK_EQ(mali_fault_exc_name(0xC9u)[0], 'U');	/* gap: unknown */
	CHECK_EQ(mali_fault_exc_name(0xCFu)[0], 'U');	/* gap: unknown */
	CHECK_EQ(mali_fault_exc_name(0xD9u)[0], 'U');	/* gap: unknown */

	/* Access types (bits 9:8). */
	CHECK_EQ(mali_fault_access_name(0x200u)[0], 'R');	/* read */
	CHECK_EQ(mali_fault_access_name(0x300u)[0], 'W');	/* write */
	CHECK_EQ(mali_fault_access_name(0x100u)[0], 'E');	/* execute */
	CHECK_EQ(mali_fault_access_name(0x000u)[0], 'U');	/* atomic->UNK */

	/* Decoder (bit 10) vs slave fault. */
	CHECK(mali_fault_is_decoder(1u << 10));
	CHECK(!mali_fault_is_decoder(0u));

	/* Source ID (bits 31:16). */
	CHECK_EQ(mali_fault_source_id(0xABCD0000u), 0xABCDu);
	CHECK_EQ(mali_fault_source_id(0x0000FFFFu), 0u);

	/* 64-bit fault VA from the HI/LO pair. */
	CHECK_EQ(mali_fault_addr(0x1u, 0xFFFFF000u), 0x1FFFFF000ULL);
	CHECK_EQ(mali_fault_addr(0u, 0u), 0u);

	/* IRQ pairing: low 16 = page fault, high 16 = bus error per AS;
	 * a slot reporting both decodes as bus error. */
	CHECK(mali_irq_is_page_fault(0x00000001u, 0));
	CHECK(!mali_irq_is_page_fault(0x00010001u, 0));
	CHECK(mali_irq_is_bus_error(0x00010001u, 0));
	CHECK(!mali_irq_is_bus_error(0x00000001u, 0));
	CHECK(mali_irq_is_page_fault(0x00000004u, 2));
	CHECK(mali_irq_is_bus_error(0x00040000u, 2));
	CHECK(!mali_irq_is_page_fault(0x00000000u, 0));

	/* Single decoder: the full low byte is the class (the retired
	 * short-code view masked only bits 5:3 and named ADDRESS_SIZE /
	 * MEMORY_ATTRIBUTES classes that do not exist on this block). */
	CHECK_EQ(MALI_AS_FAULTSTATUS_EXC_MASK, 0xFFu);
	CHECK_EQ(MALI_AS_FAULTSTATUS_EXC_CODE_MASK, (0x7u << 3));
}

/* ------------------------------------------------------------------ */
/* 13. F-MMU1: LOCK width grows with the invalidated range (the review */
/*     blocker: a raw page address locked the minimum width, leaving   */
/*     stale TLB entries for any multi-page map).                       */
/* ------------------------------------------------------------------ */
static void test_lock_width(void)
{
	uint64_t one = mali_lock_region(0x100000ULL, 0x1000ULL);
	uint64_t multi = mali_lock_region(0x100000ULL, 0x4000ULL);
	uint64_t big = mali_lock_region(0x100000ULL, 0x100000ULL);
	uint64_t full = mali_lock_region(0ULL, ~0ULL);

	/* Base stays the mapping address; width covers the whole range. */
	CHECK_EQ(one & ~0x3FULL, 0x100000ULL);
	CHECK_EQ(multi & ~0x3FULL, 0x100000ULL);
	/* Wider ranges encode wider locks (monotone). */
	CHECK((one & 0x3FULL) <= (multi & 0x3FULL));
	CHECK((multi & 0x3FULL) <= (big & 0x3FULL));
	/* 4 pages (16 KiB): pow2, exact, width 10 + fls(4) = 13. */
	CHECK_EQ(multi & 0x3FULL, 13);
	/* 256 pages (1 MiB): width 10 + fls(256) = 19. */
	CHECK_EQ(big & 0x3FULL, 19);
	/* nr = 0 must not encode width 0 / raw address (old F-MMU1 bug). */
	CHECK((mali_lock_region(0x100000ULL, 0ULL) & 0x3FULL) >= 11);
	/* Full-TLB shape saturates at the 63 ceiling. */
	CHECK_EQ(full & 0x3FULL, MALI_LOCK_REGION_MAX_SIZE);
	CHECK_EQ(MALI_LOCK_REGION_MIN_SIZE, 11);
	CHECK_EQ(MALI_LOCK_REGION_MAX_SIZE, 63);
}

/* ------------------------------------------------------------------ */
/* 14. F-MMU2/3: barrier ordering + bounded poll contract. The kernel  */
/*     twin (p7580_mmu_invalidate) must wmb() LOCKADDR before LOCK and */
/*     step the AS_ACTIVE poll (10 us); the register values asserted   */
/*     here are what that sequence programs.                           */
/* ------------------------------------------------------------------ */
static void test_barrier_poll_contract(void)
{
	/* wmb before LOCK: LOCKADDR is programmed from the width-encoded
	 * region, never the raw page address — both halves carry it. */
	uint64_t region = mali_lock_region(0x200000ULL, 0x8000ULL);
	uint32_t lo = (uint32_t)(region & 0xFFFFFFFFULL);
	uint32_t hi = (uint32_t)(region >> 32);

	CHECK_EQ(lo & ~0x3Fu, 0x200000u);
	CHECK_EQ(lo & 0x3Fu, 14);	/* 8 pages: 10 + fls(8) = 14 */
	CHECK_EQ(hi, 0u);
	/* Poll contract: stepped 10 us x 100k (twin of pf7580_as_wait_
	 * ready), ~1 s ceiling — never a tight readl loop. */
	CHECK_EQ(MALI_AS_INACTIVE_MAX_LOOPS, 100000);
	CHECK_EQ(MALI_AS_POLL_STEP_US, 10);
	/* Command codes the sequence issues, in order LOCK/FLUSH/UNLOCK. */
	CHECK_EQ(MALI_AS_COMMAND_LOCK, 0x02);
	CHECK_EQ(MALI_AS_COMMAND_FLUSH_PT, 0x04);
	CHECK_EQ(MALI_AS_COMMAND_FLUSH_MEM, 0x05);
	CHECK_EQ(MALI_AS_COMMAND_UPDATE, 0x01);
}

/* ------------------------------------------------------------------ */
/* 15. F-MMU5: teardown empties the whole context (recursive walk).    */
/*     Map N pages across separate L2 sub-trees, unmap+free_empty must  */
/*     return every table; a second teardown-shaped pass finds none.   */
/* ------------------------------------------------------------------ */
static void test_teardown(void)
{
	struct mali_pgtable pt;
	uint64_t pa = 0;
	int i;

	fixture_init(&pt, 32);

	/* Three pages in three different 2 MiB L2 sub-trees. The two low
	 * pages share L0[0]/L1[0]/L2[0] paths differently: count tables
	 * from the builder, not from a hand estimate. */
	CHECK_EQ(mali_pgtable_map(&pt, 0x000000ULL, 0x90000000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK_EQ(mali_pgtable_map(&pt, 0x200000ULL, 0x90001000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK_EQ(mali_pgtable_map(&pt, 0x400000ULL, 0x90002000ULL,
				  MALI_ATTR_GPU_RW), MALI_OK);
	CHECK(heap.used > 0);

	/* Kernel twin: p7580_mmu_teardown() walks levels 0..2 and frees
	 * every child table, clearing leaves only (BO pages stay GEM's).
	 * The host builder's free_empty=1 is the same shape bottom-up. */
	for (i = 0; i < 3; i++) {
		uint64_t iova = (uint64_t)i * 0x200000ULL;

		CHECK_EQ(mali_pgtable_walk(&pt, iova, &pa, NULL), MALI_OK);
		CHECK_EQ(mali_pgtable_unmap(&pt, iova, 1), MALI_OK);
		CHECK_EQ(mali_pgtable_walk(&pt, iova, &pa, NULL),
			 MALI_ENOENT);
	}
	CHECK_EQ(heap.used, 0);
	CHECK_EQ(heap.frees, heap.allocs);

	/* Teardown of an empty context frees nothing and fails nothing. */
	CHECK_EQ(mali_pgtable_unmap(&pt, 0x600000ULL, 1), MALI_ENOENT);
	CHECK_EQ(heap.used, 0);
}


int main(void)
{
	printf("Mali Midgard LPAE / register-map unit tests\n");
	printf("-------------------------------------------\n");

	test_index_math();
	test_descriptors();
	test_as_config();
	test_registers();
	test_id_decode();
	test_map_walk();
	test_unmap();
	test_attr_indices();
	test_as_select();
	test_va_window();
	test_lock_region();
	test_as_values();
	test_fault_decode();
	test_lock_width();
	test_barrier_poll_contract();
	test_teardown();

	printf("-------------------------------------------\n");
	printf("%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
