/*
 * gem_validate.c - host unit test for the panfrost GEM offset/size validation.
 *
 * Exercises the pure-C surface of panfrost7580_gem.h without kernel headers:
 * IOVA window geometry, CREATE_BO/CMA/SHMEM rounding + rejection rules
 * (incl. the 8/16 MiB SHMEM tiler-heap regression from gpu31-gem-shmem.md),
 * WAIT_BO absolute->relative conversion, and the MMU map-loop helper
 * contract (npages/phys walk). Mirrors the kernel .c logic line-for-line;
 * the kernel build is covered by `make kcheck`/`make kobj` in the Makefile.
 *
 * Build: cc -Wall -Wextra -o gem_validate gem_validate.c && ./gem_validate
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---- mirror of panfrost7580_gem.h constants (host copy) ---- */
#define P7580_BO_CMA		0u
#define P7580_BO_SHMEM		1u
#define P7580_IOVA_START	(32u << 20)
#define P7580_IOVA_SIZE		((4ull << 30) - P7580_IOVA_START)
#define P7580_GEM_WC		(1u << 0)
#define P7580_GEM_CACHABLE	(1u << 1)
#define P7580_VMAP_MAX		8u
#define P7580_PAGE_SHIFT	12u
#define P7580_PAGE_SIZE		(1u << P7580_PAGE_SHIFT)

static int fails = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

/* round_up(size, PAGE_SIZE) as the kernel .c does */
static unsigned long round_page(unsigned long size)
{
	return (size + P7580_PAGE_SIZE - 1ul) & ~(P7580_PAGE_SIZE - 1ul);
}

/* create validation mirror: 0 ok, -22 EINVAL */
static int create_ok(unsigned long size, unsigned int backing)
{
	if (!size)
		return -22;
	size = round_page(size);
	if (size == 0)
		return -22;	/* overflow wrap guard */
	if (backing != P7580_BO_CMA && backing != P7580_BO_SHMEM)
		return -22;
	return 0;
}

/* IOVA range check mirror: [iova, iova+size) inside the window */
static int iova_ok(unsigned long iova, unsigned long size)
{
	if (iova == 0)
		return -22;	/* HW treats 0 as unmapped */
	if (!(iova >= P7580_IOVA_START))
		return -28;	/* -ENOSPC */
	if (size > P7580_IOVA_SIZE)
		return -28;
	if (iova - P7580_IOVA_START > P7580_IOVA_SIZE - size)
		return -28;
	return 0;
}

static long wait_rel(long long timeout_ns, long long now_ns)
{
	long long remain = timeout_ns - now_ns;

	if (remain <= 0)
		return 0;
	/* Saturate like the kernel LONG_MAX arm (64-bit host long). */
	if ((unsigned long long)remain > (unsigned long long)0x7fffffffffffffffLL)
		return 0x7fffffffffffffffL;
	/* HZ=100 jiffy approximation is enough for the host check */
	return (long)(remain / 10000000LL);
}

int main(void)
{
	unsigned long i;

	/* IOVA window geometry */
	CHECK(P7580_IOVA_START == 0x2000000ul);
	CHECK(P7580_IOVA_SIZE == 0xFE000000ul);
	CHECK(P7580_IOVA_START + P7580_IOVA_SIZE == 0x100000000ull);

	/* size validation: zero rejected, rounding, backing selector */
	CHECK(create_ok(0, P7580_BO_CMA) == -22);
	CHECK(create_ok(1, P7580_BO_CMA) == 0);
	CHECK(round_page(1) == 4096ul);
	CHECK(round_page(4096) == 4096ul);
	CHECK(round_page(4097) == 8192ul);
	CHECK(create_ok(4096, 7u) == -22);
	CHECK(create_ok(1u << 20, P7580_BO_SHMEM) == 0);
	/* tiler-heap regression (gpu31-gem-shmem): the sizes that failed
	 * under CMA dma_alloc_attrs/swiotlb must validate as SHMEM --
	 * page count, not one contiguous extent */
	CHECK(create_ok(8u << 20, P7580_BO_SHMEM) == 0);
	CHECK(create_ok(16u << 20, P7580_BO_SHMEM) == 0);
	CHECK((round_page(16u << 20) >> P7580_PAGE_SHIFT) == 4096ul);
	/* IOVA placement: window edges, overflow, zero */
	CHECK(iova_ok(P7580_IOVA_START, 4096ul) == 0);
	CHECK(iova_ok(P7580_IOVA_START, P7580_IOVA_SIZE) == 0);
	CHECK(iova_ok(P7580_IOVA_START + 1, P7580_IOVA_SIZE) == -28);
	CHECK(iova_ok(0, 4096ul) != 0);
	CHECK(iova_ok(P7580_IOVA_START - 4096ul, 4096ul) != 0);
	CHECK(iova_ok(0xFFFFFFFFul, 4096ul) == -28);

	/* npages walk: every BO covers size>>12 pages, last byte inside */
	for (i = 1; i <= 4; i++) {
		unsigned long sz = round_page(i * 1000ul);
		unsigned long np = sz >> P7580_PAGE_SHIFT;

		CHECK(np * P7580_PAGE_SIZE == sz);
		CHECK(np >= 1);
	}

	/* WAIT_BO: absolute stamp -> relative budget */
	CHECK(wait_rel(1000000000LL, 999000000LL) == 0);	/* 1ms -> 0 jiffies */
	CHECK(wait_rel(900000000LL, 999000000LL) == 0);	/* past -> 0 */
	CHECK(wait_rel(0LL, 999000000LL) == 0);		/* zero -> 0 */
	CHECK(wait_rel(11000000000LL, 1000000000LL) == 1000); /* 10s -> 1000 */

	/* cache attrs are distinct bits; WC default path is neither */
	CHECK((P7580_GEM_WC & P7580_GEM_CACHABLE) == 0);
	CHECK(P7580_VMAP_MAX == 8u);

	if (fails == 0)
		printf("gem_validate: all checks passed\n");
	else
		printf("gem_validate: %d FAILURE(S)\n", fails);
	return fails ? 1 : 0;
}
