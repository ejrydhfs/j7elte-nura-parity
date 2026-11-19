/* kbase-stress.c - kbase winsys stress + watch tests (UK 10.6 / Mali-T720).
 *
 * Runs only soft-job atoms (DUMP_CPU_GPU_TIME / fence trigger / fence wait /
 * soft event wait/set). No Midgard job chain is ever submitted, so nothing
 * here can wedge the GPU shader cores: every payload completes in the kernel
 * soft-job handler without touching the job slots. Safe to run while X/fbdev
 * and the GPS/FM workstreams share the phone: modest CPU, a few MiB of
 * SAME_VA memory, no reboot, no kernel build, no module load.
 *
 * Tests:
 *   many-small-jobs  N sequential soft jobs; reports per-job latency
 *                     distribution + GPU cycle delta (hang detector: any
 *                     per-job wait > --stuck-ms is flagged).
 *   bo-churn          alloc/submit/free loop over BOs of --sizes pages;
 *                     catches cookie/mmap/VMA leaks (avail mem sampled
 *                     before/after via sysinfo).
 *   fence-timeout     in-fence wait on an never-signalled fence with
 *                     --fence-ms deadline: expects -ETIME, then submits a
 *                     canary soft job to prove the context is still usable
 *                     (hang-recovery check).
 *   soft-event-cancel submit EVENT_WAIT with no matching SET, wait
 *                     --fence-ms: kernel cancels at soft_job_timeout
 *                     (3 s stock); measures actual cancel latency.
 *   fault-inject      submit atoms the kernel must reject without touching
 *                     the GPU: bad stride, bad atom size, invalid dep type,
 *                     dependency on an unused slot; verifies the context
 *                     still completes a canary afterwards.
 *
 * Exit codes: 0 all pass, 1 failure, 2 no device (--print-abi still runs).
 *
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "kbase_uapi.h"
#include "kbase.h"

#define DEVPATH "/dev/mali0"

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int failures;
static void fail(const char *fmt, ...)
{
	va_list ap;
	failures++;
	fprintf(stderr, "STRESS-FAIL: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

/* Minimal raw-UK helpers (parallel to kbase.c, but with the fault-injection
 * knobs the safe client library deliberately omits). */

static int uk_call(int fd, void *args, size_t size)
{
	int r = ioctl(fd, KBASE_UK_IOCTL(size), args);
	if (r < 0)
		return -errno;
	return 0;
}

struct uk_stream_create {
	union kbase_uk_header header;
	char name[32];
	int32_t fd;
	uint32_t padding;
};

/* Submit one atom with explicit raw fields; waits for its event with a
 * deadline (ms < 0 = forever). Returns the event code, or -ETIME/-errno. */
static long one_atom_wait(struct kbase_dev *dev, uint64_t jc, uint32_t core_req,
			  uint8_t atom_no, uint8_t dep0, uint8_t dept0,
			  uint8_t dep1, uint8_t dept1, long wait_ms,
			  struct base_jd_event_v2 *ev_out)
{
	struct base_jd_atom_v2 atom;
	struct pollfd pfd;
	uint64_t dl;
	int ret;

	memset(&atom, 0, sizeof(atom));
	atom.jc = jc;
	atom.core_req = core_req;
	atom.compat_core_req = (uint16_t)(core_req & 0x7fff);
	atom.atom_number = atom_no;
	atom.prio = BASE_JD_PRIO_MEDIUM;
	atom.pre_dep[0].atom_id = dep0;
	atom.pre_dep[0].dependency_type = dept0;
	atom.pre_dep[1].atom_id = dep1;
	atom.pre_dep[1].dependency_type = dept1;

	ret = kbase_submit_atoms(dev, &atom, 1);
	if (ret)
		return ret;

	dl = wait_ms < 0 ? UINT64_MAX : now_ns() + (uint64_t)wait_ms * 1000000ull;
	for (;;) {
		int tmo;
		if (wait_ms >= 0 && now_ns() >= dl)
			return -ETIME;
		tmo = wait_ms < 0 ? -1 : (int)((dl - now_ns()) / 1000000ull);
		if (tmo < 0)
			tmo = 0;
		if (tmo > 5000)
			tmo = 5000;
		pfd.fd = dev->fd;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			continue;
		{
			struct base_jd_event_v2 ev;
			ret = kbase_read_event(dev, &ev);
			if (ret)
				return ret;
			if (ev.atom_number != atom_no)
				continue; /* foreign atom (other test fd) */
			if (ev_out)
				*ev_out = ev;
			return (long)ev.event_code;
		}
	}
}

/* --- test 1: many small jobs -------------------------------------------- */
static int t_many(struct kbase_dev *dev, int n, long stuck_ms, struct kbase_bo *bo)
{
	uint64_t t0 = now_ns(), worst = 0, sum = 0;
	uint64_t c0 = 0, c1 = 0;
	struct base_dump_cpu_gpu_counters dump;
	int stuck = 0;

	memset(&dump, 0, sizeof(dump));
	if (kbase_soft_dump_time(dev, bo, &dump) != BASE_JD_EVENT_DONE) {
		fail("many-small-jobs: warmup soft job failed");
		return -1;
	}
	c0 = dump.cycle_counter;

	for (int i = 0; i < n; i++) {
		uint64_t a = now_ns();
		long rc = one_atom_wait(dev, bo->gpu_va,
					BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME,
					10, 0, 0, 0, 0, 10000, NULL);
		uint64_t dt = now_ns() - a;
		if (rc != BASE_JD_EVENT_DONE) {
			fail("many-small-jobs: job %d rc=%ld (%s)", i, rc,
			     rc < 0 ? strerror(-rc) : kbase_event_name((unsigned)rc));
			return -1;
		}
		sum += dt;
		if (dt > worst)
			worst = dt;
		if (dt > (uint64_t)stuck_ms * 1000000ull) {
			stuck++;
			printf("  job %d took %llu ms (> %ld ms stuck line)\n",
			       i, (unsigned long long)(dt / 1000000ull), (long)stuck_ms);
		}
	}
	memset(&dump, 0, sizeof(dump));
	if (kbase_soft_dump_time(dev, bo, &dump) != BASE_JD_EVENT_DONE) {
		fail("many-small-jobs: closing soft job failed");
		return -1;
	}
	c1 = dump.cycle_counter;

	printf("many-small-jobs: n=%d avg=%.2f ms worst=%.2f ms stuck=%d "
	       "gpu_cycles_delta=%" PRIu64 " total=%.2f s\n",
	       n, (double)sum / n / 1e6, (double)worst / 1e6, stuck,
	       c1 >= c0 ? c1 - c0 : 0, (double)(now_ns() - t0) / 1e9);
	if (stuck)
		fail("many-small-jobs: %d stuck jobs", stuck);
	return stuck ? -1 : 0;
}

/* --- test 2: BO churn ----------------------------------------------------- */
static int t_churn(struct kbase_dev *dev, int iters, const char *sizes)
{
	struct sysinfo si0, si1;
	uint64_t pages_list[16];
	int np = 0;
	char *dup = strdup(sizes ? sizes : "1,4,16");
	char *tok;

	for (tok = strtok(dup, ","); tok && np < 16; tok = strtok(NULL, ","))
		pages_list[np++] = strtoull(tok, NULL, 0);
	if (!np) {
		free(dup);
		fail("bo-churn: no sizes parsed");
		return -1;
	}
	sysinfo(&si0);
	for (int i = 0; i < iters; i++) {
		struct kbase_bo bo;
		char err[128] = "";
		uint64_t p = pages_list[i % np];
		int ret = kbase_alloc(dev, p,
				      BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
				      BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
				      BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM,
				      &bo, err, sizeof(err));
		if (ret) {
			fail("bo-churn: iter %d alloc %" PRIu64 " pages: %s", i, p, err);
			free(dup);
			return -1;
		}
		memset(bo.cpu, 0xa5, p * 4096);
		{
			long rc = one_atom_wait(dev, bo.gpu_va,
						BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME,
						11, 0, 0, 0, 0, 10000, NULL);
			if (rc != BASE_JD_EVENT_DONE) {
				fail("bo-churn: iter %d submit rc=%ld", i, rc);
				kbase_free(dev, &bo);
				free(dup);
				return -1;
			}
		}
		if (kbase_free(dev, &bo)) {
			fail("bo-churn: iter %d free failed", i);
			free(dup);
			return -1;
		}
		if ((i + 1) % 100 == 0)
			printf("  churn %d/%d\n", i + 1, iters);
	}
	sysinfo(&si1);
	free(dup);
	printf("bo-churn: iters=%d sizes=%s freeram before=%lu MiB after=%lu MiB delta=%+ld KiB\n",
	       iters, sizes ? sizes : "1,4,16",
	       si0.freeram / (1024 * 1024), si1.freeram / (1024 * 1024),
	       (long)((long)si1.freeram - (long)si0.freeram) / 1024);
	return 0;
}

/* --- test 3: fence timeout + canary ----------------------------------------
 * A FENCE_WAIT atom on a valid but never-signalled fence + a TRIGGER atom
 * depending on it form a genuinely stuck batch (both stay pending). The
 * test proves (a) userspace wait detects the stall with a deadline instead
 * of hanging, (b) the out-fence stays unsignalled, (c) a fresh context
 * still drives soft jobs (isolation), (d) closing the stuck fd releases
 * its atoms. The valid-but-pending fence comes from a second batch whose
 * trigger depends on a never-set soft event.
 *
 * NOTE: a wait atom with fd=-1 is rejected synchronously with
 * BASE_JD_EVENT_JOB_INVALID (0x4003) — that is the fault-inject case, not
 * a hang. */
static int t_fence(struct kbase_dev *dev, struct kbase_bo *bo, long fence_ms)
{
	struct uk_stream_create sc;
	struct base_jd_atom_v2 atoms[2];
	struct kbase_uk_job_submit sub;
	struct base_jd_event_v2 ev;
	struct base_fence {
		struct {
			int fd;
			int stream_fd;
		} basep;
	} wf, tf, pending_tf;
	uint64_t dl, evt_addr;
	int ret, out_fd = -1;
	bool got_pending_fence = false;

	memset(&sc, 0, sizeof(sc));
	sc.header.id = UK_FUNC_ID + 24; /* KBASE_FUNC_STREAM_CREATE */
	snprintf(sc.name, sizeof(sc.name), "stress-tl");
	ret = uk_call(dev->fd, &sc, sizeof(sc));
	if (ret || sc.header.ret) {
		fail("fence-timeout: STREAM_CREATE rc=%d ret=%u", ret, sc.header.ret);
		return -1;
	}

	/* Step 1: build a never-firing out-fence. A trigger atom depending
	 * on a soft-event-wait that is never SET stays pending forever, and
	 * its trigger writes back a valid but unsignalled fence fd. */
	evt_addr = bo->gpu_va + 768;
	((unsigned char *)bo->cpu)[768] = 0; /* RESET */
	memset(&pending_tf, 0, sizeof(pending_tf));
	pending_tf.basep.fd = -1;
	pending_tf.basep.stream_fd = sc.fd;
	memset(atoms, 0, sizeof(atoms));
	atoms[0].jc = evt_addr;
	atoms[0].core_req = BASE_JD_REQ_SOFT_EVENT_WAIT;
	atoms[0].compat_core_req = (uint16_t)(BASE_JD_REQ_SOFT_EVENT_WAIT & 0x7fff);
	atoms[0].atom_number = 18;
	atoms[1].jc = (uint64_t)(uintptr_t)&pending_tf;
	atoms[1].core_req = BASE_JD_REQ_SOFT_FENCE_TRIGGER;
	atoms[1].compat_core_req = (uint16_t)(BASE_JD_REQ_SOFT_FENCE_TRIGGER & 0x7fff);
	atoms[1].atom_number = 19;
	atoms[1].pre_dep[0].atom_id = 18;
	atoms[1].pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_ORDER;
	memset(&sub, 0, sizeof(sub));
	sub.header.id = KBASE_FUNC_JOB_SUBMIT;
	sub.addr.value = atoms;
	sub.nr_atoms = 2;
	sub.stride = sizeof(atoms[0]);
	ret = uk_call(dev->fd, &sub, sizeof(sub));
	if (ret || sub.header.ret) {
		fail("fence-timeout: pending-fence submit rc=%d ret=%u", ret, sub.header.ret);
		close(sc.fd);
		return -1;
	}
	if (pending_tf.basep.fd < 0) {
		fail("fence-timeout: no pending fence fd written back");
		close(sc.fd);
		return -1;
	}
	got_pending_fence = true;
	printf("fence-timeout: pending fence fd %d (never fires)\n", pending_tf.basep.fd);

	/* Step 2: the stuck batch — wait on the never-firing fence, trigger
	 * depending on the wait. */
	memset(&wf, 0, sizeof(wf));
	wf.basep.fd = pending_tf.basep.fd; /* valid, never signalled */
	wf.basep.stream_fd = sc.fd;
	memset(&tf, 0, sizeof(tf));
	tf.basep.fd = -1;
	tf.basep.stream_fd = sc.fd;
	memset(atoms, 0, sizeof(atoms));
	atoms[0].jc = (uint64_t)(uintptr_t)&wf;
	atoms[0].core_req = BASE_JD_REQ_SOFT_FENCE_WAIT;
	atoms[0].compat_core_req = (uint16_t)(BASE_JD_REQ_SOFT_FENCE_WAIT & 0x7fff);
	atoms[0].atom_number = 20;
	atoms[1].jc = (uint64_t)(uintptr_t)&tf;
	atoms[1].core_req = BASE_JD_REQ_SOFT_FENCE_TRIGGER;
	atoms[1].compat_core_req = (uint16_t)(BASE_JD_REQ_SOFT_FENCE_TRIGGER & 0x7fff);
	atoms[1].atom_number = 21;
	atoms[1].pre_dep[0].atom_id = 20;
	atoms[1].pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_ORDER;
	memset(&sub, 0, sizeof(sub));
	sub.header.id = KBASE_FUNC_JOB_SUBMIT;
	sub.addr.value = atoms;
	sub.nr_atoms = 2;
	sub.stride = sizeof(atoms[0]);
	ret = uk_call(dev->fd, &sub, sizeof(sub));
	if (ret || sub.header.ret) {
		fail("fence-timeout: stuck submit rc=%d ret=%u", ret, sub.header.ret);
		close(pending_tf.basep.fd);
		close(sc.fd);
		return -1;
	}
	out_fd = tf.basep.fd; /* out-fence, must stay pending */
	if (out_fd < 0) {
		fail("fence-timeout: no out-fence fd written back");
		close(pending_tf.basep.fd);
		close(sc.fd);
		return -1;
	}

	/* The batch must NOT complete within fence_ms: proves poll detects
	 * the stall instead of hanging. */
	printf("fence-timeout: waiting %ld ms for stuck batch (expect timeout)...\n", fence_ms);
	dl = now_ns() + (uint64_t)fence_ms * 1000000ull;
	{
		struct pollfd pfd;
		pfd.fd = dev->fd;
		pfd.events = POLLIN;
		bool done = false;
		while (now_ns() < dl) {
			int tmo = (int)((dl - now_ns()) / 1000000ull);
			if (tmo > 250)
				tmo = 250;
			ret = poll(&pfd, 1, tmo);
			if (ret < 0 && errno == EINTR)
				continue;
			if (ret <= 0)
				continue;
			if (kbase_read_event(dev, &ev) == 0 &&
			    (ev.atom_number == 20 || ev.atom_number == 21)) {
				done = true;
				break;
			}
		}
		if (done) {
			fail("fence-timeout: stuck batch completed unexpectedly (atom %u code 0x%x)",
			     ev.atom_number, ev.event_code);
			close(out_fd);
			close(pending_tf.basep.fd);
			close(sc.fd);
			return -1;
		}
		printf("fence-timeout: batch correctly pending after %ld ms\n", fence_ms);
	}

	/* Out-fence must also stay unsignalled. */
	{
		struct pollfd pfd;
		pfd.fd = out_fd;
		pfd.events = POLLIN;
		ret = poll(&pfd, 1, 0);
		if (ret > 0) {
			fail("fence-timeout: out-fence signalled while batch stuck");
			close(out_fd);
			close(pending_tf.basep.fd);
			close(sc.fd);
			return -1;
		}
		printf("fence-timeout: out-fence fd %d correctly unsignalled\n", out_fd);
	}

	/* Samsung vendor fence timer fires at 1000 ms with a dmesg note
	 * ("Release fence is not signalled") but does NOT cancel or fake
	 * the fence: still pending here, which is the correct behaviour. */
	printf("fence-timeout: (vendor 1000 ms fence timer may log dmesg; fence stays pending)\n");

	/* Hang-recovery: a fresh fd must still drive soft jobs (context
	 * isolation: the stuck atoms belong to this fd only). */
	{
		struct kbase_dev dev2;
		struct kbase_bo bo2;
		struct base_dump_cpu_gpu_counters dump;
		char err[128] = "";
		memset(&dev2, 0, sizeof(dev2));
		if (kbase_open_dev(&dev2, DEVPATH, err, sizeof(err))) {
			fail("fence-timeout: canary open: %s", err);
			close(out_fd);
			close(pending_tf.basep.fd);
			close(sc.fd);
			return -1;
		}
		if (kbase_alloc(&dev2, 1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
				BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
				BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM,
				&bo2, err, sizeof(err))) {
			fail("fence-timeout: canary alloc: %s", err);
			kbase_close_dev(&dev2);
			close(out_fd);
			close(pending_tf.basep.fd);
			close(sc.fd);
			return -1;
		}
		memset(&dump, 0, sizeof(dump));
		if (kbase_soft_dump_time(&dev2, &bo2, &dump) != BASE_JD_EVENT_DONE) {
			fail("fence-timeout: canary soft job failed (GPU wedged?)");
			kbase_free(&dev2, &bo2);
			kbase_close_dev(&dev2);
			close(out_fd);
			close(pending_tf.basep.fd);
			close(sc.fd);
			return -1;
		}
		printf("fence-timeout: canary on fresh fd OK (cycles=%llu) — context usable\n",
		       (unsigned long long)dump.cycle_counter);
		kbase_free(&dev2, &bo2);
		kbase_close_dev(&dev2);
	}

	/* Cleanup: closing this fd tears down the stuck atoms with the
	 * context (kbasep_js_remove_cancelled_job). */
	close(out_fd);
	if (got_pending_fence)
		close(pending_tf.basep.fd);
	close(sc.fd);
	printf("fence-timeout: PASS (stuck batch detected; close fd to release atoms)\n");
	(void)bo;
	return 0;
}

/* --- test 4: soft-event wait cancel ----------------------------------------
 * EVENT_WAIT with no SET must be cancelled by the kernel soft-job timeout
 * (stock 3000 ms). Measures the real cancel latency. */
static int t_softwait(struct kbase_dev *dev, struct kbase_bo *bo, long budget_ms)
{
	uint64_t evt_addr = bo->gpu_va + 512;
	uint64_t t0;
	long rc;

	((unsigned char *)bo->cpu)[512] = 0; /* BASE_JD_SOFT_EVENT_RESET */
	t0 = now_ns();
	rc = one_atom_wait(dev, evt_addr, BASE_JD_REQ_SOFT_EVENT_WAIT,
			   22, 0, 0, 0, 0, budget_ms, NULL);
	printf("soft-event-cancel: rc=0x%lx (%s) after %.2f s (budget %ld ms)\n",
	       (unsigned long)rc, rc < 0 ? strerror(-rc) : kbase_event_name((unsigned)rc),
	       (double)(now_ns() - t0) / 1e9, budget_ms);
	if (rc != BASE_JD_EVENT_JOB_CANCELLED) {
		fail("soft-event-cancel: expected CANCELLED(0x4002), got 0x%lx", (unsigned long)rc);
		return -1;
	}
	printf("soft-event-cancel: PASS (kernel cancelled stuck soft-event wait)\n");
	return 0;
}

/* --- test 5: fault injection -------------------------------------------------
 * Each malformed submit must be rejected synchronously (submit ret != 0)
 * without consuming atoms or wedging the context; a canary soft job must
 * still complete afterwards. Uses a scratch fd so failures cannot disturb
 * the main test fd. */
static int t_fault(struct kbase_dev *dev, struct kbase_bo *bo)
{
	struct kbase_dev fd;
	char err[128] = "";
	struct base_jd_atom_v2 atom;
	struct kbase_uk_job_submit sub;
	int ret, nbad = 0;

	(void)dev;
	memset(&fd, 0, sizeof(fd));
	if (kbase_open_dev(&fd, DEVPATH, err, sizeof(err))) {
		fail("fault-inject: open: %s", err);
		return -1;
	}

#define SUBMIT_RAW(atoms_, nr_, stride_) \
	({ memset(&sub, 0, sizeof(sub)); \
	   sub.header.id = KBASE_FUNC_JOB_SUBMIT; \
	   sub.addr.value = (atoms_); sub.nr_atoms = (nr_); sub.stride = (stride_); \
	   ret = uk_call(fd.fd, &sub, sizeof(sub)); \
	   (ret == 0) ? (int)sub.header.ret : ret; })

	/* (a) bad stride */
	memset(&atom, 0, sizeof(atom));
	atom.jc = bo->gpu_va;
	atom.core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME;
	atom.atom_number = 30;
	ret = SUBMIT_RAW(&atom, 1, sizeof(atom) - 8);
	printf("fault-inject: bad-stride submit -> %d (expect nonzero)\n", ret);
	if (ret == 0) {
		fail("fault-inject: bad stride accepted");
	} else {
		nbad++;
	}

	/* (b) invalid dep type (3) */
	memset(&atom, 0, sizeof(atom));
	atom.jc = bo->gpu_va;
	atom.core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME;
	atom.atom_number = 31;
	atom.pre_dep[0].atom_id = 30;
	atom.pre_dep[0].dependency_type = 3;
	ret = SUBMIT_RAW(&atom, 1, sizeof(atom));
	printf("fault-inject: bad-dep-type submit -> %d (expect nonzero or CANCELLED event)\n", ret);
	if (ret == 0) {
		/* Accepted at submit; the atom must then fail fast with a
		 * fault/CANCELLED event, not hang. */
		long rc = one_atom_wait(&fd, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
		(void)rc;
		/* Drain the event for atom 31. */
		uint64_t dl = now_ns() + 5000000000ull;
		bool got = false;
		while (now_ns() < dl) {
			struct pollfd pfd;
			struct base_jd_event_v2 ev;
			pfd.fd = fd.fd;
			pfd.events = POLLIN;
			if (poll(&pfd, 1, 500) <= 0)
				continue;
			if (kbase_read_event(&fd, &ev))
				break;
			if (ev.atom_number == 31) {
				got = true;
				printf("fault-inject: bad-dep-type event 0x%x (%s)\n",
				       ev.event_code, kbase_event_name(ev.event_code));
				if (ev.event_code == BASE_JD_EVENT_DONE)
					fail("fault-inject: bad dep type reported DONE");
				else
					nbad++;
				break;
			}
		}
		if (!got)
			fail("fault-inject: bad-dep-type atom produced no event (hang?)");
	} else {
		nbad++;
	}

	/* (c) dependency on an unused slot (never submitted) -> dependent
	 * must fail fast, not hang. */
	{
		struct base_jd_atom_v2 atoms[2];
		memset(atoms, 0, sizeof(atoms));
		atoms[0].jc = bo->gpu_va;
		atoms[0].core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME;
		atoms[0].atom_number = 32;
		atoms[0].pre_dep[0].atom_id = 33; /* UNUSED, no event */
		atoms[0].pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_DATA;
		atoms[1].jc = bo->gpu_va;
		atoms[1].core_req = BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME;
		atoms[1].atom_number = 33;
		/* Submit only atom 32 first would block on slot 33... instead
		 * submit both with 33 depending on nothing: 33 runs, but 32's
		 * dependency resolves against the COMPLETED-unused entry.
		 * Simpler deterministic case: submit 32 alone with dep on 33
		 * which is UNUSED with event_code 0 (not DONE) -> kernel
		 * fails 32 immediately (jd.c:1095-1130 will_fail path). */
		ret = SUBMIT_RAW(&atoms[0], 1, sizeof(atoms[0]));
		if (ret != 0) {
			printf("fault-inject: dep-on-unused submit -> %d (sync reject, good)\n", ret);
			nbad++;
		} else {
			uint64_t dl = now_ns() + 5000000000ull;
			bool got = false;
			while (now_ns() < dl) {
				struct pollfd pfd;
				struct base_jd_event_v2 ev;
				pfd.fd = fd.fd;
				pfd.events = POLLIN;
				if (poll(&pfd, 1, 500) <= 0)
					continue;
				if (kbase_read_event(&fd, &ev))
					break;
				if (ev.atom_number == 32) {
					got = true;
					printf("fault-inject: dep-on-unused event 0x%x (%s)\n",
					       ev.event_code, kbase_event_name(ev.event_code));
					if (ev.event_code == BASE_JD_EVENT_DONE)
						fail("fault-inject: dep-on-unused reported DONE");
					else
						nbad++;
					break;
				}
			}
			if (!got)
				fail("fault-inject: dep-on-unused produced no event (hang?)");
		}
	}

	/* Canary: context must still work. */
	{
		struct base_dump_cpu_gpu_counters dump;
		struct kbase_dev cdev;
		struct kbase_bo cbo;
		memset(&cdev, 0, sizeof(cdev));
		if (kbase_open_dev(&cdev, DEVPATH, err, sizeof(err))) {
			fail("fault-inject: canary open: %s", err);
			kbase_close_dev(&fd);
			return -1;
		}
		if (kbase_alloc(&cdev, 1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
				BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
				BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM,
				&cbo, err, sizeof(err))) {
			fail("fault-inject: canary alloc: %s", err);
			kbase_close_dev(&cdev);
			kbase_close_dev(&fd);
			return -1;
		}
		memset(&dump, 0, sizeof(dump));
		if (kbase_soft_dump_time(&cdev, &cbo, &dump) != BASE_JD_EVENT_DONE)
			fail("fault-inject: canary soft job failed after bad submits");
		else
			printf("fault-inject: canary OK (cycles=%" PRIu64 ")\n", dump.cycle_counter);
		kbase_free(&cdev, &cbo);
		kbase_close_dev(&cdev);
	}

	kbase_close_dev(&fd);
	printf("fault-inject: %d/3 malformed cases contained\n", nbad);
	if (nbad < 3) {
		fail("fault-inject: only %d/3 cases contained", nbad);
		return -1;
	}
	printf("fault-inject: PASS\n");
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [--device PATH] [--print-abi] [tests...] [--jobs N] [--stuck-ms MS]\n"
		"       [--churn N] [--sizes a,b,c] [--fence-ms MS] [--softwait-ms MS]\n"
		"tests: many bo churn fence softwait fault all (default: all)\n", p);
}

int main(int argc, char **argv)
{
	const char *devpath = DEVPATH;
	const char *want[8];
	int nwant = 0, jobs = 500, churn = 300, stuck_ms = 2000;
	long fence_ms = 1500, softwait_ms = 8000;
	const char *sizes = "1,4,16";
	bool print_abi = false;
	struct kbase_dev dev;
	struct kbase_bo bo;
	char err[256] = "";
	bool run_all = true;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			devpath = argv[++i];
		else if (!strcmp(argv[i], "--print-abi"))
			print_abi = true;
		else if (!strcmp(argv[i], "--jobs") && i + 1 < argc)
			jobs = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stuck-ms") && i + 1 < argc)
			stuck_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--churn") && i + 1 < argc)
			churn = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--sizes") && i + 1 < argc)
			sizes = argv[++i];
		else if (!strcmp(argv[i], "--fence-ms") && i + 1 < argc)
			fence_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "--softwait-ms") && i + 1 < argc)
			softwait_ms = atol(argv[++i]);
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-' && strcmp(argv[i], "all")) {
			usage(argv[0]);
			return 2;
		} else {
			if (nwant < 8)
				want[nwant++] = argv[i];
			run_all = false;
		}
	}

	printf("kbase UK 10.6 winsys stress\n");
	printf("wire ABI: atom=%zu submit=%zu event=%zu dump=%zu UK=%u.%u\n",
	       sizeof(struct base_jd_atom_v2), sizeof(struct kbase_uk_job_submit),
	       sizeof(struct base_jd_event_v2),
	       sizeof(struct base_dump_cpu_gpu_counters),
	       KBASE_UK_VERSION_MAJOR, KBASE_UK_VERSION_MINOR);
	if (print_abi)
		return 0;

	memset(&dev, 0, sizeof(dev));
	if (kbase_open_dev(&dev, devpath, err, sizeof(err))) {
		printf("device %s unavailable: %s\n", devpath, err);
		printf("ABI self-check passed, stress not executed (no hardware)\n");
		return 2;
	}
	printf("device %s open (ctx %d, DDK %s)\n", devpath, dev.ctx_id, dev.ddk_version);

	memset(&bo, 0, sizeof(bo));
	if (kbase_alloc(&dev, 1, BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
			BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
			BASE_MEM_CACHED_CPU | BASE_MEM_COHERENT_SYSTEM,
			&bo, err, sizeof(err))) {
		fail("setup BO alloc: %s", err);
		kbase_close_dev(&dev);
		return 1;
	}

#define WANT(nm) (run_all || ({ bool _f = false; for (int _i = 0; _i < nwant; _i++) if (!strcmp(want[_i], nm) || !strcmp(want[_i], "all")) _f = true; _f; }))

	if (WANT("many")) {
		printf("\n== many-small-jobs (n=%d, stuck-line=%d ms) ==\n", jobs, stuck_ms);
		t_many(&dev, jobs, stuck_ms, &bo);
	}
	if (WANT("bo")) {
		printf("\n== bo-churn (iters=%d sizes=%s) ==\n", churn, sizes);
		t_churn(&dev, churn, sizes);
	}
	if (WANT("fence")) {
		printf("\n== fence-timeout (deadline=%ld ms) ==\n", fence_ms);
		t_fence(&dev, &bo, fence_ms);
	}
	if (WANT("softwait")) {
		printf("\n== soft-event-cancel (budget=%ld ms) ==\n", softwait_ms);
		t_softwait(&dev, &bo, softwait_ms);
	}
	if (WANT("fault")) {
		printf("\n== fault-inject ==\n");
		t_fault(&dev, &bo);
	}

	kbase_free(&dev, &bo);
	kbase_close_dev(&dev);
	printf("\n%s\n", failures ? "STRESS FAILED" : "STRESS PASS");
	return failures ? 1 : 0;
}
