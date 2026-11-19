# GPU31 MMU slice — completion report

Date: 2026-11-19. Slice: `G31_Mmu`. Phone state per Main: UP (#35, SSH/RNDIS
ok); no reboot/flash performed (Integrator/M1Verify own those); one
read-only locked SSH check done earlier (sysfs read, no `/dev/dri` node
expected pre-M1). Shared kernel tree never modified (read-only `-c`/kbuild
scratch checks only).

## Deliverables (all in repo)

- `code/userspace/gpu/panfrost/mali_hw.h` — MMU block completed: FAULTEXTRA
  LO/HI, MMU IRQ pairing (`PAGE_FAULT(n)`/`BUS_ERROR(n)`), TRANSCFG
  identity/mask + AARCH64 PTW fields (named-but-zero on LPAE), LOCK width
  limits 11/63, full fault-status decode (exception byte/access/DEC/source
  ID), LPAE exception classes 0xC0/0xC8/0xD0/0xD8, AS_ACTIVE poll ceiling.
- `code/userspace/gpu/panfrost/mali_mmu.h` — NEW freestanding AS-lifecycle +
  invalidate + fault host model: `mali_as_count/select_ok`, 33-bit VA window
  (`mali_va_limit/in_range`, `mali_l0_entries`), saturating
  `mali_lock_region`, `mali_as_transtab/transtab_disabled/memattr_kbase`,
  fault class/access/decoder/source/IRQ-pairing/VA decode. Every sequence
  cites kbase r15p0 LPAE (COHERENCY_NONE) and panfrost v5.2 file:line.
- `code/userspace/gpu/panfrost/mali_lpae_test.c` + `Makefile` — 114 M0
  checks + 71 new (§§8–12: AS select, 33-bit window incl. builder
  end-to-end at 0x1FFFFF000/0x200000000, LOCK vectors, AS values, fault
  decode); deps gain `mali_mmu.h`.
- `code/gpu31/mmu/pf7580_as.h` + `code/gpu31/mmu/README` — NEW data-free
  kernel static-inline helper: `pf7580_as_enable/disable` (TRANSTAB/MEMATTR/
  TRANSCFG + wmb + UPDATE, wait !ACTIVE both sides), `pf7580_as_invalidate`
  (LOCK + FLUSH_PT/MEM + wait), saturating `pf7580_lock_region`, IRQ +
  fault decode. No 9630 double-UNLOCK: absent from the T720 r1p0 errata
  list. Packaged as `code/gpu31/mmu/0001-pf7580-as-helper.patch` (new file
  only under `drivers/gpu/drm/panfrost/`; integrator applies).
- `docs/reports/gpu31-mmu.md` — this report (acceptance pointer).

## Verification (all pass)

- `cd code/userspace/gpu/panfrost && make check`: **185 checks, 0 failures**,
  native gcc + qemu-aarch64 (`mali_lpae_test.aarch64`).
- Kernel header: kbuild `M=<scratch> modules` with
  `CROSS_COMPILE=aarch64-linux-gnu-` against the shared 3.10.108 tree —
  stage-2 link OK (plain-object harness exercising enable/invalidate/
  disable; module-macro `cold`-attribute noise under gcc 16 was harness
  artifact, not this header; scratch removed).
- Three self-found expectation bugs fixed during development (L0-is-always-0
  below 2^40 — L1 spans 0..7 at 33 bits; fls(4)=3 width-13 exact-pow2 rule;
  64-bit LOCK saturation at width 63) — model matched kbase/panfrost in all
  three; tests corrected, not the sequences.

## Review disposition (gpu31-review.md @ 625ed18)

No finding applies to this slice: the review names §§1–4 (fence/resv/
syncobj/sched), §6 display driver, §7 DT, §8 config, §10 iommu-dma PRIME,
§11 r7p0 stride, §12 Mesa n_slots — zero mentions of `mali_hw.h`,
`mali_mmu.h`, `mali_lpae_test.c`, `pf7580_as.h`, or `code/gpu31/mmu/`
(verified by grep; the only "mmu" hit is §10's title word "iommu-dma",
which is G31_DmaBuf's PRIME patch, not this slice). No patch change
required; state recorded here explicitly per Main's instruction.

Boundary holds: `G31_Iommu` owns `code/gpu31/iommu/panfrost7580_mmu.*`
(`p7580_mmu_*`/`P7580_*`); this slice is `mali_*` (host) +
`pf7580_as_*`/`PF7580_*` (kernel). `G31_PanfrostTarget` confirmed no
collision in `code/userspace/gpu/panfrost/`.

## Key semantics for the integrator/driver author

Enable AS n: TRANSTAB_LO/HI ← pgd | TABLE(3) | READ_INNER(4) (never
SHARE_OUTER here), MEMATTR ← 0x884D4F48/0x00008D88 (kbase LPAE), TRANSCFG
← 0, wmb, COMMAND ← UPDATE gated on !AS_ACTIVE before+after. Map/unmap:
sync table pages (COHERENCY_NONE PTW), LOCK(iova,size) + FLUSH_PT + wait.
Full TLB: LOCK(0,~0) + FLUSH_MEM + wait. Fault path: mask MMU IRQs, per
set AS read FAULTADDR_HI/LO + FAULTSTATUS, clear via MMU_INT_CLEAR,
re-enable; AS_FAULTSTATUS low byte class, bits 9:8 access (ATOMIC→UNKNOWN
on LPAE), bit 10 decoder/slave, bits 31:16 source ID; 33-bit VA window
[0, 2^33), L0[0] only.
