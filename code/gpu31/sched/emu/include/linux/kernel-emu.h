/* SPDX-License-Identifier: GPL-2.0 */
/* Host emulation of the remaining kernel primitives the shims need. */
#ifndef _SCHED_EMU_KERN_H
#define _SCHED_EMU_KERN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/time.h>

typedef uint32_t u32;
typedef uint64_t u64;

/* ---- types ---- */
typedef int gfp_t;
#define GFP_KERNEL 0
#define GFP_ATOMIC 0
#define GFP_NOWAIT 0
#define __GFP_NOWARN 0

typedef long ktime_t;
static inline ktime_t ktime_get(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (ktime_t)tv.tv_sec * 1000000L + tv.tv_usec;
}
static inline ktime_t ktime_set(long s, long ns)
{
	(void)ns;
	return s * 1000000L;
}

/* ---- bug/warn ---- */
extern int sched_emu_bug_fired;
extern int sched_emu_warn_count;
/* BUG_ON must work in void functions: flag it, then trap via abort so
 * a missed contract fails the test loudly instead of returning. The
 * dma_fence_init NULL-release test below avoids BUG_ON by asserting
 * the contract directly (see test_fence_init). */
#include <assert.h>
#define BUG_ON(c) do { \
	if (c) { sched_emu_bug_fired = 1; assert(!(c)); } \
} while (0)
#define WARN_ON(c) ({ \
	int __w = !!(c); \
	if (__w) sched_emu_warn_count++; \
	__w; \
})


typedef struct { atomic_int counter; } atomic_t;
#define atomic_set(v, i) atomic_store(&(v)->counter, (i))
#define atomic_read(v) atomic_load(&(v)->counter)
#define atomic_inc(v) (void)atomic_fetch_add(&(v)->counter, 1)
#define atomic_inc_return(v) (atomic_fetch_add(&(v)->counter, 1) + 1)
#define atomic_dec_and_test(v) (atomic_fetch_sub(&(v)->counter, 1) == 1)

static inline int test_bit(int nr, const volatile unsigned long *addr)
{
	return !!(*addr & (1UL << nr));
}
static inline int test_and_set_bit(int nr, volatile unsigned long *addr)
{
	unsigned long mask = 1UL << nr;
	unsigned long old = *addr;

	*addr = old | mask;
	return !!(old & mask);
}

/* ---- kref ---- */
struct kref { atomic_int refcount; };
static inline void kref_init(struct kref *kref)
{
	atomic_init(&kref->refcount, 1);
}
static inline void kref_get(struct kref *kref)
{
	atomic_fetch_add(&kref->refcount, 1);
}
static inline int kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
	if (atomic_fetch_sub(&kref->refcount, 1) == 1) {
		release(kref);
		return 1;
	}
	return 0;
}

/* ---- spinlock/mutex (single-threaded: no-ops with held tracking) ---- */
typedef struct { int locked; } spinlock_t;
#define DEFINE_SPINLOCK(x) spinlock_t x = { 0 }
static inline void spin_lock_init(spinlock_t *l) { l->locked = 0; }
static inline void spin_lock(spinlock_t *l) { (void)l; }
static inline void spin_unlock(spinlock_t *l) { (void)l; }
#define spin_lock_irqsave(l, f) do { (void)(f); } while (0)
#define spin_unlock_irqrestore(l, f) do { } while (0)
#define lockdep_assert_held(l) do { } while (0)

struct mutex { int locked; };
static inline void mutex_init(struct mutex *m) { m->locked = 0; }
static inline void mutex_lock(struct mutex *m) { m->locked = 1; }
static inline void mutex_unlock(struct mutex *m) { m->locked = 0; }
static inline int mutex_lock_interruptible(struct mutex *m)
{
	m->locked = 1;
	return 0;
}

/* ---- completion (manual: test drives signaling) ---- */
struct completion { int done; };
static inline void init_completion(struct completion *c) { c->done = 0; }
#define INIT_COMPLETION(c) ((c).done = 0)
static inline void complete(struct completion *c) { c->done = 1; }
static inline void complete_all(struct completion *c) { c->done = 1; }
static inline int completion_done(struct completion *c) { return c->done; }
static inline unsigned long wait_for_completion_timeout(struct completion *c,
							unsigned long t)
{
	if (c->done)
		return t ? t : 1;
	return 0;
}
static inline unsigned long wait_for_completion_interruptible_timeout(
		struct completion *c, unsigned long t)
{
	return wait_for_completion_timeout(c, t);
}

/* ---- jiffies ---- */
#define HZ 100
#define MAX_SCHEDULE_TIMEOUT ((signed long)(~0UL >> 1))
static inline unsigned long msecs_to_jiffies(unsigned int m)
{
	return (unsigned long)m * HZ / 1000;
}

/* ---- waitqueue (unused stub: signal path only completes) ---- */
typedef struct { int dummy; } wait_queue_head_t;
static inline void init_waitqueue_head(wait_queue_head_t *q) { (void)q; }
static inline void wake_up_all(wait_queue_head_t *q) { (void)q; }

/* ---- workqueue (synchronous inline dispatch for determinism) ---- */
struct workqueue_struct { int id; };
struct work_struct {
	void (*fn)(struct work_struct *work);
	int pending;
};
struct delayed_work {
	struct work_struct work;
	unsigned long delay;
	int armed;
};
static inline void INIT_WORK(struct work_struct *w,
			     void (*fn)(struct work_struct *))
{
	w->fn = fn;
	w->pending = 0;
}
static inline void INIT_DELAYED_WORK(struct delayed_work *w,
				     void (*fn)(struct work_struct *))
{
	w->work.fn = fn;
	w->work.pending = 0;
	w->delay = 0;
	w->armed = 0;
}
static inline int queue_work(struct workqueue_struct *wq,
			     struct work_struct *w)
{
	(void)wq;
	w->pending = 1;
	return 0;
}
static inline int queue_delayed_work(struct workqueue_struct *wq,
				     struct delayed_work *w,
				     unsigned long delay)
{
	(void)wq;
	w->armed = 1;
	w->delay = delay;
	w->work.pending = 1;
	return 0;
}
/* mod_delayed_work(): re-arm an already-pending timer or queue a new
 * one, at the caller's delay. Host emu: remember whether the work was
 * already armed (kernel return contract) and mark it pending. */
static inline bool mod_delayed_work(struct workqueue_struct *wq,
				    struct delayed_work *w,
				    unsigned long delay)
{
	int was_armed = w->armed;

	(void)wq;
	w->armed = 1;
	w->delay = delay;
	w->work.pending = 1;
	return was_armed ? true : false;
}
static inline void cancel_delayed_work_sync(struct delayed_work *w)
{
	w->armed = 0;
	w->work.pending = 0;
}
static inline void cancel_work_sync(struct work_struct *w)
{
	w->pending = 0;
}
static inline void flush_workqueue(struct workqueue_struct *wq) { (void)wq; }
static inline void destroy_workqueue(struct workqueue_struct *wq) { (void)wq; }
static inline struct workqueue_struct *emu_alloc_wq(void)
{
	static struct workqueue_struct wq;

	return &wq;
}
#define alloc_ordered_workqueue(fmt, flags, args...) (emu_alloc_wq())

/* ---- idr (tiny open-addressing table for the syncobj test) ---- */
#define SCHED_EMU_IDR_MAX 512
struct idr {
	void *ptrs[SCHED_EMU_IDR_MAX];
	int next;
};
static inline void idr_init(struct idr *idp)
{
	memset(idp->ptrs, 0, sizeof(idp->ptrs));
	idp->next = 1;
}
static inline void idr_preload(gfp_t gfp) { (void)gfp; }
static inline void idr_preload_end(void) { }
static inline int idr_alloc(struct idr *idp, void *ptr, int start, int end,
			    gfp_t gfp)
{
	int i;

	(void)end;
	(void)gfp;
	if (start < 1)
		start = 1;
	for (i = start; i < SCHED_EMU_IDR_MAX; i++) {
		if (!idp->ptrs[i]) {
			idp->ptrs[i] = ptr;
			return i;
		}
	}
	return -1;
}
static inline void *idr_find(struct idr *idp, int id)
{
	if (id < 0 || id >= SCHED_EMU_IDR_MAX)
		return NULL;
	return idp->ptrs[id];
}
static inline void *idr_get_next(struct idr *idp, int *nextid)
{
	int id;

	for (id = *nextid; id < SCHED_EMU_IDR_MAX; id++) {
		if (idp->ptrs[id]) {
			*nextid = id;
			return idp->ptrs[id];
		}
	}
	return NULL;
}
static inline void idr_remove(struct idr *idp, int id)
{
	if (id >= 0 && id < SCHED_EMU_IDR_MAX)
		idp->ptrs[id] = NULL;
}
static inline void idr_destroy(struct idr *idp)
{
	memset(idp->ptrs, 0, sizeof(idp->ptrs));
}

/* ---- slab ---- */
static inline void *kmalloc(size_t s, gfp_t g)
{
	(void)g;
	return malloc(s ? s : 1);
}
static inline void *kzalloc(size_t s, gfp_t g)
{
	(void)g;
	return calloc(1, s ? s : 1);
}
static inline void *kcalloc(size_t n, size_t s, gfp_t g)
{
	(void)g;
	return calloc(n ? n : 1, s ? s : 1);
}
#define kfree(p) free(p)

/* ---- misc kernel ---- */
#define EXPORT_SYMBOL(sym)
#define MODULE_LICENSE(x)
#define IS_ERR(p) ((unsigned long)(p) > (unsigned long)-1000UL)
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(long)(e))
#define ENODATA 61
#define EOPNOTSUPP 95
#define ENOMEM 12
#define EINVAL 22
#define ENOENT 2
#define EBUSY 16
#define ETIME 62
#define ERESTARTSYS 512
#define ENODEV 19
#define ENOSYS 38
#define EAGAIN 11
#define ENOSPC 28
#define EEXIST 17
#define EFAULT 14
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define upper_32_bits(x) ((unsigned)((x) >> 32))

typedef unsigned long uintptr_kernel_t;

#endif /* _SCHED_EMU_KERN_H */
