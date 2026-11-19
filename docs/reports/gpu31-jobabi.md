# GPU31 job ABI — descriptor layout, core requirements, tiler heap, caches, MMU tables

Date: 2026-11-19. Scope: `code/gpu31/jobs`, `code/gpu31/mmu`, Mali M0 model
(`code/userspace/gpu/panfrost/`). No tree edits, no flash, no device writes
for this report; all values below are read from the shared-tree kbase r15p0
sources (read-only) and the in-repo M0/host model that encodes them.

Ticket asks for: the exact job descriptor layout used (type/flags/addresses,
core requirements, tiler heap, caches) and the MMU table format the driver
programs — one table per field, with evidence from the M0 model/kbase.

## 0. Sources of truth (evidence key)

| Tag | File | What it proves |
|---|---|---|
| `[KT-defs]` | shared tree `drivers/gpu/arm/t7xx/r15p0/mali_kbase_defs.h:1682-1712` | `KBASE_AS_INACTIVE_MAX_LOOPS`, `BASEP_JD_REPLAY_LIMIT 15`, `struct job_descriptor_header` ("identical for all GPU archs") |
| `[KT-base]` | same tree `mali_base_kernel.h:440-720,1742-1810` | `BASE_JD_REQ_*` bits, soft-job subtypes, `ATOM_TYPE` mask, `tiler_heap_free`/`tiler_core_req` replay payload |
| `[KT-reg]` | same tree `mali_midg_regmap.h:199-418,535-543` | JOB/MMUS register map, `JS_CONFIG`/`JS_COMMAND`/`AS_*` values, `JS_FEATURE_*`, MEMATTR indices, fault-code masks |
| `[KT-replay]` | same tree `mali_kbase_replay.c:29-64,110-200` | `JOB_TYPE_*` numerics, `fragment_job` layout, FBD offsets, tiler-heap reset shape |
| `[KT-lpae]` | same tree `mali_kbase_mmu_mode_lpae.c:25-41,76-189` | LPAE entry bits, MEMATTR table build, TRANSTAB build, `entry_set_ate/pte/invalidate` |
| `[M0-hw]` | repo `code/userspace/gpu/panfrost/mali_hw.h` | register map + LPAE/PTE/MEMATTR/TRANSTAB/TRANSCFG/fault constants both drivers agree on; 185-check host suite input |
| `[M0-mmu]` | repo `code/userspace/gpu/panfrost/mali_mmu.h` | AS lifecycle + `mali_lock_region` + fault-decode host model |
| `[M0-lpae]` | repo `code/userspace/gpu/panfrost/mali_lpae.h` + `mali_lpae_test.c` | 4-level builder (`map/walk/unmap`), index math, descriptor vectors (`make check`: 185/185 native+qemu) |
| `[J-hw]` | repo `code/gpu31/jobs/panfrost7580_jm_hw.h` | T720 JM programming: slot regs, IRQ demux, `p7580_js_config()`, fault table, 32-B `p7580_job_desc`, v5.2 `p7580_submit` |
| `[J-test]` | repo `code/gpu31/jobs/job_test.c` | 60 host checks pinning regs/IRQ/routing/cfg/faults/sizes |
| `[MMU-k]` | repo `code/gpu31/iommu/panfrost7580_mmu.{c,h}` | kernel walker (alloc/map/unmap), `program_as`, `invalidate`, fault names |
| `[MMU-as]` | repo `code/gpu31/mmu/pf7580_as.h` | AS enable/disable/invalidate inline sequences, `pf7580_lock_region`, IRQ pairing |
| `[UAPI]` | repo `code/gpu31/gem/panfrost_drm.h` | byte-identical v5.2 UAPI (`PANFROST_JD_REQ_FS` only, 6 ioctls) |

Live silicon anchor (read-only, `gpu31-jobs.md` §7 / `gpu31-kbase-abi.md` §0):
`Mali-T72x MP2 r1p0 0x0720`, `js_present 0x7` (3 slots), 2 shader cores,
`mmu_features 0x2821` (VA 33), coherency `0x1f` = NONE.

## 1. Job descriptor header — 32 bytes, common prefix of every job

`[KT-defs]` 1689-1712; `[J-hw]` `struct p7580_job_desc`; `[J-test]` asserts
`sizeof == 32`. The driver programs `jc` (GPU VA of this struct) into
`JS_HEAD_NEXT_LO/HI`; the MMU slice validates the mapping before HW reads it.

| Off | Size | Field | Meaning / values | Evidence |
|---|---|---|---|---|
| 0 | u32 | `exception_status` | Written by HW on fault: low byte exception code, bits 9:8 access type, bits 31:16 source ID (same packing as `AS_FAULTSTATUS`; `[KT-replay]` `dump_job_head` prints exactly this split). `0` = `JOB_NOT_STARTED` | `[KT-defs]`, `[KT-replay]:29,71-104` |
| 4 | u32 | `first_incomplete_task` | Index of first task that did not complete; fault pointer companion | `[KT-defs]` |
| 8 | u64 | `fault_pointer` | GPU VA latched at fault (shader/tiler PC or data address, type-dependent) | `[KT-defs]` |
| 16 | u8 bit 0 | `job_descriptor_size` | 0 = 32-bit `next_job` (`_32`), 1 = 64-bit (`_64`). `[KT-replay]` `dump_job_head` branches on it | `[KT-defs]`, `[KT-replay]:100-104` |
| 16 | u8 bits 6:0 (7 bits) | `job_type` | Payload selector — see §2 table | `[KT-defs]`, `[KT-replay]:30-34` |
| 17 | u8 bit 0 | `job_barrier` | 1 = barrier job: still counted in `job_index`/deps but serialises the chain | `[KT-defs]` |
| 17 | u8 bits 7:1 | `_reserved_01/_1/_02/_03/_2/_04/_05` | Must be zero; 7 reserved bits packing byte 17 with `job_barrier` | `[KT-defs]` |
| 18 | u16 | `job_index` | Chain sequence number; deps refer to it. Max `JOB_HEADER_ID_MAX 0xffff` | `[KT-defs]`, `[KT-replay]:51` |
| 20 | u16 | `job_dependency_index_1` | First dependency (`job_index` of predecessor) | `[KT-defs]` |
| 22 | u16 | `job_dependency_index_2` | Second dependency | `[KT-defs]` |
| 24 | u64/u32 | `next_job` (`_64`/`_32`) | GPU VA of next descriptor in chain; NULL terminates. Width selected by `job_descriptor_size` | `[KT-defs]`, `[J-hw]:217-221` |

Total: 4+4+8+2 (bitfield bytes) +2+2+2+8 = 32. `[J-test]` checks
`sizeof == 8+8+2+6+8`.

## 2. Job types (`job_type`, 7 bits)

Numerics from `[KT-replay]:29-34` (only grounded values; gaps are reserved):

| Value | Name | Slot / requirement | Payload after the 32-B header (grounded) |
|---|---|---|---|
| 0 | (not-started sentinel) | `exception_status == JOB_NOT_STARTED`, not a type | — |
| 1 | `JOB_TYPE_NULL` | Any (`JS_FEATURE_NULL_JOB 1<<1`) | No payload; completes immediately. IRQ-throttle note: NULL jobs can complete inside the IRQ handler (`KBASE_JS_MAX_JOB_SUBMIT_PER_SLOT_PER_IRQ 2`) |
| 5 | `JOB_TYPE_VERTEX` | Vertex/tiler slot; `BASE_JD_REQ_CS` | Vertex payload. Replay rule: on failure the header is rewritten to `JOB_TYPE_NULL` (`[KT-replay]:399-400`) |
| 7 | `JOB_TYPE_TILER` | Vertex/tiler slot; `BASE_JD_REQ_T` | Tiler payload; references the FBD (§5). Replay recurses into `*job_header` + hierarchy mask (`[KT-replay]:289-347`) |
| 8 | `JOB_TYPE_FUSED` | Vertex/tiler slot | Fused vertex+tiler; replay resets both sides (`[KT-replay]:402`) |
| 9 | `JOB_TYPE_FRAGMENT` | Fragment slot; `BASE_JD_REQ_FS` | `struct fragment_job`: `u32 x[2]` then `fragment_fbd` (`u64` if 64-bit else `u32`) — `[KT-replay]:57-64`. FBD offset §5 |

Slot capability advertisement (`JS<n>_FEATURES`, `[KT-reg]:535-543`):

| Bit | Feature | Meaning |
|---|---|---|
| 1 | `NULL_JOB` | NULL job supported |
| 2 | `SET_VALUE_JOB` | Set-value job supported |
| 3 | `CACHE_FLUSH_JOB` | Cache-flush job supported |
| 4 | `COMPUTE_JOB` | Compute-shader job supported |
| 5 | `VERTEX_JOB` | Vertex job supported |
| 6 | `GEOMETRY_JOB` | Geometry job supported |
| 7 | `TILER_JOB` | Tiler job supported |
| 8 | `FUSED_JOB` | Fused job supported |
| 9 | `FRAGMENT_JOB` | Fragment job supported |

T720 note: the port only ever emits types the v5.2 UAPI can address
(vertex/tiler chain on JS1, fragment chain on JS0); JS2/compute is wired but
idle (`gpu31-jobs.md` §1).

## 3. Core requirements (`core_req` / `requirements`)

Full kbase vocabulary (`[KT-base]:462-720`); the panfrost wire carries a
1-bit subset (`[UAPI]`, `[J-hw]:111-123`).

| Bit | kbase name | HW/SW | Meaning |
|---|---|---|---|
| 0 | `BASE_JD_REQ_FS` | HW | Fragment shaders → fragment slot. **The only bit on the v5.2 UAPI wire** (`PANFROST_JD_REQ_FS`) |
| 1 | `BASE_JD_REQ_CS` | HW | Compute/vertex/geometry shader job → vertex/tiler or compute slot |
| 2 | `BASE_JD_REQ_T` | HW | Tiling work → vertex/tiler slot |
| 3 | `BASE_JD_REQ_CF` | HW | Requires cache flushes |
| 4 | `BASE_JD_REQ_V` | HW | Requires value writeback |
| 5 | `EVENT_COALESCE` | SW | No completion event for this atom |
| 6 | `COHERENT_GROUP` | SW | Needs any coherent core group |
| 7 | `PERMON` | SW | Perf counters needed (power hint) |
| 8 | `EXTERNAL_RESOURCES` | SW | Atom references external resources (no bundled syncsets) |
| 9 | `SOFT_JOB` | SW | Not submitted to HW; low 5 bits select soft subtype (dump-time `0x1`, fence-trigger `0x2`, fence-wait `0x3`, replay `0x4`, event `0x5-0x7`, debug-copy `0x8`, JIT alloc/free `0x9/0xa`, ext-res map/unmap `0xb/0xc`) |
| 10 | `ONLY_COMPUTE` | HW | Pure compute-shader chain → compute slot. Kernel-internal at v5.2 (`P7580_JD_REQ_ONLY_COMPUTE`), never on the UAPI wire |
| 11 | `SPECIFIC_COHERENT_GROUP` | SW/HW | `device_nr` selects the core group (compute only) |
| 12 | `EVENT_ONLY_ON_FAILURE` | SW | Event only on failure |
| 13 | `FS_AFBC` | SW | Fragment job with AFBC encoding |
| 14 | `EVENT_NEVER` (`BASEP_`) | SW | Never emit completion event |
| 15 | `SKIP_CACHE_START` | SW | Skip GPU cache clean/invalidate at job start |
| 16 | `SKIP_CACHE_END` | SW | Skip GPU cache clean/invalidate at job end |

`BASE_JD_REQ_ATOM_TYPE` masks the type-control bits (`FS|CS|T|CF|V|SOFT|
ONLY_COMPUTE(+coalesce/coherent/permon/external…)`, `[KT-base]:692-707`).
Routing actually programmed (`[J-hw]:116-123`, v5.2 `panfrost_job_get_slot`):
`FS → JS0`, `ONLY_COMPUTE → JS2`, else `JS1`. Validation (`gpu31-jobs.md`
§3): `jc` nonzero + 8-aligned, `requirements ⊆ {FS[, ONLY_COMPUTE
internal]}`, slot present in `js_present`.

## 4. Submit programming (what the driver writes per submit)

Slot `n` base = `0x1800 + n*0x80` (`[KT-reg]` `JOB_SLOT_REG` ==
`[M0-hw]` `JS_BASE`; `[J-test]` pins `0x1800/0x1880/0x1900`).

| Register (offset in slot) | Absolute (slot 0) | Value programmed |
|---|---|---|
| `JS_HEAD_NEXT_LO/HI` `0x40/0x44` | `0x1840/0x1844` | `jc` low/high |
| `JS_AFFINITY_NEXT_LO/HI` `0x50/0x54` | `0x1850/0x1854` | full `shader_present` mask (live: 2 cores) |
| `JS_CONFIG_NEXT` `0x58` | `0x1858` | `p7580_js_config(as_nr)` — see cache table §6 |
| `JS_COMMAND_NEXT` `0x60` | `0x1860` | `START (0x01)`; guard `COMMAND_NEXT != 0 → WARN_ON` + recovery |
| `JS_FLUSH_ID_NEXT` `0x70` | `0x1870` | **not programmed** (no FLUSH_REDUCTION on T720) |

Read-back: `JS_HEAD/TAIL_LO/HI` (`0x00-0x0C`), `JS_COMMAND` (`0x20`),
`JS_STATUS` (`0x24`, §7). IRQs: `JOB_INT_{RAWSTAT,CLEAR,MASK,STAT}` =
`0x1000/0x1004/0x1008/0x100C`; demux `DONE(j)=1<<j`, `ERR(j)=1<<(j+16)`,
`MK_JS_MASK(j)=0x10001<<j` (`[J-hw]:66-68`, `[J-test]` vectors).

## 5. Tiler heap + FBD (framebuffer descriptor)

The tiler heap is a growable GPU allocation the tiler spills polygon lists
into; the FBD points at it and carries a `heap_free` cursor the HW bumps.
On replay the driver resets the cursor and hierarchy weights
(`[KT-replay]:110-270`).

| Item | Layout / value | Evidence |
|---|---|---|
| `tiler_heap_free` (replay payload) | `u64` GPU address written back into the FBD | `[KT-base]:1745-1747,1780` (`base_jd_replay_payload`, UK10_2 variant identical) |
| FBD tiler cursor | `heap_free_address` u64 inside the FBD tiler section | `[KT-replay]:115-119,200-207` (SFBD and MFBD variants) |
| 32-bit job FBD pointer | at header + `31*4` = +124 | `[KT-replay]:37` `JOB_HEADER_32_FBD_OFFSET` |
| 64-bit job FBD pointer | at header + `44*4` = +176 | `[KT-replay]:38` `JOB_HEADER_64_FBD_OFFSET` |
| SFBD tiler section | FBD + `48*4` = +192: `u32 pad, flags; u64 pad[2]; u64 heap_free; u32 pad[8]; u32 weights[8]` | `[KT-replay]:41,111-119` |
| MFBD tiler section | FBD + `14*4` = +56: `u32 pad, flags; u64 pad[2]; u64 heap_free; u64 pad; u32 weights[8]` | `[KT-replay]:43,193-200` |
| FBD pointer alignment | `FBD_POINTER_MASK ~0x3f` (64-B aligned) | `[KT-replay]:40` |
| FBD type / hierarchy | `FBD_TYPE 1`; `FBD_HIERARCHY_WEIGHTS 8`, mask `0x1fff`, full `HIERARCHY_WEIGHTS 13` | `[KT-replay]:45-49` |
| Heap size policy | 128 MiB tiler heap + sample-position BOs per context (Mesa backend) | `gpu-mesa-kbase.md` §`panfrost_device_init` |
| Failure semantics | `OUT_OF_MEMORY (JS_STATUS 0x60)` when the heap exhausts; replay re-runs with reset cursor | `[KT-reg]` fault table; `[KT-replay]` reset path |

## 6. Caches (JS_CONFIG + MEMATTR + coherency discipline)

T720 quirk basis: `base_hw_features_t72x` (features.h:72-83) has **no**
`FLUSH_REDUCTION / JOBCHAIN_DISAMBIGUATION / PROTECTED_MODE`; `t72x` issue
list (issues.h:566-580) **has 10649** → `START_MMU` always.

| `JS_CONFIG_NEXT` field | Bits | T720 value | Evidence |
|---|---|---|---|
| `as_nr` | 3:0 | context AS number | `[KT]` `kbase_job_hw_submit:102`, `[J-hw]:135-142` |
| `START_FLUSH_CLEAN_INVALIDATE` | 9:8 = `3` | always set | `[KT-reg]:409`, `[J-hw]`, `[J-test]` |
| `START_MMU` | 10 | always set (10649) | `[KT-reg]`, issues.h:571, `[J-hw]` |
| `JOB_CHAIN_FLAG` | 11 | 0 (no disambiguation HW) | `[KT-reg]`, `[J-test]` asserts clear |
| `END_FLUSH_CLEAN_INVALIDATE` | 13:12 = `3` | always set | `[KT-reg]:413`, `[J-test]` |
| `ENABLE_FLUSH_REDUCTION` | 14 | 0 (feature absent) | `[J-test]` asserts clear |
| `DISABLE_DESC_WR_BK` | 15 | 0 (no protected mode) | `[J-test]` asserts clear |
| `THREAD_PRI(8)` | 23:16 | 8 | `[KT-reg]:418`, `[J-hw]:141` |

`JS_COMMAND` (`[KT-reg]:385-393`): `NOP 0x00 / START 0x01 / SOFT_STOP 0x02 /
HARD_STOP 0x03 / SOFT_STOP_0 0x04 / HARD_STOP_0 0x05 / SOFT_STOP_1 0x06 /
HARD_STOP_1 0x07`, mask `0x07`. `GPU_CMD` (`[KT-reg]`, `[J-hw]:88-93`):
`SOFT_RESET 0x01 / HARD_RESET 0x02 / CLEAN_CACHES 0x07 /
CLEAN_INV_CACHES 0x08`; `RESET_COMPLETED = 1<<8`.

MEMATTR (LPAE indices, `[KT-reg]:506-531`, `[KT-lpae]:76-87`):

| Index | kbase LPAE byte | panfrost byte | Role |
|---|---|---|---|
| 0 | `0x48` impl-def cache | `0x88` impl-def | default region policy (`AS_MEMATTR_INDEX_DEFAULT 0`) |
| 1 | `0x4F` force-cache-all | `0x8D` write-alloc | kbase force-cache / panfrost cache |
| 2 | `0x4D` write-alloc | `0x88` device | kbase write-alloc |
| 3 | `0x88` outer impl-def | — | outer coherent impl-def (ACE only) |
| 4 | `0x8D` outer WA | — | outer coherent WA (ACE only) |

Programmed pair on this unit: `MEMATTR_LO 0x884D4F48 / HI 0x00008D88`
(`[MMU-as]:95-96`, `[MMU-k]:89-90`), i.e. u64 `0x8D884D4F48`; mainline
alternative `0x888D88` kept defined for A/B comparison. Coherency:
`COHERENCY_NONE` (DT has no `system-coherency`; live `0x1f`) → never set
`SHARE_OUTER`, PTW non-coherent → `dma_sync_single_for_device` after every
table write (`[KT-lpae]` sync discipline, `[MMU-k]:50-54,176-179`).

## 7. `JS_STATUS` fault decode (slot health)

`[KT-reg]:418-451`; `[J-hw]:147-193`; `[J-test]` name/is-fault vectors.
`status >= 0x40` = fault → recovery path; below = benign.

| Code | Name | Class |
|---|---|---|
| 0x02 | INTERRUPTED | benign |
| 0x03 | STOPPED | benign |
| 0x04 | TERMINATED | benign |
| 0x40 | CONFIG_FAULT | fault |
| 0x41 | POWER_FAULT | fault |
| 0x42 | READ_FAULT | fault |
| 0x43 | WRITE_FAULT | fault |
| 0x44 | AFFINITY_FAULT | fault |
| 0x48 | BUS_FAULT | fault |
| 0x50-0x56 | INSTR_INVALID_PC/ENC, TYPE_MISMATCH, OPERAND/TLS/BARRIER/ALIGN_FAULT | fault |
| 0x58 | DATA_INVALID_FAULT | fault |
| 0x59 | TILE_RANGE_FAULT | fault |
| 0x5A | ADDRESS_RANGE_FAULT | fault (unmapped `jc` injection vector) |
| 0x60 | OUT_OF_MEMORY | fault (tiler heap exhaust) |
| 0x7F | UNKNOWN | fault |

## 8. MMU table format (what the driver programs)

4 levels × 512 entries × 9 bits, 4 KiB granule (`[KT-lpae]:25-35`,
`[M0-hw]:249-252`, `[MMU-k]:26-28`). VA 33 bits on this unit
(`mmu_features 0x2821` → `[M0-hw]` `MALI_MMU_VA_BITS`), PA 40 bits
(`MMU_HW_OUTA_BITS 40`). Index: `vpfn >> ((3-level)*9) & 0x1FF`
(`[M0-lpae]` `mali_pte_index`, `[MMU-k]:34-38`, M0 test §1 vectors).

### 8.1 Entry encoding (one u64 per entry)

| Bits | Field | Values | Evidence |
|---|---|---|---|
| 1:0 | type | `1` ATE/block (leaf), `2` INVAL, `3` PTE/table | `[KT-lpae]:25-35`, `[M0-hw]:254-257`, `[MMU-k]:31-34` |
| 39:12 | output address | 40-bit OAS masked (`~0xFFF` within 40 bits); bits above 40 never leak (`[M0-lpae]` test asserts) | `[KT-reg]` `MMU_HW_OUTA_BITS`, `[M0-hw]:261-262` |
| 4:2 | memattr index | 0..7 (`memattr_idx ≤ 7` enforced); `KBASE_REG_MEMATTR_VALUE << 2` | `[KT-lpae]` `get_mmu_flags`, `[MMU-k]:204-205` |
| 6 | RD | `ENTRY_RD_BIT / PTE_RD_BIT` | `[KT-lpae]`, `[M0-hw]:265`, `[MMU-k]:37` |
| 7 | WR | `ENTRY_WR_BIT / PTE_WR_BIT` | same |
| 9:8 | share | `0` non-shareable; `2<<8` both/inner+outer (`SHARE_BOTH`), `3<<8` inner (`SHARE_INNER`); T720 leaf default OS (`[M0-hw]` `MALI_ATTR_GPU_*`) | `[KT-lpae]` `get_mmu_flags`, `[KT-defs]` share enum, `[M0-hw]:267-269` |
| 10 | AF (access flag) | always set on mapped leaf (`ENTRY_ACCESS_BIT`) | `[KT-lpae]` `entry_set_ate`, `[MMU-k]:206` |
| 54 | NX | `ENTRY_NX_BIT`; set iff `PROT_XN` | `[KT-lpae]`, `[MMU-k]:212-213` |

Ready-made attribute sets (`[M0-hw]:286-292`): `GPU_RO = RD|AF|SH_OS|idx(CACHE)`,
`GPU_RW = RO|WR`, `GPU_RW_NX = RW|XN`. Grounding vector (`[M0-lpae]` test §2):
`leaf(0x80000000, GPU_RW) == 0x800006C5`. Fresh pages are pre-filled `INVAL
(0x2)` (`[MMU-k]:90-93,152-155`).

### 8.2 AS registers (per address space)

MMU base `0x2000`, AS0 `0x400`, stride `0x40` (`[KT-reg]:255-280` ==
`[M0-hw]:110-120` `MMU_BASE 0x2400`); AS register offsets identical both sides:

| Off | Register | Value programmed |
|---|---|---|
| 0x00/0x04 | `TRANSTAB_LO/HI` | `pgd & 0xFFFFF000 \| TABLE(3) \| READ_INNER(1<<2)`; never `SHARE_OUTER` (COHERENCY_NONE). HI = `pgd >> 32` |
| 0x08/0x0C | `MEMATTR_LO/HI` | `0x884D4F48 / 0x00008D88` (kbase LPAE) |
| 0x10/0x14 | `LOCKADDR_LO/HI` | `mali_lock_region(iova,size)` — §8.3 |
| 0x18 | `COMMAND` | `UPDATE 0x01 / LOCK 0x02 / UNLOCK 0x03 / FLUSH_PT 0x04 / FLUSH_MEM 0x05` |
| 0x1C | `FAULTSTATUS` | decode §8.4 (RO) |
| 0x20/0x24 | `FAULTADDR_LO/HI` | fault VA (RO) |
| 0x28 | `STATUS` | `AS_ACTIVE 0x01`; poll `!ACTIVE`, bound 100 000 (`[KT-defs]:1682`) |
| 0x30/0x34 | `TRANSCFG_LO/HI` | `0` (LEGACY; LPAE build) |
| 0x38/0x3C | `FAULTEXTRA_LO/HI` | secondary fault address (RO) |

Enable = TRANSTAB → MEMATTR → TRANSCFG(0) → `wmb` → `UPDATE`, gated on
`!ACTIVE` before+after (`[MMU-as]:175-197`, `[MMU-k]:251-270`). Disable =
TRANSTAB 0 (`ADRMODE_UNMAPPED`) + MEMATTR 0 + `UPDATE`. Map/unmap = write
PTEs + `dma_sync` + `LOCK(iova,size)` + `FLUSH_PT` + wait; full TLB =
`LOCK(0,~0)` + `FLUSH_MEM` + wait (`[MMU-as]:216-234`,
`[MMU-k]:285-311`). No 9630 double-UNLOCK (absent from T720 r1p0 errata).

### 8.3 LOCK region encoding

| Item | Rule | Evidence |
|---|---|---|
| Base | `iova & ~0xFFF` | `[M0-mmu]:129-166`, `[MMU-as]:107-141` |
| Width field bits 5:0 | `10 + fls(pages)`, rounded up to pow2 page count; exact-pow2 uses exact width | `[M0-mmu]`, M0 LOCK vectors §11 |
| Clamp | `[11, 63]`; saturates at 63 above 2^53 B (covers `LOCK(0,~0UL)` full-TLB) | `[M0-hw]:182-183`, `[MMU-as]:99-100` |
| Bound | `LOCK_MIN_W 11 / LOCK_MAX_W 63` | same |

### 8.4 MMU fault decode

| Bits | Field | Values | Evidence |
|---|---|---|---|
| 7:0 | exception class (LPAE) | `0xC0-0xC7` translation, `0xC8` permission, `0xD0-0xD7` transtab-bus, `0xD8` access-flag | `[M0-hw]:207-210`, `[MMU-as]:253-268` |
| 5:3 | kbase coarse code | `0<<3` translation, `1<<3` permission, `2<<3` transtab-bus, `3<<3` access-flag, `4<<3` address-size, `5<<3` mem-attr | `[KT-reg]:341-353`, `[MMU-k]:313-331` |
| 9:8 | access type | `0` ATOMIC (named UNKNOWN on LPAE), `1` EXECUTE, `2` READ, `3` WRITE | `[KT-reg]`, `[M0-hw]:193-198`, `[MMU-k]:333-347` |
| 10 | decoder/slave | `1` decoder, `0` slave | `[M0-hw]:193`, `[MMU-as]:233-235` |
| 31:16 | source ID | faulting unit ID | `[M0-hw]:187`, `[MMU-as]:239-242` |
| VA | `FAULTADDR_HI:LO` | 64-bit fault VA | `[M0-mmu]:259-263` |

IRQ pairing: bit `i` = page fault in AS `i`, bit `i+16` = bus error in AS
`i`; a slot reporting both decodes as bus error (`[KT]` `kbase_mmu_interrupt`,
`[M0-hw]:142-144`, `[M0-mmu]:244-257`, `[MMU-as]:238-251`).

## 9. Verification (exercised, not claimed)

- `code/userspace/gpu/panfrost && make check`: **185 checks, 0 failures**,
  native + qemu-aarch64 (114 M0 + 71 §§8-12: AS select, 33-bit window incl.
  builder end-to-end at `0x1FFFFF000/0x200000000`, LOCK vectors, AS values,
  fault decode).
- `code/gpu31/jobs && make check`: **60 checks PASS** (regs, IRQ bits, slot
  routing, `JS_CONFIG` construction + T720 exclusions, 6 fault names +
  fault-vs-benign split, timeouts, 32-B descriptor + SUBMIT sizes, caps).
- Kernel: `pf7580_as.h` kbuild `-c` vs real 3.10.108 headers OK;
  `panfrost7580_job.o` 16 232 B, 0 errors (`gpu31-jobs.md` §7).
- NOT verified: no Image link, no HW submit, no fault injection — needs
  M1 (`/dev/dri`) + GEM + MMU + sched landed first (first-submit recipe in
  `gpu31-jobs.md` §7).
