/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pf7580_fault.h - fault taxonomy + host fault-decode helpers for the
 * Mali-T720 panfrost port (Exynos 7580, j7elte).
 *
 * This header is the contract between the driver fault path
 * (code/gpu31/jobs/panfrost7580_job.c: IRQ demux, timedout, recover) and
 * the host fault-injection matrix (fault_matrix.c on pf7580_mock_gpu.c).
 *
 * Register truth is NOT duplicated here: job-manager offsets, IRQ demux
 * bits, JS_STATUS names, slot routing, JS_CONFIG and both timeouts come
 * straight from code/gpu31/jobs/panfrost7580_jm_hw.h (pure-C host branch).
 * Only the MMU AS_FAULTSTATUS decode is mirrored locally, because the
 * kernel MMU headers need linux kernel headers and cannot compile on
 * the host (provenance below; cross-checked at runtime in F12).
 *
 * MMU decode provenance: single LPAE decoder (F-MMU4 fix), implemented
 * identically in four places — p7580_mmu_fault_code/atype
 * (iommu/panfrost7580_mmu.c), pf7580_fault_exc/access
 * (mmu/pf7580_as.h:253-282), mali_fault_exc/access_name (userspace
 * mali_mmu.h), and pf7580_fault_mmu/lpae_* below: low byte 0xC0-0xC7
 * translation / 0xC8 permission / 0xD0-0xD7 transtab bus / 0xD8 access
 * flag; bits 9:8 access (EXECUTE/READ/WRITE, ATOMIC decodes UNKNOWN on
 * this LPAE build). The old short-code view (bits 5:3, ADDRESS_SIZE /
 * MEMORY_ATTRIBUTES classes) is retired — it mislabelled silicon
 * faults. */

#ifndef PF7580_FAULT_H
#define PF7580_FAULT_H

#include <stdint.h>

/* JM hardware truth shared with the driver (slots, IRQ bits, JS_STATUS
 * table, JS_CONFIG, 5 s job timeout, 3 s reset bound). */
#include "panfrost7580_jm_hw.h"

/* ------------------------------------------------------------------ */
/* Matrix case IDs (F1..F12, see fault_matrix.c and gpu31-fault.md).   */
/* ------------------------------------------------------------------ */

#define PF7580_FAULT_NCASES	12u

/* BO/IOMMU permission bits: same values as P7580_PROT_* in
 * iommu/panfrost7580_mmu.h:115-118 (kept local so this stays host-C). */
#define PF7580_F_PROT_RD	(1u << 0)
#define PF7580_F_PROT_WR	(1u << 1)
#define PF7580_F_PROT_XN	(1u << 2)

/* Reset kind reported by the recovery model. */
enum pf7580_reset_kind {
	PF7580_RESET_NONE = 0,
	PF7580_RESET_SOFT = 1,
	PF7580_RESET_HARD = 2,
};

/* MMU exception classes, single LPAE decoder (F-MMU4; the retired
 * short-code mask is gone). */
static inline const char *pf7580_fault_mmu_code(uint32_t status)
{
	uint32_t e = status & 0xFFu;

	if (e >= 0xC0u && e <= 0xC7u)
		return "TRANSLATION_FAULT";
	if (e == 0xC8u)
		return "PERMISSION_FAULT";
	if (e >= 0xD0u && e <= 0xD7u)
		return "TRANSTAB_BUS_FAULT";
	if (e == 0xD8u)
		return "ACCESS_FLAG";
	return "UNKNOWN";
}

/* MMU access types (bits 9:8; ATOMIC names UNKNOWN on LPAE). */
static inline const char *pf7580_fault_mmu_atype(uint32_t status)
{
	switch (status & (0x3u << 8)) {
	case (0x1u << 8):
		return "EXECUTE";
	case (0x2u << 8):
		return "READ";
	case (0x3u << 8):
		return "WRITE";
	default:
		return "UNKNOWN";
	}
}

/* LPAE exception byte view (pf7580_as.h:256-268). */
static inline const char *pf7580_fault_lpae_exc(uint32_t st)
{
	uint32_t e = st & 0xFFu;

	if (e >= 0xC0u && e <= 0xC7u)
		return "TRANSLATION_FAULT";
	if (e == 0xC8u)
		return "PERMISSION_FAULT";
	if (e >= 0xD0u && e <= 0xD7u)
		return "TRANSTAB_BUS_FAULT";
	if (e == 0xD8u)
		return "ACCESS_FLAG";
	return "UNKNOWN";
}

/* LPAE access view (pf7580_as.h:270-282): ATOMIC names UNKNOWN. */
static inline const char *pf7580_fault_lpae_access(uint32_t st)
{
	switch (st & (0x3u << 8)) {
	case (0x1u << 8):
		return "EXECUTE";
	case (0x2u << 8):
		return "READ";
	case (0x3u << 8):
		return "WRITE";
	default:
		return "UNKNOWN";
	}
}

#endif /* PF7580_FAULT_H */
