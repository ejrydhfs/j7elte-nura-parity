# GPU31 kbase UK 10.6 ABI — winsys ioctl set, validated on live /dev/mali0

Date: 2026-11-19. Device: SM-J700M (j7elte), Nura Alpine aarch64.
Kernel: `3.10.108 #35 SMP PREEMPT` (gcc 16.2.0, built Wed Nov 18 23:36 EST).
**Phone UP; prior "link down / kernel #33" notes are stale.** `/dev/mali0`
present (`Mali-T72x MP2 r1p0 0x0720`); `/dev/dri` absent. All device
commands ran under host `flock /tmp/gpsdev.lock` with short holds; no
reboot; no kernel build/flash (Integrator/M1Verify own those).

Scope: the exact kbase UK 10.6 ioctl ABI that
`code/userspace/gpu/kbase-winsys` speaks — every function id, every struct
size/offset, what each field means — validated three ways: (1) measured
from the shared-tree r15p0 headers with a host extractor, (2) static-asserted
in `kbase_uapi.h`, (3) exercised against the running `/dev/mali0` tonight.

ABI sources of truth (shared tree, read-only):
`drivers/gpu/arm/t7xx/r15p0/mali_uk.h` (transport/header),
`…/mali_kbase_uku.h` (function ids + call structs),
`…/mali_base_kernel.h` (atoms, events, fences, flags, props types),
`…/mali_kbase_gpuprops_types.h` + `mali_kbase_platform_uku.h` (props),
`…/mali_kbase_core_linux.c` (`kbase_ioctl`/`kbase_dispatch`/handshake),
`…/mali_kbase_jd.c` (submit stride), `…/mali_kbase_mem_linux.c` (mmap).
DDK release: `r15p0-00rel0` (`t7xx/r15p0/Kbuild:18`), UK 10.6
(`mali_kbase_uku.h:69-70`).

## 0. Validation summary (tonight, kernel #35)

| Check | Result |
|---|---|
| Host extractor vs `kbase_uapi.h` | **MATCH**: kernel-header `kbase-layout.c` output == `kbase-selftest --print-sizes` field-for-field (all 13 structs, 6 offsets, UK 10.6, `KBASE_FUNC_MAX` 569, `CALL_MAX_SIZE` 568) |
| Winsys prints (`kbw-selftest --print-abi`, aarch64 build) | uk 10.6, atom 64, event 24, mem_alloc 56, job_submit 40, dump 64 — consistent |
| Props layout (`kbw-props-layout` vs kernel headers) | `mali_base_gpu_props` 528; all 7 member offsets and all raw/coherency/thread/l2/tiler offsets match winsys `_Static_assert`s |
| Live `/dev/mali0` transport | **PASS**: `kbase-probe` rc 0 — handshake 10.6→10.6 ret 0, `SET_FLAGS(0)` ret 0, `GET_VERSION` ret 0, DDK `K:r15p0-00rel0(GPL)`; no `/dev/dri` node |
| Live props | **PASS**: `kbw-selftest-g31 --print-caps` rc 0 — GPU_ID `0x07201000` (0x720 r1p0), shader 0x3 (2 cores), tiler 1, l2 1, mmu `0x2821` (VA 33), l2 `0x07110206`, tiler `0x0809`, thread `0x0a040400`, js 0x7 (3 slots), 1 group / 1 coherent group, coherency `0x1f` NONE |
| Live memory + job + fence | **PASS**: full `kbw-selftest-g31` rc 0 — BO `cpu=gpu=0x1ffffe000` 4096 flags `0x300f` (SAME_VA), soft `DUMP_CPU_GPU_TIME` DONE, 227 GPU cycles, out-fence fd signalled, in-fence submit accepted, context idle |
| Staged device binaries (in `/data`, not the repo) | `/data/kbase-probe-g31`, `/data/kbw-selftest-g31` |

## 1. Transport (not ioctl numbers)

One misc node (`/dev/mali0`, major 10 minor 59 here); one ioctl whose
**size field selects the argument buffer** (`mali_kbase_core_linux.c:1342-1365`):

* `size = _IOC_SIZE(cmd)`; type/number/direction bits are **ignored**.
  Canonical construction: `KBASE_UK_IOC(size) = _IOC(_IOC_WRITE,0,0,size)`.
* `size > CALL_MAX_SIZE` (568 = `KBASE_FUNC_MAX − 1`, `:1340`) → `-ENOTTY`.
* Whole buffer copied in, dispatched on `((union uk_header *)args)->id`,
  copied back. Dispatch failure → ioctl returns `-1`/`EFAULT` (no errno detail).
* Every struct below is rejected unless `_IOC_SIZE == sizeof(struct)`
  (`bad_size`, one check per case in `kbase_dispatch()`), except the
  handshake's own exact check (`:300`).
* Order is enforced: `UKP_FUNC_ID_CHECK_VERSION` (id 0) first; then
  `KBASE_FUNC_SET_FLAGS` before anything else. Events arrive via `read()`
  (24-byte records), pollable.
* `UK_FUNC_ID = 512` (`mali_uk.h:85`); internal `UKP_FUNC_ID_CHECK_VERSION = 0`.

## 2. Function-id table (r15p0, `mali_kbase_uku.h:479-569`)

`KBASE_FUNC_MAX = 569`; `CALL_MAX_SIZE = 568`. Gaps are real (13, 17, 30,
33/34 unit-test-only, 37): sending those ids returns `-EINVAL`.

| id | Name | Call struct | size | winsys uses it |
|---|---|---|---|---|
| 0 | `UKP_FUNC_ID_CHECK_VERSION` | `uku_version_check_args` | 16 | yes (first call) |
| 512 (+0) | `MEM_ALLOC` | `kbase_uk_mem_alloc` | 56 | yes |
| 513 (+1) | `MEM_IMPORT` | `kbase_uk_mem_import` | 48 | no |
| 514 (+2) | `MEM_COMMIT` | `kbase_uk_mem_commit` | 32 | no |
| 515 (+3) | `MEM_QUERY` | `kbase_uk_mem_query` | 32 | no |
| 516 (+4) | `MEM_FREE` | `kbase_uk_mem_free` | 16 | yes (non-SAME_VA path) |
| 517 (+5) | `MEM_FLAGS_CHANGE` | `kbase_uk_mem_flags_change` | 32 | no |
| 518 (+6) | `MEM_ALIAS` | `kbase_uk_mem_alias` | 56 | no |
| 520 (+8) | `SYNC` | `kbase_uk_sync_now` (wraps `base_syncset`) | 40 | no |
| 521 (+9) | `POST_TERM` | `kbase_uk_post_term` | 8 | no |
| 522 (+10) | `HWCNT_SETUP` | `kbase_uk_hwcnt_setup` | 40 | no |
| 523 (+11) | `HWCNT_DUMP` | `kbase_uk_hwcnt_dump` | 8 | no |
| 524 (+12) | `HWCNT_CLEAR` | `kbase_uk_hwcnt_clear` | 8 | no |
| 526 (+14) | `GPU_PROPS_REG_DUMP` | `kbase_uk_gpuprops` | 536 | yes |
| 527 (+15) | `FIND_CPU_OFFSET` | `kbase_uk_find_cpu_offset` | 40 | no |
| 528 (+16) | `GET_VERSION` | `kbase_uk_get_ddk_version` | 80 | yes |
| 530 (+18) | `SET_FLAGS` | `kbase_uk_set_flags` | 16 | yes (second call) |
| 531 (+19) | `SET_TEST_DATA` | `kbase_uk_set_test_data` | — | no (test) |
| 532 (+20) | `INJECT_ERROR` | `kbase_uk_error_params` | — | no (test) |
| 533 (+21) | `MODEL_CONTROL` | `kbase_uk_model_control_params` | — | no (test) |
| 535 (+23) | `FENCE_VALIDATE` | `kbase_uk_fence_validate` | 16 | no |
| 536 (+24) | `STREAM_CREATE` | `kbase_uk_stream_create` | 48 | yes (per context) |
| 537 (+25) | `GET_PROFILING_CONTROLS` | `kbase_uk_profiling_controls` | — | no |
| 538 (+26) | `SET_PROFILING_CONTROLS` | `kbase_uk_profiling_controls` | — | no |
| 539 (+27) | `DEBUGFS_MEM_PROFILE_ADD` | `kbase_uk_debugfs_mem_profile_add` | 24 | no |
| 540 (+28) | `JOB_SUBMIT` | `kbase_uk_job_submit` | 40 | yes |
| 541 (+29) | `DISJOINT_QUERY` | `kbase_uk_disjoint_query` | 16 | no |
| 543 (+31) | `GET_CONTEXT_ID` | `kbase_uk_context_id` | 16 | yes (informational) |
| 544 (+32) | `TLSTREAM_ACQUIRE_V10_4` | `kbase_uk_tlstream_acquire_v10_4` | 16 | no |
| 547 (+35) | `TLSTREAM_FLUSH` | `kbase_uk_tlstream_flush` | 8 | no |
| 548 (+36) | `HWCNT_READER_SETUP` | `kbase_uk_hwcnt_reader_setup` | 32 | no |
| 550 (+38) | `SOFT_EVENT_UPDATE` | `kbase_uk_soft_event_update` | 24 | no |
| 551 (+39) | `MEM_JIT_INIT` | `kbase_uk_mem_jit_init` | 16 | no |
| 552 (+40) | `TLSTREAM_ACQUIRE` | `kbase_uk_tlstream_acquire` | 16 | no |
| 553 (+41) | `HWCNT_UTIL_SETUP` (Samsung) | vendor | — | no |
| 554 (+42) | `HWCNT_GPR_DUMP` (Samsung) | vendor | — | no |
| 555 (+43) | `VSYNC_SKIP` (Samsung) | vendor | — | no |
| 556 (+44) | `CREATE_SURFACE` (Samsung) | vendor | — | no |
| 557 (+45) | `DESTROY_SURFACE` (Samsung) | vendor | — | no |
| 558 (+46) | `SET_MIN_LOCK` (Samsung) | vendor | — | no |
| 559 (+47) | `UNSET_MIN_LOCK` (Samsung) | vendor | — | no |
| 560 (+48) | `TMU_SKIP` (Samsung) | vendor | — | no |
| 567 (+55) | `SECURE_WORLD_RENDERING` | — | — | no |
| 568 (+56) | `NON_SECURE_WORLD_RENDERING` | — | — | no |

## 3. Validated struct table (sizes measured from the kernel headers)

"Off" = byte offset. All rows confirmed by the host extractor (§0, row 1)
and by `kbase_uapi.h` static asserts; the ★ rows were additionally exercised
live tonight.

| Struct | size | Layout (off: field) | Semantics |
|---|---|---|---|
| `union uk_header` ★ | 8 | 0: `id` in / `ret` out (u32, aliased); 0–8: `sizer` (u64, forces 8-B alignment) | Every call starts with this; kernel writes `ret = MALI_ERROR_NONE` optimistically then the handler result |
| `uku_version_check_args` ★ | 16 | 8: `major` u16; 10: `minor` u16; 12: pad[4] | In: client version. Out: kernel version. `kbase_api_handshake()`: major 10 → `minor = min(6, peer)`; legacy 6/7/8/9 gated on `BASE_LEGACY_UK*_SUPPORT` (not observed); other majors → kernel answers 10.6 and userspace must bail. Live: offered 10.6, got 10.6, ret 0 |
| `kbase_uk_set_flags` ★ | 16 | 8: `create_flags` u32; 12: pad | Mandatory second call; `BASE_CONTEXT_CREATE_FLAG_NONE = 0` is what the winsys sends. Anything else first leaves the context stuck in setup. Live: ret 0 |
| `kbase_uk_get_ddk_version` ★ | 80 | 8: `version_buffer[64]`; 72: `version_string_size` u32; 76: pad | Kernel writes `"K:" MALI_RELEASE_NAME "(GPL)"`. Live: `K:r15p0-00rel0(GPL)`. `version_string_size` includes NUL |
| `kbase_uk_context_id` ★ | 16 | 8: `id` int | Reports the kernel context id (context is created implicitly by `open()`). Winsys reads it, ignores failure |
| `kbase_uk_gpuprops` ★ | 536 | 8: `mali_base_gpu_props` (528, fixed) | Register-dump blob; never TLV on this kernel. Winsys parses it into `kbw_caps` (see §5). Live values in §0 |
| `kbase_uk_mem_alloc` ★ | 56 | 8: `va_pages` u64; 16: `commit_pages` u64; 24: `extent` u64; 32: `flags` u64 in/out; 40: `gpu_va` u64 out; 48: `va_alignment` u16 + pad[6] | On 64-bit clients the kernel forces `BASE_MEM_SAME_VA`; `gpu_va` is a **cookie** `(idx+64)<<12`, not an address — userspace must `mmap(fd, pages*4096, RW, MAP_SHARED, cookie)` and the resulting `vma->vm_start` is both CPU and GPU VA. Winsys growable (`GROW_ON_GPF`) path commits 1 page initially. Live: 4096-B BO mapped `cpu == gpu`, flags `0x300f` |
| `kbase_uk_mem_free` ★ | 16 | 8: `gpu_addr` u64 | Frees an **unmapped cookie**. On a SAME_VA mapping it returns `-EINVAL` — free by `munmap` instead. Winsys branches on `flags & SAME_VA` |
| `kbase_uk_mem_query` | 32 | 8: `gpu_addr`; 16: `query` (`1`=commit size, `2`=VA size, `3`=flags); 24: `value` out | Introspection only; winsys does not call it |
| `kbase_uk_mem_commit` | 32 | 8: `gpu_addr`; 16: `pages`; 24: `result_subcode` u32 out; 28: pad | Growable-region commit; winsys does not call it directly |
| `kbase_uk_mem_flags_change` | 32 | 8: `gpu_va`; 16: `flags`; 24: `mask` | Atomic flag update; winsys does not call it |
| `kbase_uk_mem_import` | 48 | 8: `phandle` ptr; 16: `type` u32; 20: pad; 24: `flags`; 32: `gpu_va` out; 40: `va_pages` out | UMM/dma-buf import; winsys does not use it |
| `kbase_uk_mem_alias` | 56 | 8: `flags`; 16: `stride`; 24: `nents`; 32: `ai` ptr; 40: `gpu_va` out; 48: `va_pages` out | Alias regions incl. write-alloc handle; winsys does not use it |
| `kbase_uk_mem_jit_init` | 16 | 8: `va_pages` u64 | JIT region init; JIT allocs themselves are soft jobs (`JIT_ALLOC`/`JIT_FREE`); winsys does not call it |
| `kbase_uk_find_cpu_offset` | 40 | 8: `gpu_addr`; 16: `cpu_addr`; 24: `size`; 32: `offset` out | Reverse lookup; winsys does not call it |
| `kbase_uk_sync_now` | 40 | 8: `base_syncset` | Stream sync; winsys does not call it |
| `kbase_uk_hwcnt_setup` | 40 | 8: `dump_buffer` u64; 16: `jm_bm`; 20: `shader_bm`; 24: `tiler_bm`; 28: `unused_1`; 32: `mmu_l2_bm`; 36: pad | Legacy HWCNT; winsys does not call it |
| `kbase_uk_hwcnt_reader_setup` | 32 | 8: `buffer_count`; 12–24: `jm/shader/tiler/mmu_l2_bm`; 28: `fd` s32 out | HWCNT reader; winsys does not call it |
| `kbase_uk_hwcnt_dump` / `_clear` | 8 | header only | Winsys does not call them |
| `kbase_uk_post_term` | 8 | header only | Winsys does not call it |
| `kbase_uk_fence_validate` | 16 | 8: `fd` s32 in; 12: pad | Validate an Android sync fence fd; winsys polls fences directly instead |
| `kbase_uk_stream_create` ★ | 48 | 8: `name[32]` in; 40: `fd` s32 out; 44: pad | Per-context sync-timeline fd (`CONFIG_SYNC=y`). Winsys creates one per context (`"kbw-<pid>"`); soft fence atoms reference it. Live: stream created, trigger fd issued |
| `struct base_fence` ★ | 8 | 0: `basep.fd` int; 4: `basep.stream_fd` int | Soft fence descriptor. A fence-trigger/wait atom's `jc` is a **CPU pointer** to this, not a GPU VA; the kernel writes the new fence fd into `basep.fd` during `kbase_prepare_soft_job()`. `INVALID_PLATFORM_FENCE = -1` expected in `fd` before trigger. Live: trigger wrote fd 7 |
| `kbase_uk_job_submit` ★ | 40 | 8: `addr` ptr (array of `base_jd_atom_v2`); 16: `nr_atoms` u32; 20: `stride` u32; 24: `gles_ctx_handle` u32; 28: `frame_number` u32; 32: `surfacep` ptr | Driver requires `args_size == 40` **and** `stride == 64`; atoms are copied stride-wise, `atom_number` indexes the 256 persistent slots. Samsung systrace triple is unconditional in r15p0 (winsys zeroes it). Live: submits accepted |
| `base_jd_atom_v2` ★ | 64 | 0: `jc` u64; 8: `udata.blob[2]`; 24: `extres_list` ptr; 32: `nr_extres` u16; 34: `compat_core_req` u16 (UK≤10.2 only); 36: `pre_dep[2]` (`atom_id` u8 + `dependency_type` u8 each); 40: `atom_number` u8; 41: `prio` u8; 42: `device_nr` u8; 43: pad; 44: `core_req` u32; 48: `gles_ctx_handle` u32; 52: `frame_number` u32; 56: `surfacep` ptr | One job. `jc`: GPU VA of a Midgard job chain — except soft fence atoms (CPU `base_fence*`) and the dump-time atom (GPU counter buffer). `core_req` moved to 44 in UK≥10.3 (34 kept as compat). Prio: 0 medium / 1 high / 2 low. Deps: 0 invalid / 1 data (propagates faults) / 2 order. Live: soft-job atom number echoed back in the event |
| `base_jd_event_v2` ★ | 24 | 0: `event_code` (enum, 4 B); 4: `atom_number` u8 (+3 pad); 8: `udata.blob[2]` | One `read()` record per completed atom. Live: `DONE (0x01)` with matching atom number and udata tag |
| `base_dump_cpu_gpu_counters` ★ | 64 | 0: `system_time` u64; 8: `cycle_counter` u64; 16: `sec` u64; 24: `usec` u32; 28: pad[36] | Written by `SOFT_DUMP_CPU_GPU_TIME` into the `jc` buffer (must be GPU-writable). Live: 227 cycles, host timestamp. Note: kernel declares `usec` u32 + 36 pad; `kbase_uapi.h` writes `usec` u64 + 32 pad — both total 64 B at identical field offsets 0/8/16/24, so the wire is unaffected; the header transcription differs cosmetically |
| `union kbase_pointer` ★ | 8 | `value` ptr / `compat_value` u32 / `sizer` u64 | 8 bytes in every ABI (native 64-bit uses `value`); compat layer reads `compat_value`. Verified ILP32-size-8 by `verify-abi32.sh` design |
| `kbase_uk_disjoint_query` | 16 | 8: `counter` u32; 12: pad | Power-domain disjoint counter; winsys does not call it |
| `kbase_uk_soft_event_update` | 24 | 8: `evt` u64; 16: `new_status` u32 (SET 0x6 / RESET); 20: `flags` 0 | Pairs with `SOFT_EVENT_WAIT/SET/RESET` atoms; winsys does not call it |
| `kbase_uk_debugfs_mem_profile_add` | 24 | 8: `len` u32; 12: pad; 16: `buf` ptr | Debugfs profiling; winsys does not call it |
| `kbase_uk_tlstream_acquire` | 16 | 8: `flags` u32; 12: pad | UK-10.6 flags variant; winsys does not call it |
| `kbase_uk_tlstream_acquire_v10_4` | 16 | header + 8 payload | Legacy tlstream acquire; winsys does not call it |
| `kbase_uk_tlstream_flush` | 8 | header only | Winsys does not call it |

## 4. Semantics the table does not show

* **Handshake reply** (`kbase_api_handshake`): offering 10.x with x>6 returns
  10.6 — the winsys then rejects anything but (10, ≥3) or major >10, so a
  future kernel stays safe.
* **Read path**: `read(fd)` blocks until ≥1 24-byte event is queued
  (`O_NONBLOCK` → `-EAGAIN`; device teardown mid-read → `-EPIPE` via
  `DRV_TERMINATED`). Returns whole events only (`-ENOBUFS` if short).
* **Submit validation order** (`kbase_jd.c:1343-1352`): context submit-enabled
  → `stride == 64` → copy atoms → per-atom checks. A wrong-size ioctl buffer
  fails earlier in `kbase_dispatch` (`bad_size`), which is exactly how an
  r7p0-shaped (32-B) submit dies — proven by `kbase-selftest --probe-legacy`.
* **SAME_VA zone**: 39-bit VA (`mmu_features & 0xff` = 33 live is the
  *feature* encoding; `kbase_uapi.h` masks the low byte). `mmap(NULL,…)` lands
  inside the zone, so `cpu == gpu` always holds for 64-bit clients.
* **Tracking page first**: `mmap(... MAP_SHARED, 0x3000)` (handle 3<<12) must
  precede every other mapping; the winsys maps `PROT_NONE` 4096 B there and
  holds it for the fd lifetime.
* **mmap handles** (`mali_base_kernel.h:274-282`): invalid `0<<12`, tracking
  `3<<12` (0x3000), write-alloc `4<<12`, cookie base `64<<12` (0x40000).
* **Flags used live** (`mali_base_kernel.h:142-177`): `PROT_CPU_RD(1<<0) |
  PROT_CPU_WR(1<<1) | PROT_GPU_RD(1<<2) | PROT_GPU_WR(1<<3) |
  COHERENT_SYSTEM(1<<10) | CACHED_CPU(1<<12) | SAME_VA(1<<13)` = `0x300f`
  (kernel echoes it back in `flags` out). `NEED_MMAP (1<<14)` is output-mask
  only; `GROW_ON_GPF (1<<9)` is the winsys-local growable marker.
* **Soft-job vocabulary** (`mali_base_kernel.h:523-625`): `SOFT_JOB (1<<9)` |
  `0x1` dump-time, `0x2` fence-trigger, `0x3` fence-wait, `0x4` replay,
  `0x5/0x6/0x7` event wait/set/reset, `0x8` debug-copy, `0x9/0xa` JIT
  alloc/free, `0xb/0xc` ext-res map/unmap. Event-wait/set/reset need
  `SOFT_EVENT_UPDATE`. Winsys uses dump-time, fence-trigger, fence-wait.
* **Fence polling**: kbase fences are Android sync fds; `poll(POLLIN)` =
  signalled. No `sync_pt` ioctls needed.
* **Atom slots**: `BASE_JD_ATOM_COUNT 256`; slot 0 unused by the winsys
  (slots start at 1). Event `atom_number` selects the slot; `udata` is opaque
  echo (winsys tags dump-time atoms and checks the tag on completion).

## 5. GPU props (528-B blob, live decode)

Kernel order: `core(0,48) / l2(48,8) / unused(56,8) / tiler(64,8) /
thread(72,24) / raw(96,160) / coherency(256,272)`.
Key raw offsets: `shader_present` 96, `mem_features` 136, `mmu_features` 140,
`as_present` 144, `js_present` 148, `js_features[16]` 152,
`tiler_features` 216, `texture_features[3]` 220, `gpu_id` 232,
`thread_max_threads` 236, `thread_max_workgroup_size` 240,
`thread_max_barrier_size` 244, `thread_features` 248, `coherency_mode` 252.
Coherency info: `num_groups` 256, `num_core_groups` 260,
`group[16]` at 272 (mask u64 + `num_cores` u16 each).
Live decode (tonight): GPU_ID `0x07201000`, product `0x720`, 2 shader cores,
3 job slots, 1 group, coherency `0x1f` = NONE (non-coherent — the winsys
decodes `!= 31` as coherent; `gpu-verify.md` §6.2 fix retained).

## 6. What the winsys covers (and deliberately does not)

`code/userspace/gpu/kbase-winsys` speaks exactly 8 calls: `CHECK_VERSION`,
`SET_FLAGS`, `GET_VERSION`, `GET_CONTEXT_ID`, `GPU_PROPS_REG_DUMP`,
`MEM_ALLOC`/`MEM_FREE`, `STREAM_CREATE`, `JOB_SUBMIT` (+ `read()` events,
SAME_VA `mmap`, tracking-page `mmap`). Everything else in §2 is documented
for completeness so a future Mesa-backend or JIT/tiler-heap slice knows the
numbers without re-reading the kernel. Compat note: 32-bit AAPCS layout is
identical for the five hot structs (`verify-abi32.sh` asserts
version-check/mem_alloc/job-submit/atom/event sizes and the two
`systrace` offsets under `armv7a`).

## 7. Provenance and files

* Kernel header dump (host, tonight): `/tmp/kbsizes.c` method — full
  size+offset listing in §3, `KBASE_FUNC_MAX 569`.
* Repo code: `code/userspace/gpu/kbase/{kbase_uapi.h,kbase.c,kbase-selftest.c,tools/{kbase-layout.c,verify-abi.sh,verify-abi32.sh}}`,
  `code/userspace/gpu/kbase-probe/kbase-probe.c`,
  `code/userspace/gpu/kbase-winsys/{include/kbase_winsys.h,src/kbase_winsys.c,tests/kbw-selftest.c,tools/kbw-props-layout.c}`.
* Background: `docs/reports/gpu-kbase.md` §1 (transport/ids/layout),
  `gpu-verify.md` §§2/4/6 (live evidence + two fixed defects),
  `code/userspace/gpu/kbase/analysis.md` §1 (dispatch/memory model).
* Repo delta by this slice: **this report only** (`docs/reports/gpu31-kbase-abi.md`).
  Device binaries staged at `/data/kbase-probe-g31`, `/data/kbw-selftest-g31`
  (removed nothing, rebooted nothing, flashed nothing).
