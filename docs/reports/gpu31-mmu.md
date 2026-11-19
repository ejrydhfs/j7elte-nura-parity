# GPU31 MMU slice — F-MMU1..5 fix wave (review ebfc1f5)

Date: 2026-11-19. Slice: `G31_MmuFix`. Source of defects:
`docs/reports/gpu31-driver-review.md` (commit ebfc1f5, reviewer
G31_DriverReview2) §F-MMU1 + §F-MMU2/3/4/5.

Phone state per Main: UP (#37). No flash, no device access used by this
slice (host work only; verification Main's job). Shared kernel tree
never edited (host `-fsyntax-only` against stub headers + host
`make check` only).

## What the review found

| ID | Severity | Defect |
|---|---|---|
| F-MMU1 | blocker | `p7580_mmu_invalidate` wrote `vpfn << PAGE_SHIFT` raw into LOCKADDR, ignoring the width field. Page-aligned addresses have low bits zero → minimum width always → multi-page maps left stale TLB entries. |
| F-MMU2 | required | `p7580_mmu_program_as` has the `wmb()` its twin carries, but the invalidate path programmed LOCKADDR then LOCK with no barrier. |
| F-MMU3 | required | `p7580_as_wait_ready` (iommu twin) was a tight 100k `readl` loop — no `cpu_relax`/`udelay`; the `pf7580_as.h` twin polls 10 µs-stepped. |
| F-MMU4 | required | Fault decoders disagreed: `iommu/...:313-330` mapped `0x2<<3`→TRANSTAB_BUS / `0x3<<3`→ACCESS_FLAG (plus ADDRESS_SIZE / MEMORY_ATTRIBUTES classes), while `mmu/pf7580_as.h:256-268` maps `0xD0-0xD7`→TRANSTAB_BUS / `0xD8`→ACCESS_FLAG with different access-type words. One mislabels silicon faults. |
| F-MMU5 | required | No context-teardown walker existed. `p7580_mmu_free` freed the root only; every process exit leaked page tables. |

## Fixes (all in repo)

- `code/gpu31/iommu/panfrost7580_mmu.c`:
  - New `p7580_lock_region()` — byte-identical twin of
    `pf7580_lock_region()` (`mmu/pf7580_as.h`) and `mali_lock_region()`
    (userspace `mali_mmu.h`): page-aligned base + pow2-rounded count in
    bits 5:0, saturating at width 63. `p7580_mmu_invalidate` programs
    `p7580_lock_region(vpfn<<12, nr<<12)` (F-MMU1).
  - `wmb()` after LOCKADDR HI/LO, before LOCK (F-MMU2).
  - `p7580_as_wait_ready` stepped: `udelay(P7580_AS_WAIT_STEP_US)` per
    iteration (F-MMU3), twin of `pf7580_as_wait_ready`.
  - Fault decoders reconciled to the single LPAE view (F-MMU4):
    `0xC0-0xC7` translation / `0xC8` permission / `0xD0-0xD7` transtab
    bus / `0xD8` access flag; bits 9:8 EXECUTE/READ/WRITE, ATOMIC→UNKNOWN.
  - New `p7580_mmu_teardown()` + static `p7580_teardown_level()`
    (F-MMU5): recursive walk, clear-parent-first (no dangling pointers),
    leaves cleared only (BO pages stay GEM's), every non-root table
    freed. Wire to file-close/remove, then `p7580_mmu_free()` for root.
- `code/gpu31/iommu/panfrost7580_mmu.h`: `P7580_LOCK_MIN_W/MAX_W` (11/63),
  `P7580_AS_WAIT_STEP_US` (10), `P7580_FAULT_CODE_MASK 0xFF` (was
  `0x7<<3`), `p7580_mmu_teardown()` declaration, invalidate contract
  rewritten (width + barrier + teardown pointer).
- `code/gpu31/iommu/0001-panfrost7580-mmu-helper.patch`: regenerated from
  the fixed `.c/.h` (integrator applies a current patch).
- `code/gpu31/mmu/pf7580_as.h`: canonical-decoder note (F-MMU4); the
  twin itself was already the correct LPAE view — unchanged behaviour.
- `code/gpu31/gem/panfrost7580_gem.{h,c}`: contract comments point at
  `p7580_mmu_teardown()` (F-MMU5) instead of the old
  "caller frees child tables by hand" wording. No code change (GEM slice
  owns the logic; confirmed with G31_Gem3 before editing).
- `code/gpu31/fault/pf7580_fault.h`, `pf7580_mock_gpu.h`,
  `fault_matrix.c`, `docs/reports/gpu31-fault.md`: short-code decoder
  retired (F-MMU4); all four views byte-identical; F11 asserts the
  retired classes decode UNKNOWN. 12/12 PASS native.
- `code/userspace/gpu/panfrost/mali_lpae_test.c`: §§13–15 added —
  F-MMU1 LOCK-width monotonicity + exact densities + nr=0 guard,
  F-MMU2/3 barrier/poll contract values, F-MMU5 teardown emptiness
  (map-3/unmap-3 returns every table); §12 extended with gap vectors
  (0xC9/0xCF/0xD9 UNKNOWN) and single-decoder mask assertion.
  **227 checks, 0 failures, native + qemu-aarch64.**

## Single decoder (F-MMU4 verdict)

KT `mali_midg_regmap.h:341-353` names coarse buckets; the LPAE build's
fault-name table (`pf7580_as.h` view) is what silicon reports on this
T720, and it is also what the jobs slice's fault path consumes. The
short-code bits-5:3 naming carried two classes (ADDRESS_SIZE,
MEMORY_ATTRIBUTES) that do not exist on this block — that view is
retired everywhere (iommu + fault mirror + mock constants + F11). All
four decode sites (`p7580_mmu_fault_*`, `pf7580_fault_exc/access`,
`mali_fault_exc/access_name`, `pf7580_fault_mmu/lpae_*`) are now
byte-identical.

## Verification (exercised)

```
cd code/userspace/gpu/panfrost && make check   # 227/227 native + qemu
cd code/gpu31/fault && make check              # 12/12 native
gcc -fsyntax-only -I/tmp/mmustub -Icode/gpu31/iommu \
  code/gpu31/iommu/panfrost7580_mmu.c          # SYNTAX_OK (stub linux/*)
```

NOT verified (Main's job): kbuild `-c` against the real 3.10.108 tree,
device MMU fault injection, teardown under process-exit load.

## Boundary / coordination

- GEM contract-comment edits confirmed with G31_Gem3 (owner) before
  touching; no GEM logic changed.
- Fault-slice mirror (`pf7580_fault.h`, `pf7580_mock_gpu.h`,
  `fault_matrix.c`, `gpu31-fault.md`) is shared with the jobs slice's
  decoder: F11 now pins the single view so drift fails loudly.
- `code/gpu31/driver/0002-panfrost7580-build-wiring.patch` showed an
  uncommitted peer diff mid-task — reverted untouched (DriverAssemble
  owns it).
- Test binaries (`mali_lpae_test`, `.aarch64`) rebuilt by `make check`
  are NOT committed (tracked files left untouched).

## Integrator notes

1. Apply `code/gpu31/iommu/0001-panfrost7580-mmu-helper.patch`.
2. `driver/` land must wire `p7580_mmu_teardown()` to file-close/remove
   (F-MMU5), per the header contract: AS unbound
   (TRANSTAB 0 + UPDATE, wait !ACTIVE) → teardown under the MMU/table
   lock → `p7580_mmu_free()` for the root.
3. SUBMIT map path: `p7580_mmu_invalidate(regs, as, vpfn, nr, 0)` now
   encodes the whole range — callers pass the real `nr`, not 1.
