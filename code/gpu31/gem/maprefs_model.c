/*
 * maprefs_model.c - host-side model test for the gpu31-mmaprefs
 * userspace-mapping page lifetime (gem/panfrost7580_gem.c).
 *
 * Why a model: the failure being fixed is a page reference that the
 * mapping does not hold, so no host test can observe it on real pages
 * without the kernel. What CAN be pinned down here is the reference
 * accounting and the VMA open/close pairing, against the exact 3.10
 * mm/DRM behaviour the driver relies on:
 *
 *  1. Reference identities for a local SHMEM BO page. The model keeps
 *     them apart, because the bug and the fix only differ in which of
 *     them the mapping owns:
 *       cache_ref  shmem page-cache reference (shmem_read_mapping_page
 *                  returns the page with the cache's own reference plus
 *                  one for the caller)
 *       bo_ref     the BO's reference (p7580_shmem_alloc; dropped by
 *                  p7580_shmem_free)
 *       pin_ref    the MAPPING reference (p7580_gem_map_pin, dropped by
 *                  p7580_gem_map_unpin when the last VMA closes) --
 *                  absent before this ticket
 *       pte_ref    the PTE reference (mm/memory.c insert_page's
 *                  get_page, zapped by zap_pte_range's put_page) --
 *                  absent before this ticket (remap_pfn_range inserts
 *                  special PTEs and refcounts nothing)
 *     page_count = cache + bo + pin + pte. A page may be handed back to
 *     the allocator (clear_highpage / __GFP_ZERO reuse, the event the
 *     gpu31-shaderbo trace shows) only when that sum is 0.
 *
 *  2. VMA pairing. 3.10 __split_vma() (mm/mmap.c:2524) and copy_vma()
 *     (mm/mmap.c:2939) call ->open on the new VMA; every VMA is closed
 *     once. dup_mmap() does NOT call ->open -- which is why the driver
 *     sets VM_DONTCOPY (as the vendor kbase mapping does on this
 *     device) and why the model has a fork case that must not unpin.
 *     p7580_gem_vm_get/put mirror that pairing; the pin is taken on the
 *     0->1 transition and dropped on 1->0, so a split/mremap pair keeps
 *     the pages pinned and still balances the gets and puts.
 *
 *  3. The closure probes of the gpu31-shaderbo report §5.1
 *     (p7580_gem_debug_page_ledger, printed at the 0xCF read and at
 *     SUBMIT): what the ledger must show for a mapped, populated BO --
 *     count >= 3, mapcount >= 1 once faulted, uptodate set, the ISA
 *     words still present after an allocator zero attempt, and (the
 *     third verdict) whether the page-cache identity
 *     (page_mapping()/page_index()) shows the same page with lost
 *     references or a replaced, re-referenced page.
 *
 * The model is single-threaded; the driver serialises both transitions
 * with dev->struct_mutex, and the tests assert the resulting invariant
 * (a 0->1 take can never interleave with a 1->0 put) by construction.
 */
#include <stdio.h>
#include <string.h>

#define M_SH_NR		4	/* pages per modelled BO */
#define M_MAX_VMA	8

/* Backing selectors, mirroring P7580_BO_*. */
#define M_CMA		0u
#define M_SHMEM		1u

struct mpage {
	int cache_ref;
	int bo_ref;
	int pin_ref;
	int pte_ref;
	int mapcount;		/* rmap entries == mapped PTEs */
	int uptodate;
	int lru;
	int swapbacked;
	int dirty;
	/* Page-cache identity (page_mapping()/page_index() in the ledger):
	 * a page removed from its inode and re-added elsewhere -- or a
	 * reused struct page -- changes the pair. */
	int mapping_id;
	unsigned long page_index;
	unsigned int w[4];	/* first four words (the ISA probe) */
};

struct mbo {
	struct mpage pages[M_SH_NR];
	unsigned int nr;
	unsigned int vma_refs;
	unsigned int map_pinned;
	unsigned int backing;
	unsigned int has_sgt;	/* PRIME-imported shape */
};

struct mvma {
	struct mbo *bo;
	int pte[M_SH_NR];
	int live;
	int dontcopy;
};

static int checks;
static int failures;
static int warnings;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("FAIL %s\n", what);
	} else {
		printf("ok   %s\n", what);
	}
}

static int page_count(const struct mpage *p)
{
	return p->cache_ref + p->bo_ref + p->pin_ref + p->pte_ref;
}

/* ---------------------------------------------------------------- */
/* Kernel-side model primitives                                      */
/* ---------------------------------------------------------------- */

static void bo_create(struct mbo *bo, unsigned int nr, unsigned int backing,
		      unsigned int has_sgt)
{
	unsigned int i;

	memset(bo, 0, sizeof(*bo));
	bo->nr = nr;
	bo->backing = backing;
	bo->has_sgt = has_sgt;
	for (i = 0; i < nr; i++) {
		/* shmem_read_mapping_page: one page-cache reference plus
		 * the reference handed to the caller (the BO pin). */
		bo->pages[i].cache_ref = 1;
		bo->pages[i].bo_ref = 1;
		bo->pages[i].uptodate = 1;
		bo->pages[i].lru = 1;
		bo->pages[i].mapping_id = 1;	/* the BO's private inode */
		bo->pages[i].page_index = i;
	}
}

/* p7580_bo_cpu_mapped: local SHMEM only. */
static int bo_cpu_mapped(const struct mbo *bo)
{
	return bo->backing == M_SHMEM && bo->nr && !bo->has_sgt;
}

/* p7580_gem_map_pin / p7580_gem_map_unpin */
static void map_pin(struct mbo *bo)
{
	unsigned int i;

	for (i = 0; i < bo->nr; i++)
		bo->pages[i].pin_ref++;
	bo->map_pinned = 1;
}

static void map_unpin(struct mbo *bo)
{
	unsigned int i;

	if (!bo->map_pinned)
		return;
	bo->map_pinned = 0;
	for (i = 0; i < bo->nr; i++)
		bo->pages[i].pin_ref--;
}

/* p7580_gem_vm_get / p7580_gem_vm_put (dev->struct_mutex held) */
static void vm_get(struct mbo *bo)
{
	if (bo->vma_refs++ == 0)
		map_pin(bo);
}

static void vm_put(struct mbo *bo)
{
	if (!bo->vma_refs) {
		/* Unbalanced close: refuse to release another VMA's
		 * reference. The driver WARN_ON_ONCE()s here. */
		warnings++;
		return;
	}
	if (--bo->vma_refs == 0)
		map_unpin(bo);
}

/* p7580_gem_mmap, local SHMEM path: flags become the fault-driven
 * VM_MIXEDMAP shape and the mapping takes its reference. */
static struct mvma *vma_mmap(struct mbo *bo)
{
	static struct mvma vmas[M_MAX_VMA];
	static unsigned int slot;
	struct mvma *v;
	unsigned int i;

	if (!slot)
		memset(vmas, 0, sizeof(vmas));
	v = &vmas[slot++ % M_MAX_VMA];
	v->bo = bo;
	v->live = 1;
	for (i = 0; i < M_SH_NR; i++)
		v->pte[i] = 0;
	vm_get(bo);
	return v;
}

/* __split_vma()/copy_vma(): ->open on the new VMA, no new pin. */
static struct mvma *vma_open_copy(struct mbo *bo)
{
	static struct mvma vmas[M_MAX_VMA];
	static unsigned int slot;
	struct mvma *v;

	if (!slot)
		memset(vmas, 0, sizeof(vmas));
	v = &vmas[slot++ % M_MAX_VMA];
	v->bo = bo;
	v->live = 1;
	v->dontcopy = 1;
	/* A split keeps each address's PTE in exactly one of the two
	 * parts (the page tables do not move), so the new part starts
	 * with no PTE of its own; faulting it later is what would add
	 * one. */
	vm_get(bo);
	return v;
}

/* p7580_gem_fault -> vm_insert_page (insert_page: get_page +
 * page_add_file_rmap; -EBUSY when the PTE is already there). */
static int vma_fault(struct mvma *v, unsigned int off)
{
	struct mpage *p = &v->bo->pages[off];

	if (off >= v->bo->nr)
		return -1;
	if (v->pte[off])
		return -22;	/* -EBUSY */
	p->pte_ref++;
	p->mapcount++;
	v->pte[off] = 1;
	return 0;
}

static void vma_zap(struct mvma *v)
{
	unsigned int i;

	for (i = 0; i < M_SH_NR; i++) {
		if (!v->pte[i])
			continue;
		v->bo->pages[i].pte_ref--;
		v->bo->pages[i].mapcount--;
		v->pte[i] = 0;
	}
}

/* VMA teardown: the mm zaps the PTEs first, then ->close runs. */
static void vma_close(struct mvma *v)
{
	vma_zap(v);
	vm_put(v->bo);
	v->live = 0;
}

/* The page allocator handing the page out again: only legal when every
 * reference is gone (count == 0). clear_highpage-class zeroing. */
static int allocator_recycle(struct mpage *p)
{
	int i;

	if (page_count(p) != 0)
		return -1;
	for (i = 0; i < 4; i++)
		p->w[i] = 0;
	p->uptodate = 1;
	return 0;
}

/* The observed external event (gpu31-shaderbo §3): whatever held the
 * BO's own reference and the page-cache reference drops both, while the
 * BO object and its userspace mapping are still live. Modelled as the
 * worst case the userspace evidence admits; the mapping references are
 * NOT touched by it, which is the whole point of the fix. */
static void external_freer_event(struct mbo *bo, unsigned int off)
{
	bo->pages[off].bo_ref = 0;
	bo->pages[off].cache_ref = 0;
}

/* Old shape (pre-fix), for the negative control: the mapping is
 * remap_pfn_range/VM_PFNMAP -- special PTEs, no reference at all. */
static void old_shape_fault(struct mvma *v, unsigned int off)
{
	(void)v;
	(void)off;		/* remap_pfn_range: no refcount, no rmap */
}

/* ---------------------------------------------------------------- */
/* Closure probes (gpu31-shaderbo §5.1 ledger)                      */
/* ---------------------------------------------------------------- */

struct ledger {
	int count;
	int mapcount;
	unsigned int vma;
	unsigned int pin;
	int up;
	int lru;
	int swapbacked;
	int dirty;
	int mapping_id;
	unsigned long page_index;
	unsigned int w[4];
};

static int take_ledger(struct mbo *bo, unsigned int off, struct ledger *l)
{
	struct mpage *p;

	if (off >= bo->nr)
		return -1;
	p = &bo->pages[off];
	l->count = page_count(p);
	l->mapcount = p->mapcount;
	l->vma = bo->vma_refs;
	l->pin = bo->map_pinned;
	l->up = p->uptodate;
	l->lru = p->lru;
	l->swapbacked = p->swapbacked;
	l->dirty = p->dirty;
	l->mapping_id = p->mapping_id;
	l->page_index = p->page_index;
	memcpy(l->w, p->w, sizeof(l->w));
	return 0;
}

static void fill_isa(struct mbo *bo, unsigned int off)
{
	bo->pages[off].w[0] = 0xc87a9835;	/* fragment ISA, first two */
	bo->pages[off].w[1] = 0x01403d3d;	/* words (shaderbo trace) */
	bo->pages[off].w[2] = 0x00000030;
	bo->pages[off].w[3] = 0x00000000;
	bo->pages[off].dirty = 0;
}

/* ---------------------------------------------------------------- */
/* Tests                                                            */
/* ---------------------------------------------------------------- */

/* The fixed shape: mmap + write + the observed freer + allocator. */
static void test_mapped_page_cannot_be_recycled(void)
{
	struct mbo bo;
	struct mvma *v;

	printf("-- mapped page holds its own references --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	ok(page_count(&bo.pages[0]) == 2, "create: cache_ref + BO ref = 2");
	v = vma_mmap(&bo);
	ok(page_count(&bo.pages[0]) == 3, "mmap (VM_MIXEDMAP): mapping pin -> 3");
	ok(bo.map_pinned == 1 && bo.vma_refs == 1, "mmap: pin taken, vma_refs 1");
	ok(vma_fault(v, 0) == 0, "first fault: vm_insert_page succeeds");
	ok(page_count(&bo.pages[0]) == 4, "fault: PTE reference -> 4");
	ok(bo.pages[0].mapcount == 1, "fault: page_mapcount 1");
	fill_isa(&bo, 0);
	ok(vma_fault(v, 0) == -22, "second fault: -EBUSY (PTE present)");
	ok(page_count(&bo.pages[0]) == 4 && bo.pages[0].mapcount == 1,
	   "second fault: no extra reference, no extra rmap");
	external_freer_event(&bo, 0);
	ok(page_count(&bo.pages[0]) == 2,
	   "worst-case freer: mapping pin + PTE ref survive -> 2");
	ok(allocator_recycle(&bo.pages[0]) != 0,
	   "allocator cannot recycle a mapped page");
	ok(bo.pages[0].w[0] == 0xc87a9835 && bo.pages[0].w[1] == 0x01403d3d,
	   "ISA words intact after the refused recycle");
	vma_close(v);
	ok(page_count(&bo.pages[0]) == 0,
	   "close: zap drops the PTE ref, close drops the pin -> 0");
	ok(bo.map_pinned == 0 && bo.vma_refs == 0, "close: pin released");
	ok(allocator_recycle(&bo.pages[0]) == 0,
	   "allocator may recycle only after every reference is gone");
}

/* Negative control: the pre-fix remap_pfn_range/VM_PFNMAP shape. */
static void test_old_shape_loses_the_page(void)
{
	struct mbo bo;
	struct mvma *v;

	printf("-- negative control: mapping without page references --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	v = vma_mmap(&bo);
	bo.map_pinned = 0;			/* old shape: no pin */
	bo.pages[0].pin_ref = 0;
	old_shape_fault(v, 0);
	fill_isa(&bo, 0);
	ok(page_count(&bo.pages[0]) == 2,
	   "old shape: only cache_ref + BO ref, mapping adds 0");
	external_freer_event(&bo, 0);
	ok(page_count(&bo.pages[0]) == 0,
	   "old shape: the observed freer leaves count 0");
	ok(allocator_recycle(&bo.pages[0]) == 0,
	   "old shape: allocator recycles the live page");
	ok(bo.pages[0].w[0] == 0x0 && bo.pages[0].w[1] == 0x0,
	   "old shape: ISA words zeroed while the mapping is live");
	ok(bo.pages[0].mapcount == 0,
	   "old shape: no rmap entry, so nothing marks the page live");
	v->live = 0;		/* the VMA was still mapped all along */
	ok(page_count(&bo.pages[0]) == 0, "old shape: nothing to unpin");
}

/* Split (partial unmap / mremap): ->open on the new VMA. */
static void test_split_keeps_and_balances(void)
{
	struct mbo bo;
	struct mvma *a, *b;
	int pin_before;

	printf("-- split: open/close pairing keeps the pin --\n");
	bo_create(&bo, 2, M_SHMEM, 0);
	a = vma_mmap(&bo);
	vma_fault(a, 0);
	pin_before = bo.pages[0].pin_ref;
	b = vma_open_copy(&bo);
	ok(bo.vma_refs == 2 && bo.map_pinned == 1,
	   "split: vma_refs 2, pin still held (no second pin)");
	ok(bo.pages[0].pin_ref == pin_before,
	   "split: no extra pin reference taken");
	vma_close(a);
	ok(bo.map_pinned == 1 && bo.vma_refs == 1,
	   "first part closes: pages still pinned for the other part");
	ok(page_count(&bo.pages[0]) == 3,
	   "first part closes: its PTE ref dropped, pin still there");
	vma_close(b);
	ok(bo.vma_refs == 0 && bo.map_pinned == 0,
	   "last part closes: pin dropped");
	ok(bo.pages[0].pin_ref == 0,
	   "split: gets and puts balance over the pair");
	ok(page_count(&bo.pages[0]) == 2 && bo.pages[0].pte_ref == 0 &&
	   bo.pages[0].mapcount == 0,
	   "split: unmapped page is back to cache+BO references only");
}

/* mremap: copy_vma opens the new VMA before the old one is unmapped. */
static void test_mremap_balances(void)
{
	struct mbo bo;
	struct mvma *v, *moved;

	printf("-- mremap: open before unmap still balances --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	v = vma_mmap(&bo);
	vma_fault(v, 0);
	moved = vma_open_copy(&bo);
	ok(bo.vma_refs == 2 && bo.map_pinned == 1,
	   "mremap: new VMA opened, pin unchanged");
	vma_close(v);
	ok(bo.vma_refs == 1 && bo.map_pinned == 1,
	   "mremap: old VMA unmapped, pin still held");
	vma_close(moved);
	ok(bo.vma_refs == 0 && bo.map_pinned == 0 &&
	   bo.pages[0].pin_ref == 0,
	   "mremap: balanced, pin released exactly once");
}

/* Two independent mappings of the same BO (shaderbo magic2 probe). */
static void test_two_mappings(void)
{
	struct mbo bo;
	struct mvma *a, *b;

	printf("-- two independent mappings of one BO --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	a = vma_mmap(&bo);
	vma_fault(a, 0);
	fill_isa(&bo, 0);
	b = vma_mmap(&bo);
	ok(bo.map_pinned == 1 && bo.pages[0].pin_ref == 1,
	   "second mapping: pin taken once for the BO");
	vma_fault(b, 0);
	ok(bo.pages[0].mapcount == 2, "two PTEs, page_mapcount 2");
	external_freer_event(&bo, 0);
	ok(allocator_recycle(&bo.pages[0]) != 0 &&
	   bo.pages[0].w[0] == 0xc87a9835,
	   "both mappings see the page survive the freer");
	vma_close(a);
	ok(bo.map_pinned == 1, "one mapping left: still pinned");
	vma_close(b);
	ok(bo.map_pinned == 0 && bo.pages[0].pin_ref == 0,
	   "last mapping closed: pin released");
}

/* The 0xCF-read and SUBMIT ledger closures. */
static void test_closure_ledger(void)
{
	struct mbo bo;
	struct mvma *v;
	struct ledger l;

	printf("-- closure probes: ledger at the 0xCF read and at SUBMIT --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	v = vma_mmap(&bo);
	vma_fault(v, 0);
	fill_isa(&bo, 0);
	/* 0xCF read path: after clean-then-invalidate. */
	ok(take_ledger(&bo, 0, &l) == 0, "0xCF: ledger taken");
	ok(l.count >= 3, "0xCF: count >= 3 (cache+BO+pin, +1 for the PTE)");
	ok(l.mapcount >= 1, "0xCF: mapcount >= 1 (the mapping's PTE)");
	ok(l.up == 1, "0xCF: PageUptodate set");
	ok(l.pin == 1 && l.vma == 1, "0xCF: pin=1 vma=1");
	ok(l.w[0] == 0xc87a9835 && l.w[1] == 0x01403d3d,
	   "0xCF: ISA words read back");
	/* SUBMIT path: after p7580_gem_submit_sync. */
	ok(take_ledger(&bo, 0, &l) == 0, "SUBMIT: ledger taken");
	ok(l.count >= 3, "SUBMIT: count >= 3");
	ok(l.w[0] == 0xc87a9835 && l.w[1] == 0x01403d3d,
	   "SUBMIT: ISA words present");
	/* The report's failing sequence, replayed: the freer runs between
	 * the compile and the first emit. */
	external_freer_event(&bo, 0);
	ok(take_ledger(&bo, 0, &l) == 0, "post-freer: ledger taken");
	ok(l.count >= 2, "post-freer: count >= 2 (pin + PTE)");
	ok(l.w[0] == 0xc87a9835 && l.w[1] == 0x01403d3d,
	   "post-freer: ISA words still there at the 0xCF read");
	vma_close(v);
}

/* The final guard in p7580_gem_free_object. */
static void test_free_path_guards(void)
{
	struct mbo bo;
	struct mvma *v;

	printf("-- free path: leaked pin is refused, not double-put --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	vm_put(&bo);
	ok(warnings == 1, "close without open: refused and warned");
	ok(bo.pages[0].pin_ref == 0, "close without open: no put issued");
	v = vma_mmap(&bo);
	vma_fault(v, 0);
	/* Pretend the VMA never closed (accounting bug): the mm still
	 * zapped its PTEs, and free must drop the pin itself instead of
	 * stranding page references. */
	v->live = 0;
	vma_zap(v);
	if (bo.map_pinned)
		map_unpin(&bo);
	bo.vma_refs = 0;
	ok(bo.map_pinned == 0 && bo.pages[0].pin_ref == 0,
	   "free: leaked mapping pin dropped");
	/* BO teardown: the BO's own reference goes last. */
	bo.pages[0].bo_ref = 0;
	ok(page_count(&bo.pages[0]) == 1,
	   "free: only the page-cache reference is left");
}

/* Scope: CMA and PRIME-imported mappings are not pinned here. */
static void test_scope_cma_imported(void)
{
	struct mbo cma, imp;

	printf("-- scope: CMA and imported mappings are untouched --\n");
	bo_create(&cma, 1, M_CMA, 0);
	ok(!bo_cpu_mapped(&cma), "CMA BO: not a local-SHMEM mapping");
	bo_create(&imp, 1, M_SHMEM, 1);
	ok(!bo_cpu_mapped(&imp), "imported BO (sgt): not pinned");
	ok(imp.pages[0].pin_ref == 0 && cma.pages[0].pin_ref == 0,
	   "no mapping reference taken for either");
}

/* The 0x400-marker verdict: a page whose references were lost keeps its
 * page-cache identity; a page that was removed and re-added (or a reused
 * struct page) does not. This is the third branch the flash check needs,
 * because it separates "reference lost" from "replaced".
 */
static void test_ledger_identity_names_replacement(void)
{
	struct mbo bo;
	struct mvma *v;
	struct ledger before, after;

	printf("-- ledger identity: reference loss vs a replaced page --\n");
	bo_create(&bo, 1, M_SHMEM, 0);
	v = vma_mmap(&bo);
	vma_fault(v, 0);
	fill_isa(&bo, 0);
	ok(take_ledger(&bo, 0, &before) == 0, "identity: baseline ledger");
	external_freer_event(&bo, 0);
	ok(take_ledger(&bo, 0, &after) == 0, "identity: post-freer ledger");
	ok(after.mapping_id == before.mapping_id &&
	   after.page_index == before.page_index,
	   "identity: reference loss keeps mapping/index (same page)");
	ok(after.w[0] == 0xc87a9835,
	   "identity: reference loss leaves the words intact");
	/* Replaced: the page left its inode, was zeroed, and the struct
	 * page now serves another inode/index (what `up`/`lru` alone can
	 * miss when the page is re-referenced immediately). */
	bo.pages[0].cache_ref = 0;
	bo.pages[0].bo_ref = 0;
	bo.pages[0].pin_ref = 0;
	bo.pages[0].pte_ref = 0;
	v->pte[0] = 0;
	bo.pages[0].mapcount = 0;
	ok(allocator_recycle(&bo.pages[0]) == 0, "identity: page recycled");
	bo.pages[0].mapping_id = 2;		/* another inode */
	bo.pages[0].page_index = 17;
	bo.pages[0].uptodate = 1;
	bo.pages[0].lru = 1;
	ok(take_ledger(&bo, 0, &after) == 0, "identity: replacement ledger");
	ok(after.mapping_id != before.mapping_id ||
	   after.page_index != before.page_index,
	   "identity: replaced page changes mapping/index");
	ok(after.w[0] == 0x0 && after.up == 1,
	   "identity: replaced page is up but zeroed (not a lost ref)");
}

int main(void)
{
	printf("-- gpu31-mmaprefs host model (3.10 mm/DRM reference semantics) --\n");
	test_mapped_page_cannot_be_recycled();
	test_old_shape_loses_the_page();
	test_split_keeps_and_balances();
	test_mremap_balances();
	test_two_mappings();
	test_closure_ledger();
	test_ledger_identity_names_replacement();
	test_free_path_guards();
	test_scope_cma_imported();
	printf("maprefs_model: %d checks, %d failure(s) -> %s\n",
	       checks, failures, failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
