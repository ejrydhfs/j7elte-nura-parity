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

	printf("-------------------------------------------\n");
	printf("%d checks, %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
