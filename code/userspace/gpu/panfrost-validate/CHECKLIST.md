# CHECKLIST — on-device GPU register / MMU bring-up (Mali-T720, Exynos 7580)

For the integrator only. Every command below runs on the phone (root shell
over ssh). This agent ran NO device commands; all expected values were
derived from the read-only kbase tree and the host-side checker in this
directory (`validate.c`, 60 checks, PASS natively and under
`qemu-aarch64`). Do NOT run these until the M0/M1 module is flashed.

Prerequisite: `panfrost7580` (or successor) probed with `force=1` if kbase
still owns the registers, GPU powered (clocks + `vdd_g3d` + pd-g3d),
debugfs mounted (`mount -t debugfs none /sys/kernel/debug`).

## 1. Read the four feature registers

```sh
cat /sys/kernel/debug/panfrost7580/features
```

Expected decode (checker `test_regs` + `test_id_decode`):

| register (offset) | expected raw | decode |
|---|---|---|
| `GPU_ID` (0x000) | `0x0720rRpS` (product `0x0720`; e.g. r1p3 = `0x07201301`) | product field `[31:16]` MUST be `0x0720` (T720). major=`[15:12]`, minor=`[11:4]`, status=`[3:0]` |
| `MMU_FEATURES` (0x014) | `0x00002820` on a 32b-VA/40b-PA part | VA bits = `[7:0]` = 32 (or 40); PA bits = `[15:8]` = 40. Any other PA width changes the PTE ADDR mask |
| `AS_PRESENT` (0x018) | nonzero bitmask | popcount = number of address spaces (expect `0x0f`..`0xff`-class, i.e. 4–8 AS) |
| `JS_PRESENT` (0x01C) | nonzero bitmask | popcount = job slots (expect `0x07` = 3 slots on Midgard) |

If `GPU_ID` product != `0x0720`, STOP: wrong IP block, do not submit jobs.
If `JS_PRESENT` == 0, STOP: no schedulable slots, scheduler bring-up is moot.

## 2. IOVA window implications

- IOVA width = VA bits from `MMU_FEATURES[7:0]`.
- The LPAE walker is 4 levels x 9 bits (`validate.c: test_lpae`):
  index(level) = `(iova >> 12) >> ((3-level)*9) & 0x1FF`.
  A 32-bit VA only needs levels 1–3; level 0 stays zero — that is expected,
  not a bug (`mali_lpae_test.c: test_index_math`).
- Size the level-0 table and the allocator's IOVA range from the VA-bits
  value just read; a part reporting 40 VA bits needs the full 4-level tree.
- PA width 40 ⇒ leaf PTE address mask `0xFFFFFFFFFF000`; bits ≥ 40 in a PA
  MUST be zero or the walker faults (checker asserts the mask drops them).

## 3. MEMATTR divergence (kbase vs mainline — pick one before M1)

- kbase LPAE policy (`mali_midg_regmap.h:506-531`,
  `mali_kbase_mmu_mode_lpae.c:76-87`): packed `0x8D884D4F48`
  (idx0=`0x48` impl-def cache policy, idx1=`0x4F` force-cache-all,
  idx2=`0x4D` write-alloc, idx3=`0x88` outer impl-def, idx4=`0x8D` outer WA).
- mainline `io-pgtable-arm` ARM_MALI_LPAE: packed `0x888D88`
  (idx0=`0x88` NC, idx1=`0x8D` CACHE, idx2=`0x88` DEV) — idx0/idx2 differ.
- The checker flags (not resolves) this (`test_as_config` prints the
  divergence note and FAILs if it ever vanishes).
- On device: read back `AS_MEMATTR_LO/HI` (0x2408/0x240C + `n*0x40`) for the
  AS the test uses and confirm whichever policy M1 chose was actually
  programmed. Wrong memattr ⇒ silent data corruption, not a clean fault.

## 4. TRANSTAB / TRANSCFG programming check

- `TRANSTAB = pgd_pa | ADRMODE_TABLE(3) | READ_INNER(1<<2)`
  (`+ SHARE_OUTER(1<<4)` only if the system is ACE-coherent; Exynos 7580
  has no coherency property ⇒ kbase runs COHERENCY_NONE ⇒ bit 4 CLEAR).
  Expected for `pgd_pa=0x40000000`: `0x40000007` (checker `test_as_config`).
- `TRANSCFG_LO` = `ADRMODE_LEGACY(0)` for the LPAE path; `UNMAPPED(1)` parks
  an AS. After `AS_COMMAND_UPDATE(0x01)`, the AS is live.

## 5. Fault-injection first signal (before any job submission)

```sh
# 1. Program AS0 with TRANSCFG=UNMAPPED (or an empty pgd), then issue UPDATE.
# 2. Trigger one GPU memory access through that AS.
cat /sys/kernel/debug/panfrost7580/selftest   # in-kernel LPAE encode+walk test
dmesg | tail -30
```

- First expected signal of a working fault path: `GPU_FAULT_STATUS`
  (0x03C) nonzero and/or `AS_FAULTSTATUS` (0x241C+n*0x40) with exception
  code `TRANSTAB_BUS_FAULT (0x2<<3)`, plus `AS_FAULTADDR_LO/HI`
  (0x2420/0x2424+n*0x40) holding the faulting IOVA.
- The `selftest` debugfs file must report PASS (same vectors as the host
  checker: leaf `0x800006C5` for PA `0x80000000` + RW attrs, index vectors
  above). If selftest fails on device while the host checker passes, the
  register window or clocks/power are wrong — do not proceed to jobs.
- No fault interrupt at all after a deliberately bad access ⇒ IRQ routing
  (`INT_MASK`/`INT_RAWSTAT` 0x2028/0x2000-class path) or power is broken.

## 6. Hand-off to M2

M2 (render-node bind) is unblocked when: `GPU_ID` product `0x0720`,
`JS_PRESENT` nonzero, `MMU_FEATURES` VA/PA widths recorded, `selftest`
PASS, and one injected fault produces the `AS_FAULTSTATUS`/`FAULTADDR`
signature above.
