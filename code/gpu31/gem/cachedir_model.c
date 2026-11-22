/*
 * cachedir_model.c - host-side model test for the gpu31-cachedir DMA
 * cache-maintenance ordering (gem/panfrost7580_gem.c).
 *
 * Why a model and not the kernel: the failure mode is a cache-op
 * DIRECTION error (an invalidate where a clean was needed), which only
 * shows on COHERENCY_NONE silicon with dirty CPU lines. The model
 * reproduces the exact arm64 3.10 backend semantics (arch/arm64/mm/
 * dma-mapping.c + cache.S) so the ORDER the driver uses can be tested
 * on the host, and so the pre-fix order can be shown to lose data.
 *
 * Backend semantics modelled (verified against the tree):
 *
 *   __dma_map_area(dir)     used by dma_map_page / sync_for_device
 *       dir == DMA_FROM_DEVICE            -> invalidate (DISCARD)
 *       otherwise (TO / BIDIRECTIONAL)    -> clean (WRITEBACK)
 *   __dma_unmap_area(dir)   used by dma_unmap_page / sync_for_cpu
 *       dir == DMA_TO_DEVICE              -> nothing
 *       otherwise (FROM / BIDIRECTIONAL)  -> invalidate (DISCARD)
 *
 * Aliases modelled per BO page:
 *   user  - userspace MT_NORMAL_NC mmap: uncached, writes go to DRAM,
 *           but may leave a stale CLEAN line in the kernel alias.
 *   kern  - kernel linear map (kmap/vmap): cached; reads populate it,
 *           writes make it dirty.
 *   dev   - the GPU: reads and writes DRAM, never the CPU cache.
 *
 * Every maintenance op is recorded (the "direction tracker") so the
 * driver's real call order can be compared against the ordering table.
 */
#include <stdio.h>
#include <string.h>

#define DRAM_NONE 0u

enum dma_dir {
	DMA_TO_DEVICE_M = 0,
	DMA_FROM_DEVICE_M = 1,
	DMA_BIDIRECTIONAL_M = 2
};

/* One BO page, one cache line. */
struct page_model {
	unsigned int dram;	/* DRAM / point of coherence */
	unsigned int alias;	/* kernel linear alias (cached) */
	int alias_valid;	/* a line is present */
	int alias_dirty;	/* ... and holds the only copy */
};

/* Direction tracker. */
enum op_kind {
	OP_NONE = 0,
	OP_CLEAN,
	OP_INVERT,	/* invalidate */
	OP_NOOP,
	OP_GPU_READ,
	OP_GPU_WRITE,
	OP_USER_WRITE,
	OP_KERN_WRITE,
	OP_KERN_READ
};

#define MAXOPS 64
static struct {
	enum op_kind kind;
} log_ops[MAXOPS];
static unsigned int nops;

static void track(enum op_kind k)
{
	if (nops < MAXOPS)
		log_ops[nops].kind = k;
	nops++;
}

static void track_reset(void)
{
	nops = 0;
	memset(log_ops, 0, sizeof(log_ops));
}

/* ---- arm64 backend primitives ------------------------------------ */

static void cache_clean(struct page_model *p)
{
	if (p->alias_valid && p->alias_dirty)
		p->dram = p->alias;	/* writeback: the only copy rescued */
	p->alias_dirty = 0;
	track(OP_CLEAN);
}

static void cache_invalidate(struct page_model *p)
{
	/* DISCARD: a dirty line dies here. This is the bug amplifier. */
	p->alias_valid = 0;
	p->alias_dirty = 0;
	track(OP_INVERT);
}

/* __dma_map_area(dir): the dma_map_page / sync_for_device leg. */
static void dma_map_area(struct page_model *p, enum dma_dir dir)
{
	if (dir == DMA_FROM_DEVICE_M)
		cache_invalidate(p);
	else
		cache_clean(p);
}

/* __dma_unmap_area(dir): the sync_for_cpu / dma_unmap_page leg. */
static void dma_unmap_area(struct page_model *p, enum dma_dir dir)
{
	if (dir == DMA_TO_DEVICE_M) {
		track(OP_NOOP);
		return;
	}
	cache_invalidate(p);
}

/* ---- CPU / GPU accesses ------------------------------------------- */

static void cpu_user_write(struct page_model *p, unsigned int v)
{
	/* MT_NORMAL_NC: straight to DRAM, no cache line installed/updated. */
	p->dram = v;
	track(OP_USER_WRITE);
}

static unsigned int cpu_user_read(struct page_model *p)
{
	track(OP_KERN_READ);
	return p->dram;		/* uncached read of the PoC */
}

static void cpu_kern_write(struct page_model *p, unsigned int v)
{
	p->alias = v;
	p->alias_valid = 1;
	p->alias_dirty = 1;
	track(OP_KERN_WRITE);
}

static unsigned int cpu_kern_read(struct page_model *p)
{
	unsigned int v;

	if (!p->alias_valid) {
		p->alias = p->dram;	/* line fill from DRAM */
		p->alias_valid = 1;
		p->alias_dirty = 0;
	}
	v = p->alias;
	track(OP_KERN_READ);
	return v;
}

static unsigned int gpu_read(struct page_model *p)
{
	track(OP_GPU_READ);
	return p->dram;		/* GPU bypasses the CPU caches */
}

static void gpu_write(struct page_model *p, unsigned int v)
{
	p->dram = v;
	track(OP_GPU_WRITE);
}

/* ---- driver model (mirrors panfrost7580_gem.c) -------------------- */

/* p7580_gem_dma_bind: dma_map_page(..., DMA_BIDIRECTIONAL) per page. */
static void drv_bind(struct page_model *p)
{
	dma_map_area(p, DMA_BIDIRECTIONAL_M);
}

/* p7580_gem_clean_for_device: sync_for_device(DMA_TO_DEVICE). */
static void drv_clean_for_device(struct page_model *p)
{
	dma_map_area(p, DMA_TO_DEVICE_M);
}

/* p7580_gem_invalidate_for_cpu: clean THEN sync_for_cpu(FROM_DEVICE). */
static void drv_invalidate_for_cpu(struct page_model *p)
{
	dma_map_area(p, DMA_TO_DEVICE_M);	/* writeback first (the fix) */
	dma_unmap_area(p, DMA_FROM_DEVICE_M);	/* now discard/refill */
}

/* p7580_gem_submit_sync. */
static void drv_submit_sync(struct page_model *p)
{
	drv_clean_for_device(p);
}

/* p7580_gem_job_done_sync (pre-signal + free_job + debug read). */
static void drv_job_done_sync(struct page_model *p)
{
	drv_invalidate_for_cpu(p);
}

/* p7580_gem_dma_unbind: clean, then dma_unmap_page(BIDIRECTIONAL). */
static void drv_unbind(struct page_model *p)
{
	drv_clean_for_device(p);
	dma_unmap_area(p, DMA_BIDIRECTIONAL_M);
}

/* ---- pre-fix model (bare invalidate where a clean was needed) ----- */

static void old_submit_sync(struct page_model *p)
{
	dma_unmap_area(p, DMA_BIDIRECTIONAL_M);	/* the bug */
}

/* ---- tests -------------------------------------------------------- */

static int failures;
static int checks;

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

static void ok_eq(unsigned int got, unsigned int want, const char *what)
{
	checks++;
	if (got != want) {
		failures++;
		printf("FAIL %s: got 0x%08x want 0x%08x\n", what, got, want);
	} else {
		printf("ok   %s\n", what);
	}
}

static void page_init(struct page_model *p)
{
	memset(p, 0, sizeof(*p));
	p->dram = DRAM_NONE;
}

#define PAT_A 0xC0C3E8E1u
#define PAT_B 0x5A5A1234u
#define PAT_G 0x0000BEEFu

/* 1. Backend direction table (the assumptions the fix rests on). */
static void test_backend_table(void)
{
	struct page_model p;

	/* sync_for_device(TO_DEVICE) must be a clean, not a discard. */
	page_init(&p);
	cpu_kern_write(&p, PAT_A);
	dma_map_area(&p, DMA_TO_DEVICE_M);
	ok_eq(p.dram, PAT_A, "for_device(TO_DEVICE) writes a dirty line back");
	ok(p.alias_dirty == 0, "for_device(TO_DEVICE) leaves the line clean");

	/* sync_for_cpu(BIDIRECTIONAL) discards. */
	page_init(&p);
	cpu_kern_write(&p, PAT_A);
	dma_unmap_area(&p, DMA_BIDIRECTIONAL_M);
	ok_eq(p.dram, DRAM_NONE, "for_cpu(BIDIRECTIONAL) DISCARDS a dirty line");
	ok(p.alias_valid == 0, "for_cpu(BIDIRECTIONAL) drops the line");

	/* for_device(FROM_DEVICE) invalidates. */
	page_init(&p);
	cpu_kern_write(&p, PAT_A);
	dma_map_area(&p, DMA_FROM_DEVICE_M);
	ok_eq(p.dram, DRAM_NONE, "for_device(FROM_DEVICE) DISCARDS a dirty line");

	/* dma_unmap_page(TO_DEVICE) does nothing. */
	page_init(&p);
	cpu_kern_write(&p, PAT_A);
	dma_unmap_area(&p, DMA_TO_DEVICE_M);
	ok_eq(p.dram, DRAM_NONE, "unmap(TO_DEVICE) performs no cache op");
	ok(p.alias_dirty == 1, "unmap(TO_DEVICE) leaves the dirty line");
}

/* 2. The ticket's device-test shape: CPU fills a shader-sized BO,
 * submit a job that references it, retire, read back. Data must
 * survive in DRAM and be what the GPU saw. */
static void test_cpu_fill_survives_readonly_job(void)
{
	struct page_model p;

	page_init(&p);
	cpu_user_write(&p, PAT_A);		/* Mesa uploads the ISA */
	drv_bind(&p);
	drv_submit_sync(&p);
	ok_eq(gpu_read(&p), PAT_A, "GPU reads the CPU-filled bytes");
	ok_eq(cpu_kern_read(&p), PAT_A, "kernel view reads the same bytes");
	/* read-only job: GPU does not write. Completion: */
	drv_job_done_sync(&p);
	ok_eq(cpu_user_read(&p), PAT_A, "user view intact after completion");
	ok_eq(cpu_kern_read(&p), PAT_A, "kernel view intact after completion");
	drv_unbind(&p);
	ok_eq(cpu_user_read(&p), PAT_A, "user view intact after unbind");
}

/* 3. Negative control: the old submit-side invalidate loses a line the
 * CPU filled through the kernel alias AFTER the BO was bound. (The
 * bind-time dma_map_page is itself a clean, so only a post-bind CPU
 * fill is exposed -- exactly the kmap/vmap fill and the descriptor
 * patch the driver does between submits.) */
static void test_old_submit_invalidate_loses_data(void)
{
	struct page_model p;

	page_init(&p);
	cpu_user_write(&p, PAT_A);
	drv_bind(&p);				/* earlier job bound the BO */
	cpu_kern_write(&p, PAT_B);		/* kernel-side fill, post-bind */
	old_submit_sync(&p);			/* pre-fix: bare invalidate */
	ok_eq(gpu_read(&p), PAT_A,
	      "OLD order: GPU reads stale DRAM (data lost)");
	ok_eq(p.dram, PAT_A, "OLD order: DRAM never received the CPU bytes");

	/* Same sequence with the fix: clean first. */
	page_init(&p);
	cpu_user_write(&p, PAT_A);
	drv_bind(&p);
	cpu_kern_write(&p, PAT_B);
	drv_submit_sync(&p);
	ok_eq(gpu_read(&p), PAT_B, "NEW order: GPU reads the CPU's latest bytes");
}

/* 3b. The same hazard at the pre-fix completion leg: a post-submit CPU
 * fill would be discarded by the bare invalidate. */
static void test_old_completion_invalidate_loses_data(void)
{
	struct page_model p;

	page_init(&p);
	cpu_user_write(&p, PAT_A);
	drv_bind(&p);
	drv_submit_sync(&p);
	cpu_kern_write(&p, PAT_B);		/* CPU refilled during the job */
	cache_invalidate(&p);			/* pre-fix completion op */
	ok_eq(cpu_kern_read(&p), PAT_A,
	      "OLD completion: post-submit CPU fill discarded");
}

/* 4. Snapshot/dump read: a stale CLEAN alias line must not fool the
 * read (userspace posted newer bytes straight to DRAM). */
static void test_snapshot_read_is_fresh(void)
{
	struct page_model p;
	unsigned int v;

	page_init(&p);
	cpu_user_write(&p, PAT_A);
	(void)cpu_kern_read(&p);		/* alias now holds PAT_A, clean */
	cpu_user_write(&p, PAT_B);		/* alias became stale */

	/* Pre-fix bare invalidate: also refreshes here, but it is the
	 * wrong op when the line is dirty (test 5). */
	(void)v;
	drv_invalidate_for_cpu(&p);
	v = cpu_kern_read(&p);
	ok_eq(v, PAT_B, "snapshot read sees DRAM after clean-then-invalidate");
}

/* 5. Snapshot read when the CPU filled through the kernel alias: the
 * clean must run so the invalidate does not throw the fill away. */
static void test_snapshot_dirty_line(void)
{
	struct page_model p;
	unsigned int v;

	page_init(&p);
	cpu_user_write(&p, PAT_A);
	cpu_kern_write(&p, PAT_B);		/* dirty: PAT_B only in cache */

	drv_invalidate_for_cpu(&p);
	v = cpu_kern_read(&p);
	ok_eq(v, PAT_B, "snapshot read keeps the CPU's dirty bytes");
	ok_eq(p.dram, PAT_B, "snapshot read wrote them back");

	/* Negative control: bare invalidate loses them. */
	page_init(&p);
	cpu_kern_write(&p, PAT_B);
	cache_invalidate(&p);
	ok_eq(cpu_kern_read(&p), DRAM_NONE,
	      "bare invalidate drops the dirty CPU fill (old bug)");
}

/* 6. GPU writes become visible: completion invalidate drops the stale
 * clean alias line. */
static void test_gpu_write_visible(void)
{
	struct page_model p;

	page_init(&p);
	cpu_user_write(&p, PAT_A);
	drv_submit_sync(&p);
	(void)cpu_kern_read(&p);		/* alias = PAT_A, clean */
	gpu_write(&p, PAT_G);
	ok_eq(cpu_kern_read(&p), PAT_A, "without sync the CPU sees the stale line");
	drv_job_done_sync(&p);
	ok_eq(cpu_kern_read(&p), PAT_G, "completion invalidate exposes GPU bytes");
	ok_eq(cpu_user_read(&p), PAT_G, "user view sees GPU bytes");
}

/* 7. Unbind must not discard a final CPU write. */
static void test_unbind_keeps_final_write(void)
{
	struct page_model p;

	page_init(&p);
	cpu_user_write(&p, PAT_A);
	cpu_kern_write(&p, PAT_B);
	drv_unbind(&p);
	ok_eq(p.dram, PAT_B, "unbind writes back before invalidating");
	ok_eq(p.alias_valid, 0, "unbind leaves no stale line");
}

/* 8. Direction tracker: the driver's real call order for one BO life
 * must match the ordering table exactly. */
static void test_order_trace(void)
{
	static const enum op_kind expect[] = {
		OP_USER_WRITE,		/* create + CPU fill (user WC) */
		OP_CLEAN,		/* bind: map_page(BIDIRECTIONAL) */
		OP_CLEAN,		/* submit: sync_for_device(TO) */
		OP_CLEAN,		/* snapshot: clean ... */
		OP_INVERT,		/* snapshot: ... then invalidate */
		OP_GPU_READ,		/* job runs (read-only reference) */
		OP_CLEAN,		/* pre-signal completion: clean ... */
		OP_INVERT,		/* pre-signal completion: ... invalidate */
		OP_CLEAN,		/* free_job: clean ... */
		OP_INVERT,		/* free_job: ... invalidate */
		OP_CLEAN,		/* debug_read_word (dump): clean ... */
		OP_INVERT,		/* debug_read_word: ... invalidate */
		OP_CLEAN,		/* unbind: clean ... */
		OP_INVERT		/* unbind: unmap(BIDIRECTIONAL) */
	};
	struct page_model p;
	unsigned int i, n;
	int same = 1;

	page_init(&p);
	track_reset();
	cpu_user_write(&p, PAT_A);		/* 1 create + fill */
	drv_bind(&p);				/* 2 bind */
	drv_submit_sync(&p);			/* 3 submit */
	drv_invalidate_for_cpu(&p);		/* 4 snapshot read */
	(void)gpu_read(&p);			/* 5 GPU reads only */
	drv_job_done_sync(&p);			/* 6 pre-signal completion */
	drv_job_done_sync(&p);			/* 7 free_job repeat */
	drv_job_done_sync(&p);			/* 8 dump read */
	drv_unbind(&p);				/* 9 unbind */

	n = sizeof(expect) / sizeof(expect[0]);
	if (nops != n) {
		printf("FAIL order trace length: got %u want %u\n", nops, n);
		failures++;
		checks++;
		return;
	}
	for (i = 0; i < n; i++) {
		if (log_ops[i].kind != expect[i]) {
			printf("FAIL order trace op %u: kind %d want %d\n",
			       i, (int)log_ops[i].kind, (int)expect[i]);
			same = 0;
		}
	}
	ok(same, "driver call order matches the clean-then-invalidate table");
	/* And the invariant the table exists for: no INVERT without a
	 * preceding CLEAN since the last CPU write. */
	{
		int saw_clean = 0, bad = 0;

		for (i = 0; i < nops && i < MAXOPS; i++) {
			enum op_kind k = log_ops[i].kind;

			if (k == OP_USER_WRITE || k == OP_KERN_WRITE)
				saw_clean = 0;
			else if (k == OP_CLEAN)
				saw_clean = 1;
			else if (k == OP_INVERT && !saw_clean)
				bad = 1;
		}
		ok(!bad, "no invalidate runs without a preceding clean");
	}
}

int main(void)
{
	printf("-- gpu31-cachedir host model (arm64 3.10 backend semantics) --\n");
	test_backend_table();
	test_cpu_fill_survives_readonly_job();
	test_old_submit_invalidate_loses_data();
	test_old_completion_invalidate_loses_data();
	test_snapshot_read_is_fresh();
	test_snapshot_dirty_line();
	test_gpu_write_visible();
	test_unbind_keeps_final_write();
	test_order_trace();
	printf("cachedir_model: %d checks, %d failure(s) -> %s\n",
	       checks, failures, failures ? "FAIL" : "PASS");
	return failures ? 1 : 0;
}
