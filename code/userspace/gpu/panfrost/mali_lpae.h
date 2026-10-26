/*
 * mali_lpae.h - Midgard (T7xx) MMU page-table builder.
 *
 * This is the part of the panfrost port that can be built and exercised
 * without a GPU: the four-level, 9-bit-per-level, 4 KiB-granule LPAE page
 * table that both the out-of-tree kbase driver and mainline panfrost
 * program into AS_TRANSTAB.
 *
 * The format is pinned by two independent in-tree references:
 *
 *   kbase r15p0:
 *     - MIDGARD_MMU_TOPLEVEL..BOTTOMLEVEL are 0..3, 512 entries each,
 *       index = vpfn >> ((3 - level) * 9)  (mali_kbase_mmu.c:434-449,
 *       490, 1214-1250; KBASE_MMU_PAGE_ENTRIES 512 at :46)
 *     - leaf entries are type 1 ("ATE"), tables are type 3 ("PTE")
 *       (mali_kbase_mmu_mode_lpae.c:25-28, 175-190)
 *     - AF = bit 10, RD = bit 6, WR = bit 7, SH = bits 9:8, XN = bit 54,
 *       cache-attribute index = bits 4:2 (mali_kbase_mmu_mode_lpae.c:30-37,
 *       145-170)
 *
 *   mainline io-pgtable-arm (what panfrost uses):
 *     - ARM_MALI_LPAE keeps a full 4-level table regardless of IAS
 *       (io-pgtable-arm.c:1232-1236)
 *     - leaf descriptors are ARM_LPAE_PTE_TYPE_BLOCK (=1) for the Mali
 *       format even at the last level, not PTE_TYPE_PAGE (=3)
 *       (io-pgtable-arm.c:169-176, 377-390)
 *     - tables are ARM_LPAE_PTE_TYPE_TABLE (=3)
 *     - this format always uses SH_OS (io-pgtable-arm.c:560-569)
 *
 * The allocator is supplied by the caller so the same code runs in the
 * userspace unit test (malloc) and in the 3.10 kernel module
 * (__get_free_page / coherent pool).
 *
 * Not implemented here (deliberately; milestone M3 in design.md):
 *   - 2 MiB block mappings (level-2 leaf descriptors)
 *   - hierarchical permissions / batching
 *   - TLB and AS_COMMAND sequencing - that belongs to the kernel driver
 */
#ifndef MALI_LPAE_H
#define MALI_LPAE_H

#include "mali_hw.h"

/* Freestanding error codes so this header works in both worlds. */
#define MALI_OK		0
#define MALI_ENOENT	2
#define MALI_ENOMEM	12
#define MALI_EEXIST	17
#define MALI_EINVAL	22

/* One page-table page, as seen by the CPU (va) and by the MMU (pa). */
struct mali_table {
	uint64_t *va;
	uint64_t pa;
};

struct mali_pgtable_ops {
	/* Return a zeroed 4 KiB table.  The GPU reads it, so in the kernel
	 * this must come from a coherent pool. */
	int (*alloc)(void *cookie, struct mali_table *t);
	void (*free)(void *cookie, struct mali_table *t);
	/* Resolve a table descriptor back to its CPU mapping.  Needed to walk
	 * or extend a tree that was built earlier (e.g. across two map()
	 * calls, or from a second context). */
	int (*from_pa)(void *cookie, uint64_t pa, struct mali_table *t);
	/* Make freshly written tables visible to the MMU.  May be NULL when
	 * the memory is coherent. */
	void (*publish)(void *cookie);
};

struct mali_pgtable {
	uint64_t *pgd;				/* level-0 table, caller-owned */
	uint64_t pgd_pa;
	const struct mali_pgtable_ops *ops;
	void *cookie;
	unsigned int va_bits;			/* MMU_FEATURES[7:0] */
};

/* Index into the table at 'level' (0 = top) for a byte address. */
static inline unsigned int mali_pte_index(uint64_t iova, int level)
{
	uint64_t vpfn = iova >> MALI_LPAE_PAGE_SHIFT;

	return (unsigned int)((vpfn >> ((MALI_LPAE_LEVELS - 1 - level) *
					MALI_LPAE_BITS_PER_LEVEL)) &
			      (MALI_LPAE_ENTRIES - 1));
}

static inline unsigned int mali_pte_type(uint64_t pte)
{
	return (unsigned int)(pte & MALI_PTE_TYPE_MASK);
}

static inline uint64_t mali_pte_addr(uint64_t pte)
{
	return pte & MALI_PTE_ADDR_MASK;
}

/* Leaf descriptor: physical address + attribute bits + type 1. */
static inline uint64_t mali_pte_make_leaf(uint64_t pa, uint64_t attrs)
{
	return (pa & MALI_PTE_ADDR_MASK) |
	       (attrs & MALI_PTE_FLAGS_MASK) |
	       MALI_PTE_TYPE_BLOCK;
}

/* Table descriptor: physical address of the next level + type 3. */
static inline uint64_t mali_pte_make_table(uint64_t pa)
{
	return (pa & MALI_PTE_ADDR_MASK) | MALI_PTE_TYPE_TABLE;
}

static inline void mali_pgtable_init(struct mali_pgtable *pt, uint64_t *pgd,
				     uint64_t pgd_pa,
				     const struct mali_pgtable_ops *ops,
				     void *cookie, unsigned int va_bits)
{
	pt->pgd = pgd;
	pt->pgd_pa = pgd_pa;
	pt->ops = ops;
	pt->cookie = cookie;
	pt->va_bits = va_bits;
}

static inline int mali_pgtable_args_ok(const struct mali_pgtable *pt,
				       uint64_t iova, uint64_t pa)
{
	if ((iova | pa) & ((1ULL << MALI_LPAE_PAGE_SHIFT) - 1))
		return 0;
	if (pt->va_bits && iova >= (1ULL << pt->va_bits))
		return 0;
	if (pa != (pa & MALI_PTE_ADDR_MASK))
		return 0;
	return 1;
}

static inline int mali_table_is_empty(const struct mali_table *t)
{
	unsigned int i;

	for (i = 0; i < MALI_LPAE_ENTRIES; i++)
		if (t->va[i])
			return 0;
	return 1;
}

/*
 * Map one 4 KiB page, allocating missing intermediate tables.
 * Returns MALI_EEXIST if a leaf is already present, MALI_ENOMEM if the
 * allocator failed, MALI_EINVAL on misalignment or out-of-range addresses.
 */
static inline int mali_pgtable_map(struct mali_pgtable *pt, uint64_t iova,
				   uint64_t pa, uint64_t attrs)
{
	struct mali_table cur = { pt->pgd, pt->pgd_pa };
	int level;

	if (!mali_pgtable_args_ok(pt, iova, pa))
		return MALI_EINVAL;

	for (level = 0; level < MALI_LPAE_LEVELS - 1; level++) {
		unsigned int idx = mali_pte_index(iova, level);
		uint64_t entry = cur.va[idx];
		struct mali_table next;
		int ret;

		if (!entry) {
			ret = pt->ops->alloc(pt->cookie, &next);
			if (ret)
				return ret;
			cur.va[idx] = mali_pte_make_table(next.pa);
		} else if (mali_pte_type(entry) == MALI_PTE_TYPE_TABLE) {
			ret = pt->ops->from_pa(pt->cookie, mali_pte_addr(entry),
					       &next);
			if (ret)
				return ret;
		} else {
			return MALI_EINVAL;	/* block mapping in the way */
		}
		cur = next;
	}

	{
		unsigned int idx = mali_pte_index(iova, MALI_LPAE_LEVELS - 1);

		if (cur.va[idx] & MALI_PTE_TYPE_MASK)
			return MALI_EEXIST;
		cur.va[idx] = mali_pte_make_leaf(pa, attrs);
	}

	if (pt->ops->publish)
		pt->ops->publish(pt->cookie);
	return MALI_OK;
}

/*
 * Translate an address.  On success stores the physical address and, if
 * 'attrs' is non-NULL, the leaf's attribute bits (type bits excluded).
 * Returns MALI_ENOENT for an unmapped page.
 */
static inline int mali_pgtable_walk(const struct mali_pgtable *pt,
				    uint64_t iova, uint64_t *pa,
				    uint64_t *attrs)
{
	struct mali_table cur = { pt->pgd, pt->pgd_pa };
	int level;

	if ((iova & ((1ULL << MALI_LPAE_PAGE_SHIFT) - 1)) ||
	    (pt->va_bits && iova >= (1ULL << pt->va_bits)))
		return MALI_EINVAL;

	for (level = 0; level < MALI_LPAE_LEVELS - 1; level++) {
		unsigned int idx = mali_pte_index(iova, level);
		uint64_t entry = cur.va[idx];
		struct mali_table next;

		if (mali_pte_type(entry) != MALI_PTE_TYPE_TABLE)
			return MALI_ENOENT;
		if (pt->ops->from_pa(pt->cookie, mali_pte_addr(entry), &next))
			return MALI_EINVAL;
		cur = next;
	}

	{
		unsigned int idx = mali_pte_index(iova, MALI_LPAE_LEVELS - 1);
		uint64_t entry = cur.va[idx];

		if (mali_pte_type(entry) != MALI_PTE_TYPE_BLOCK)
			return MALI_ENOENT;
		if (pa)
			*pa = mali_pte_addr(entry) |
			      (iova & ((1ULL << MALI_LPAE_PAGE_SHIFT) - 1));
		if (attrs)
			*attrs = entry & MALI_PTE_FLAGS_MASK;
		return MALI_OK;
	}
}

/*
 * Clear one page.  With free_empty != 0, tables that became empty are
 * returned to the allocator (bottom-up).  The top-level pgd is never freed.
 */
static inline int mali_pgtable_unmap(struct mali_pgtable *pt, uint64_t iova,
				     int free_empty)
{
	struct mali_table path[MALI_LPAE_LEVELS];
	unsigned int idxs[MALI_LPAE_LEVELS];
	struct mali_table cur = { pt->pgd, pt->pgd_pa };
	int level;

	if ((iova & ((1ULL << MALI_LPAE_PAGE_SHIFT) - 1)) ||
	    (pt->va_bits && iova >= (1ULL << pt->va_bits)))
		return MALI_EINVAL;

	path[0] = cur;
	for (level = 0; level < MALI_LPAE_LEVELS - 1; level++) {
		unsigned int idx = mali_pte_index(iova, level);
		uint64_t entry = cur.va[idx];
		struct mali_table next;

		if (mali_pte_type(entry) != MALI_PTE_TYPE_TABLE)
			return MALI_ENOENT;
		if (pt->ops->from_pa(pt->cookie, mali_pte_addr(entry), &next))
			return MALI_EINVAL;
		idxs[level] = idx;
		cur = next;
		path[level + 1] = cur;
	}

	idxs[MALI_LPAE_LEVELS - 1] = mali_pte_index(iova, MALI_LPAE_LEVELS - 1);
	if (mali_pte_type(cur.va[idxs[MALI_LPAE_LEVELS - 1]]) !=
	    MALI_PTE_TYPE_BLOCK)
		return MALI_ENOENT;
	cur.va[idxs[MALI_LPAE_LEVELS - 1]] = 0;

	if (free_empty) {
		/* Bottom-up: never free the caller-owned pgd itself, but do
		 * clear its entry once the whole sub-tree is gone. */
		for (level = MALI_LPAE_LEVELS - 2; level >= 0; level--) {
			struct mali_table child = path[level + 1];

			if (!mali_table_is_empty(&child))
				break;
			path[level].va[idxs[level]] = 0;
			pt->ops->free(pt->cookie, &child);
		}
	}

	if (pt->ops->publish)
		pt->ops->publish(pt->cookie);
	return MALI_OK;
}

#endif /* MALI_LPAE_H */
