/* SPDX-License-Identifier: GPL-2.0 */
/*
 * chainsplit_test.c - host test for the gpu31-chainsplit submit path.
 *
 * The descriptor walk and the isolate/rewrite rules are the real pure-C
 * helpers from jobs/panfrost7580_jm_hw.h (the same header the driver
 * compiles). The scheduling part is a pure-C model of p7580_drv_submit
 * + p7580_drv_split_queue in drivers/gpu/drm/panfrost/
 * panfrost7580_submit.c: one sched job per chain node, queued in chain
 * order on one slot entity, each node depending on the previous node's
 * armed fence, with the ioctl out_sync installed on the LAST queued
 * node only.
 *
 * Models (no kernel headers, no device):
 *   T1 chain walk + isolate: Mesa's WRITE_VALUE -> VERTEX -> TILER
 *      sample (gpu31-chainexec §2): types, next pointers, deps, and the
 *      rewritten shadow header (next=NULL, deps=0, index=pos+1).
 *   T2 ordering: every node after the first depends on the immediately
 *      previous node's fence; all nodes use the submit's slot.
 *   T3 single out-fence: exactly one replace, target = last node fence.
 *   T4 error unwinding: queue failure at node k leaves only 0..k-1
 *      queued, publishes/installs the last QUEUED fence, releases the
 *      unqueued nodes' shadow refs, and balances PM/BO references when
 *      the queued nodes retire.
 *   T5 single-node pass-through: a one-node chain with no dep indices
 *      keeps the original descriptor and allocates no shadow BO.
 *   T6 32-bit next form + unsupported type rejection + chain cap.
 *
 * Build: cc -Wall -Wextra -std=gnu89 -o chainsplit_test chainsplit_test.c
 *        && ./chainsplit_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../jobs/panfrost7580_jm_hw.h"

static int fails;

#define CHECK(cond) do {					\
	if (!(cond)) {						\
		printf("FAIL %d: %s\n", __LINE__, #cond);	\
		fails++;					\
	}							\
} while (0)

/* ------------------------------------------------------------------ */
/* T1: real helper surface on the measured Mesa chain.                */
/* ------------------------------------------------------------------ */

/* Words 0..7 of each measured descriptor (gpu31-chainexec §2). */
static const uint32_t wv_hdr[8] = {
	0x00000001, 0x00000000, 0x00000000, 0x00000000,
	0x00020005,	/* Is64 | type 2 (WRITE_VALUE) | index 2 */
	0x00000000,	/* dep1 0 / dep2 0 */
	0x035840c0, 0x00000000	/* next = 0x35840c0 */
};
static const uint32_t vs_hdr[8] = {
	0x00000001, 0x00000000, 0x00000000, 0x00000000,
	0x0001000b,	/* Is64 | type 5 (VERTEX) | index 1 */
	0x00000000,
	0x03584000, 0x00000000	/* next = 0x3584000 */
};
static const uint32_t tiler_hdr[8] = {
	0x00000001, 0x00000000, 0x00000000, 0x00000000,
	0x0003000f,	/* Is64 | type 7 (TILER) | index 3 */
	0x00020001,	/* dep1 1 (vertex) / dep2 2 (write value) */
	0x00000000, 0x00000000	/* next = NULL */
};

static void test_chain_decode(void)
{
	uint32_t t[8];

	CHECK(p7580_chain_hdr_is64(wv_hdr) == 1);
	CHECK(p7580_chain_hdr_type(wv_hdr) == P7580_JOB_TYPE_WRITE_VALUE);
	CHECK(p7580_chain_hdr_index(wv_hdr) == 2);
	CHECK(p7580_chain_hdr_next(wv_hdr) == 0x35840c0ull);
	CHECK(p7580_chain_job_bytes(P7580_JOB_TYPE_WRITE_VALUE) == 56);

	CHECK(p7580_chain_hdr_type(vs_hdr) == P7580_JOB_TYPE_VERTEX);
	CHECK(p7580_chain_hdr_index(vs_hdr) == 1);
	CHECK(p7580_chain_hdr_next(vs_hdr) == 0x3584000ull);
	CHECK(p7580_chain_job_bytes(P7580_JOB_TYPE_VERTEX) == 192);

	CHECK(p7580_chain_hdr_type(tiler_hdr) == P7580_JOB_TYPE_TILER);
	CHECK(p7580_chain_hdr_index(tiler_hdr) == 3);
	CHECK(p7580_chain_hdr_dep1(tiler_hdr) == 1);
	CHECK(p7580_chain_hdr_dep2(tiler_hdr) == 2);
	CHECK(p7580_chain_hdr_next(tiler_hdr) == 0);
	CHECK(p7580_chain_job_bytes(P7580_JOB_TYPE_TILER) == 192);
	/* Generated Mesa lengths (midgard_pack.h): a VERTEX (COMPUTE_JOB)
	 * is as long as the tiler; +96 truncates Textures..FBD. */
	CHECK(p7580_chain_job_bytes(P7580_JOB_TYPE_FRAGMENT) == 64);
	CHECK(p7580_chain_job_bytes(P7580_JOB_TYPE_NULL) == 32);

	/* Isolate the tiler into the last shadow slot: no next, no deps,
	 * renumbered to its split position + 1, type/Is64 preserved. */
	memcpy(t, tiler_hdr, sizeof(t));
	p7580_chain_hdr_isolate(t, 2);
	CHECK(p7580_chain_hdr_next(t) == 0);
	CHECK(p7580_chain_hdr_dep1(t) == 0);
	CHECK(p7580_chain_hdr_dep2(t) == 0);
	CHECK(p7580_chain_hdr_index(t) == 3);
	CHECK(p7580_chain_hdr_type(t) == P7580_JOB_TYPE_TILER);
	CHECK(p7580_chain_hdr_is64(t) == 1);

	/* Same for the write-value node at position 0. */
	memcpy(t, wv_hdr, sizeof(t));
	p7580_chain_hdr_isolate(t, 0);
	CHECK(p7580_chain_hdr_next(t) == 0);
	CHECK(p7580_chain_hdr_index(t) == 1);
	CHECK(p7580_chain_hdr_type(t) == P7580_JOB_TYPE_WRITE_VALUE);

	/* Shadow slots are 64-byte aligned and never overlap even for the
	 * largest (192-byte tiler) node. */
	CHECK(P7580_CHAIN_SLOT_STRIDE >= 192);
	CHECK((P7580_CHAIN_SLOT_STRIDE % 64) == 0);
	CHECK(P7580_CHAIN_MAX_JOBS * P7580_CHAIN_SLOT_STRIDE <= 4096);
}

/* ------------------------------------------------------------------ */
/* T2/T3/T4: model of the submit split and the scheduler queue.       */
/* ------------------------------------------------------------------ */

#define MAXN 8

struct fake_fence {
	int signaled;
	int refs;
};

struct fake_job {
	struct fake_fence *fence;	/* this node's armed fence */
	struct fake_fence *dep;		/* node this job waits for */
	int slot;
	int queued;
};

struct model {
	struct fake_job job[MAXN];
	unsigned int nodes;
	unsigned int queued;
	int slot;
	struct fake_fence *out_installed;
	unsigned int replaces;
	struct fake_fence *serial_prev;
	/* reference ledgers */
	int pm_armed;
	int pm_released;
	int user_refs;		/* caller lookup refs */
	int shadow_refs;	/* one per node, released by queued jobs */
	int shadow_unmaps;
};

static void model_init(struct model *m, unsigned int nodes, int slot)
{
	unsigned int i;

	memset(m, 0, sizeof(*m));
	m->nodes = nodes;
	m->slot = slot;
	/* Static fence pool, reset per model: distinct within a model. */
	{
		static struct fake_fence pool[16];
		static unsigned int used;

		for (i = 0; i < nodes; i++) {
			m->job[i].fence = &pool[used++ % 16];
			m->job[i].fence->signaled = 0;
			m->job[i].fence->refs = 1;
			m->job[i].slot = slot;
		}
	}
	m->user_refs = 3;	/* model: three submitted BOs */
	m->shadow_refs = nodes;
}

/* Mirrors p7580_drv_split_queue() up to the drm_sched submit. */
static int model_split_queue(struct model *m, unsigned int pos,
			     struct fake_fence *prev, int fail_at)
{
	struct fake_job *j = &m->job[pos];

	if ((int)pos == fail_at)
		return -1;
	j->slot = m->slot;
	j->dep = prev;		/* pos 0: serial prev; else previous node */
	j->queued = 1;
	m->queued++;
	/* node lifetime: one PM arm, one own BO ref, one shadow ref */
	m->pm_armed++;
	m->user_refs++;		/* per-job reference (caller refs remain) */
	return 0;
}

/* Mirrors the driver's out_sync install + serial publish + gap release. */
static void model_finish(struct model *m, struct fake_fence *last_fence)
{
	unsigned int i;

	if (m->queued && last_fence) {
		m->out_installed = last_fence;
		m->replaces++;
	}
	if (m->queued)
		m->serial_prev = last_fence;
	/* unqueued nodes' shadow refs are dropped here */
	for (i = m->queued; i < m->nodes; i++) {
		if (--m->shadow_refs == 0)
			m->shadow_unmaps++;
	}
}

/* Simulate retire of every queued job (free_job). */
static void model_retire_all(struct model *m)
{
	unsigned int i;

	for (i = 0; i < m->nodes; i++) {
		if (!m->job[i].queued)
			continue;
		m->pm_released++;
		m->user_refs--;
		if (--m->shadow_refs == 0)
			m->shadow_unmaps++;
		m->job[i].queued = 0;
	}
}

static void test_ordering_and_out_fence(void)
{
	struct model m;
	struct fake_fence *last = NULL;
	unsigned int i;

	model_init(&m, 3, 1);
	/* No serial predecessor: node 0 is dependency-free. */
	last = NULL;
	for (i = 0; i < 3; i++) {
		CHECK(model_split_queue(&m, i, last, -1) == 0);
		last = m.job[i].fence;
	}
	model_finish(&m, last);

	/* T2: chain order on one slot, each node depends on the previous. */
	CHECK(m.queued == 3);
	CHECK(m.job[0].dep == NULL);
	CHECK(m.job[1].dep == m.job[0].fence);
	CHECK(m.job[2].dep == m.job[1].fence);
	for (i = 0; i < 3; i++)
		CHECK(m.job[i].slot == 1);

	/* T3: exactly one out-fence install, on the final node. */
	CHECK(m.replaces == 1);
	CHECK(m.out_installed == m.job[2].fence);
	CHECK(m.serial_prev == m.job[2].fence);

	/* PM/BO/shadow lifetimes: three jobs own one each. */
	CHECK(m.pm_armed == 3);
	CHECK(m.user_refs == 3 + 3);
	CHECK(m.shadow_refs == 3);
	model_retire_all(&m);
	CHECK(m.pm_released == 3);
	CHECK(m.user_refs == 3);	/* caller lookup refs only */
	CHECK(m.shadow_refs == 0);
	CHECK(m.shadow_unmaps == 1);
}

static void test_error_unwind(void)
{
	struct model m;
	struct fake_fence *last = NULL;
	unsigned int i;

	/* T4: fail while queueing node 1 of 3. */
	model_init(&m, 3, 1);
	last = NULL;
	for (i = 0; i < 3; i++) {
		int rc = model_split_queue(&m, i, last, 1);

		if (rc) {
			CHECK(i == 1);
			break;
		}
		last = m.job[i].fence;
	}
	model_finish(&m, last);

	CHECK(m.queued == 1);
	CHECK(m.job[0].queued == 1);
	CHECK(m.job[1].queued == 0);
	CHECK(m.job[2].queued == 0);
	/* The single out-fence covers what the GPU accepted. */
	CHECK(m.replaces == 1);
	CHECK(m.out_installed == m.job[0].fence);
	CHECK(m.serial_prev == m.job[0].fence);
	/* Unqueued nodes released their shadow refs; the queued one keeps
	 * its own until retire. */
	CHECK(m.shadow_refs == 1);
	CHECK(m.shadow_unmaps == 0);

	model_retire_all(&m);
	CHECK(m.pm_armed == 1);
	CHECK(m.pm_released == 1);
	CHECK(m.user_refs == 3);	/* caller lookup refs only */
	CHECK(m.shadow_refs == 0);
	CHECK(m.shadow_unmaps == 1);

	/* Fail on the first node: nothing queued, no out-fence install,
	 * no serial publish, all shadow refs released. */
	model_init(&m, 3, 1);
	CHECK(model_split_queue(&m, 0, NULL, 0) != 0);
	model_finish(&m, NULL);
	CHECK(m.queued == 0);
	CHECK(m.replaces == 0);
	CHECK(m.out_installed == NULL);
	CHECK(m.serial_prev == NULL);
	CHECK(m.shadow_refs == 0);
	CHECK(m.shadow_unmaps == 1);	/* last release unmaps */
}

/* ------------------------------------------------------------------ */
/* T5/T6: pass-through decision, 32-bit next, unsupported type.       */
/* ------------------------------------------------------------------ */

static int need_shadow(const uint32_t hdr[MAXN][8], unsigned int nodes)
{
	return nodes > 1 ||
		p7580_chain_hdr_dep1(hdr[0]) ||
		p7580_chain_hdr_dep2(hdr[0]);
}

static void test_passthrough_and_forms(void)
{
	uint32_t hdr[MAXN][8];
	uint32_t next32[8] = {
		0, 0, 0, 0,
		0x00000012,	/* Is64 clear | type 9 (FRAGMENT) */
		0, 0x12340000, 0
	};

	/* Fragment submit: one node, next NULL, deps 0 -> no shadow. */
	hdr[0][4] = 0x00000013;	/* Is64 | type 9 */
	hdr[0][5] = 0;
	hdr[0][6] = 0;
	hdr[0][7] = 0;
	CHECK(need_shadow((const uint32_t (*)[8])hdr, 1) == 0);

	/* A one-node chain carrying dependency indices still needs the
	 * isolate path (the indices name jobs absent from the dispatch). */
	hdr[0][5] = 0x00020001;
	CHECK(need_shadow((const uint32_t (*)[8])hdr, 1) == 1);

	/* 32-bit next form: Is64 clear -> word 6 is the pointer; word 7
	 * is not read. */
	CHECK(p7580_chain_hdr_is64(next32) == 0);
	CHECK(p7580_chain_hdr_next(next32) == 0x12340000ull);
	CHECK(p7580_chain_hdr_type(next32) == P7580_JOB_TYPE_FRAGMENT);

	/* Unsupported/reserved type is rejected by the walker (0 bytes),
	 * and the cap bounds the walk. */
	CHECK(p7580_chain_job_bytes(0) == 0);
	CHECK(p7580_chain_job_bytes(6) == 0);
	CHECK(p7580_chain_job_bytes(P7580_JOB_TYPE_NULL) == 32);
	CHECK(P7580_CHAIN_MAX_JOBS == 16);
}

int main(void)
{
	test_chain_decode();
	test_ordering_and_out_fence();
	test_error_unwind();
	test_passthrough_and_forms();

	if (fails) {
		printf("chainsplit_test: %d FAILURE(S)\n", fails);
		return 1;
	}
	printf("chainsplit_test: all checks passed\n");
	return 0;
}
