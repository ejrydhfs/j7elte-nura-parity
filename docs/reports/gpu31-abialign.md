# gpu31-abialign — kernel vs Mesa 20.3 syncobj/panfrost ABI alignment

Date: 2026-11-20. Slice: G31_ABIAlign. Phone up on int16 (renderD128 live,
Mesa 20.3.5 renders `Mali T720 (Panfrost)` but draws are black / readback
zero because no `PANFROST_SUBMIT` ever reaches the GPU).
Related: `gpu31-target.md` (v5.2 H1 lock), `gpu31-mesa.md`, `gpu31-mesa20.md`,
`gpu31-syncobj.md`, `gpu31-submit-hw.md`, `gpu31-integration16.md`.

## 0. Verdict

Two live defects found by comparing the kernel structs against Mesa 20.3.5's
bundled `include/drm-uapi/{panfrost_drm.h,drm.h}` (fetched from
`/data/mesabuild/mesa20` on-device) plus the disassembled system libdrm
2.4.134 wrappers — then **fixed in this slice, pending flash** (no flash
from this ticket; Main/integrator own the rebuild):

1. **`SYNCOBJ_HANDLE_TO_FD` / `FD_TO_HANDLE` reject `flags==1` with
   `-EINVAL`.** Mesa 20.3 libdrm sets bit 0 (`EXPORT_SYNC_FILE` /
   `IMPORT_SYNC_FILE`) **unconditionally** (disassembly §4), and
   `pan_screen.c:panfrost_fence_create` calls `drmSyncobjExportSyncFile`
   on **every** flush (`glFinish`/`ReadPixels`), so every draw tripped
   the reject 3× and `SUBMIT` was never issued. Fix: tolerate bit 0,
   reject all other unknown bits (TIMELINE bit 1 included — the 0003
   shim has no timeline points; silently accepting them would hang Mesa).
2. **Copy-back over-write on longer userspace structs.** The 3.10 core
   keys `copy_to_user` on the caller's `_IOC_SIZE`, so a 24 B
   `drm_syncobj_handle` (current headers add `__u64 point`) got 24 B
   written back from a 16 B kernel struct — 8 B of stack leaked to
   userspace. The dispatcher's copy-in had the same shape (a 40 B WAIT
   with `deadline_nsec` over-ran the 32 B struct). Fix: truncate
   copy-in to our size, cap copy-back at our size (§5).

`GET_PARAM` needs **no** struct fix: the 16 B shape is identical on v5.2,
Mesa 20.3, and current headers (table §1). The 3 `GET_PARAM → EINVAL`
rows in the egltest strace are the *correct* upstream behaviour — Mesa
probes non-`PROD_ID` params with `required=false` and takes compiled-in
defaults on `-EINVAL` (`panfrost_query_raw`). Likewise the 385
`MADVISE → EINVAL` rows are benign by Mesa design (`pan_bo.c:207-276`
ignores the return). The ticket's "MADVISE or PANFROST_MADVISE" strace
decode is ambiguous: our node has no `0x08` panfrost row, so every one
of those 385 is the unregistered ioctl falling through to `-EINVAL`
(the BO-cache keeps working, pages just stay unreclaimable — accepted
in `gpu31-target.md` §2).

## 1. Comparison table (kernel vs Mesa 20.3 `include/drm-uapi`)

Wire truth measured on-device 2026-11-20 (see §7 probes): `panfrost_drm.h`
+ `drm.h` from `/data/mesabuild/mesa20`, `sizeof`/offset probe, and
`objdump -d /usr/lib/libdrm.so.2.134.0` for the exact ioctl words libdrm
emits. All structs LP64 (`__u64` 8 B aligned).

### 1a. Panfrost (6 ioctls — all match, no fix)

| ioctl | nr | dir/size (both sides, identical) | struct (field, type, offset, size) | flags gate | live probe |
|---|---|---|---|---|---|
| SUBMIT | 0x00 | IOW `0x40286440` 40 B | `jc` u64 0/8; `in_syncs` u64 8/8; `in_sync_count` u32 16/4; `out_sync` u32 20/4; `bo_handles` u64 24/8; `bo_handle_count` u32 32/4; `requirements` u32 36/4 | `AUTH` (Mode-B render always-authenticated) + `UNLOCKED`; `!jc\|jc&7`→EINVAL; `reqs & ~FS`→EINVAL (no tail: 20.3 header has no `jm_ctx_handle`; Mesa 20.3 never sends it) | 122× `= 0` in egltest draw (`/data/egl.strace`) — submits DO reach the kernel once HTOF is fixed |
| WAIT_BO | 0x01 | IOW `0x40106441` 16 B | `handle` u32 0/4; `pad` u32 4/4 (must be 0); `timeout_ns` s64 8/8 absolute `CLOCK_MONOTONIC` | `UNLOCKED` | 126× `= 0` |
| CREATE_BO | 0x02 | IOWR `0xc0186442` 24 B | `size` u32 0/4; `flags` u32 4/4 (must be 0 — Mesa 20.3 sends 0, driver is v1.0); `handle` u32 8/4; `pad` u32 12/4; `offset` u64 16/8 | `UNLOCKED` | 133× `= 0` |
| MMAP_BO | 0x03 | IOWR `0xc0106443` 16 B | `handle` u32 0/4; `flags` u32 4/4 (=0); `offset` u64 8/8 | `UNLOCKED` | 75× `= 0` |
| GET_PARAM | 0x04 | IOWR `0xc0106444` 16 B | `param` u32 0/4; `pad` u32 4/4 (=0); `value` u64 8/8 | `UNLOCKED`; only `param==0` (`GPU_PROD_ID`) succeeds → `0x720`; all else `-EINVAL` (upstream-identical gate; order fixed: param check first, then pad) | prod probe `= 0 value=0x720`; 3× further probes `= -1 EINVAL` → **correct, Mesa defaults** |
| GET_BO_OFFSET | 0x05 | IOWR `0xc0106445` 16 B | `handle` u32 0/4; `pad` u32 4/4 (=0); `offset` u64 8/8 | `UNLOCKED` | covered by selftest round-trip |

### 1b. Syncobj (7 ioctls — HTOF/FTOH flag gate + copy sizes fixed)

20.3 `drm.h` syncobj shapes (no `point` field on HANDLE, no
`deadline_nsec` on WAIT — both postdate 20.3; current host headers have
24 B / 40 B and MUST NOT be used as the kernel struct):

| ioctl | nr | wire (both sides after fix) | struct (field, type, offset, size) | flags gate (after fix) | live probe |
|---|---|---|---|---|---|
| CREATE | 0xBF | IOWR `0xc00864bf` 8 B | `handle` u32 0/4; `flags` u32 4/4 | only `SIGNALED`(bit 0), else `-EINVAL` (unchanged) | `= 0` (pre- and post-fix) |
| DESTROY | 0xC0 | IOWR `0xc00864c0` 8 B | `handle` u32 0/4; `pad` u32 4/4 (=0) | (unchanged; doc fix only — the old `DRM_IOW` client spelling aliased the same `_IOC_NR`, but new code MUST use IOWR per 20.3 headers) | `= 0` |
| HANDLE_TO_FD | 0xC1 | IOWR `0xc01864c1` **16 B** | `handle` u32 0/4; `flags` u32 4/4; `fd` s32 8/4; `pad` u32 12/4 | **bit 0 tolerated**, all other bits `-EINVAL`; `pad` `-EINVAL`; unknown handle `-ENOENT`; NULL fence `-EINVAL` | pre-fix: `flags=0`→0 / `flags=1`→`-EINVAL` (§7); post-fix `flags=1`→0 (mock + code review; device proof needs flash) |
| FD_TO_HANDLE | 0xC2 | IOWR `0xc01864c2` **16 B** | same 16 B | **same as HTOF** (was: all flags `-EINVAL`) | same as HTOF |
| WAIT | 0xC3 | IOWR `0xc02864c3` **32 B** | `handles` u64 0/8; `timeout_nsec` s64 8/8 abs; `count_handles` u32 16/4; `flags` u32 20/4; `first_signaled` u32 24/4; `pad` u32 28/4 | `ALL`(bit 0) honoured; `FOR_SUBMIT/AVAILABLE`(bits 1-2) + `DEADLINE`(bit 3) `-EINVAL` (unchanged) | 32 B and 40 B encodings both `= 0` on signalled (dispatcher truncates; §7) |
| RESET | 0xC4 | IOWR `0xc01064c4` 16 B | `handles` u64 0/8; `count_handles` u32 8/4; `pad` u32 12/4 | (unchanged) | — |
| SIGNAL | 0xC5 | IOWR `0xc01064c5` 16 B | same 16 B | (unchanged) | — |

## 2. Why GET_PARAM/HANDLE_TO_FD return EINVAL, and why SUBMIT is never attempted

- `GET_PARAM → EINVAL (3×)`: **not a bug.** Mesa probes `SHADER_PRESENT`
  etc. with `required=false`; our `-EINVAL` (upstream-identical) makes
  each caller take its default (`pan_props.c`). Verified: prod probe
  returns `0x720`, exactly one `= 0` + three `= -EINVAL` in both traces.
- `HANDLE_TO_FD → EINVAL (3×)`: **the blocker.** `panfrost_fence_create`
  (`pan_screen.c:677-721`) runs on every flush: `ExportSyncFile(fd,
  ctx->syncobj)` with `flags=1` → our old `if (args->flags) return
  -EINVAL` → `export failed` → fence `NULL` → flush path returns early
  → `Submit` is never called. The egltest log prints `export failed`
  and `RENDER_MISMATCH` (all-zero readback) with **zero** SUBMIT rows
  in its trace — matching the ticket's "SUBMIT never attempted".
- `MADVISE → EINVAL (385×)`: benign (Mesa ignores the return; §0).
- `SUBMIT` itself is healthy: once HTOF is fixed, submits flow (122×
  `= 0` in the older `/data/egl.strace` from a run that got further;
  our own `probe_abi4` NULL-job `SUBMIT rc=0` on silicon).

## 3. Root cause (one line each)

- HTOF/FTOH: the syncobj port assumed `flags==0` ("no sync_file.c, so no
  flags"), but libdrm 2.4.134 **always** sets `EXPORT/IMPORT_SYNC_FILE`
  (bit 0) — so the only flag Mesa ever sends was the one we rejected.
- Copy sizes: the dispatcher copied the *caller's* `_IOC_SIZE` bytes and
  wrote the *caller's* size back — correct only when userspace and kernel
  agree on the struct size, which they don't for current (24/40 B) headers.

## 4. libdrm 2.4.134 evidence (disassembly, on-device)

- `drmSyncobjHandleToFD`: `mov w1,#0x64c1; movk w1,#0xc018` →
  `0xc01864c1` (IOWR 24 B: `stp xzr` clears `point` at `[sp,#12]`),
  flag word `str wzr,[sp,#20]`→ overwritten per-call; `ExportSyncFile`
  variant: `mov x1,#0xffffffff00000001` (fd=-1) + `mov w1,#1` (flags=1).
- `drmSyncobjCreate`: `0xc00864bf`; `drmSyncobjDestroy`: `0xc00864c0`
  (**IOWR**, not IOW); `drmSyncobjWait`: `0xc02864c3` (40 B: libdrm
  zeroes only 32 B at `[sp]`+`[sp,#24]`, `deadline_nsec` left from a
  40 B stack slot); `drmSyncobjImportSyncFile`: `0xc01864c2` flags=1.
- So the live userspace sends 24 B HTOF / 40 B WAIT: our `_IOC_NR`-keyed
  dispatcher routes them correctly, and after this slice truncates
  (never over-copies) both directions.

## 5. Kernel-side fix (this slice)

`code/gpu31/gem/panfrost7580_syncobj.c` (+ regen into
`code/gpu31/driver/0001-*.patch`):
- `p7580_syncobj_abi_check()` (`BUILD_BUG_ON` sizes/offsets/flag bits)
  in this TU; call it in create/dispatch paths. `panfrost7580_ioctl.c`
  gains `p7580_abi_check_get_param/submit()` (16 B get_param, 40 B
  submit, field offsets) with a call in `panfrost_ioctl_get_param`.
- HTOF/FTOH: `if (args->flags & ~SYNC_FILE_BIT) return -EINVAL;`
  (was `if (args->flags)`); pad/ENOENT/NULL-fence rules unchanged.
- Dispatcher `p7580_syncobj_do`: copy-in truncated to `min(usize,asize)`
  + zero-pad; copy-back capped at `min(usize,asize)` (no stack leak).
- Doc/header corrections: DESTROY is IOWR on the wire (client
  `pf7580.c` macro fixed to `DRM_IOWR`, same `_IOC_NR` so dispatch
  unaffected); stale "flags must be 0" comments updated in both TUs.
- `GET_PARAM`: check `param` before `pad` (upstream order), keep the
  PROD_ID-only gate.

`code/gpu31/client/` (selftest + library, host-verified):
- `PF7580_IOCTL_SYNCOBJ_DESTROY` → `DRM_IOWR`; new 16 B
  `pf7580_syncobj_handle` (+`HANDLE_TO_FD`/`FD_TO_HANDLE` macros with
  `DRM_IOWR`), `SYNC_FILE` bit-0 defines; new
  `pf7580_syncobj_handle_to_fd/fd_to_handle` (real + mock backends).
- Selftest: `--print-abi` prints all 8 wire sizes; mock suite adds
  `t_get_param_abi` (Mesa's u64 probe order) + `t_syncobj_abi`
  (CREATE-signalled → HTOF flags=1 → FTOH flags=1 → WAIT round-trip);
  `--device` path adds the same two sequences on silicon.

## 6. Selftest output (device, int16 kernel = PRE-fix build)

Static cross binary `/tmp/abialign/pf7580-abialign` (built from this
slice's client), run 2026-11-20 under `flock /tmp/gpsdev.lock`:

```
--print-abi -- submit 40 wait_bo 16 create_bo 24 mmap_bo 16 get_param 16
get_bo_offset 16 syncobj_handle 16 syncobj_wait 32 T720 0x0720  ABI-RC=0
-- mock -- MOCK SUITE: PASS (incl. new t_get_param_abi + t_syncobj_abi) MOCK-RC=0
-- device /dev/dri/renderD128 --
GPU_PROD_ID 0x720 (T720); BO handle=1 gpu_offset=0x37b7000; CPU map R/W ok
GET_PARAM u64: OK 0x720
GET_PARAM SHADER_PRESENT: EINVAL ok (Mesa default path)
SYNCOBJ_CREATE signaled: OK handle=1
FAIL handle_to_fd SYNC_FILE: EINVAL        <-- the reported bug, on the OLD kernel
DEVICE PATH: FAILED (1)
-- probe-only -- PASS (GET_PARAM/CREATE/MMAP/GET_BO_OFFSET round-trip)
```

Plus pre-fix targeted probes (§7): `GET_PARAM(REVISION/SHADER/TILER/L2)`
all `-EINVAL` (correct); `HTOF flags=0` on a signalled syncobj `= 0 fd=5`
(proves the fd-install path works — the ONLY broken case is `flags==1`);
`WAIT` 32 B and 40 B encodings both `= 0`; `SUBMIT` NULL-job `rc=0`.
Post-flash rerun of the same binary must show `HANDLE_TO_FD SYNC_FILE:
OK` + `FD_TO_HANDLE SYNC_FILE: OK (round-trip)` and `DEVICE PATH: PASS`;
then `eglinfo`/`egltest` should show SUBMIT rows and non-zero readback.

## 7. Raw probe evidence (kept: `/tmp/abiw*`, `/tmp/probe_abi*` on device)

- `abiw`: Mesa-header sizes/encodings — submit 40 `0x40286440`, wait_bo
  16, create 24, mmap 16, get_param 16 `0xc0106444`, get_bo_offset 16,
  handle 24 `0xc01864c1`, create 8, wait 40 `0xc02864c3`.
- `abiw2`: offsets — submit 0/8/16/20/24/32/36; get_param 0/4/8; handle
  0/4/8/12/16(`point`); wait 0/8/16/20/24/28.
- `probe_abi`: PROD_ID `r=0 value=0x720`; ids 1-4 `r=-22`; CREATE
  signalled/plain `r=0`; HTOF plain-unsignalled `-22`, HTOF SYNC_FILE
  (signalled) `-22`, HTOF plain signalled `r=0 fd=5`.
- `probe_abi2`: 16 B HTOF code `0xc01064c1` `r=0 fd=5`; pad!=0 `-22`.
- `probe_abi3`: WAIT 40 B and 32 B on signalled `r=0`.
- `probe_abi4/6`: `SUBMIT rc=0`; stale `out_sync` → `-ENOENT` (correct);
  `WAIT_BO t=0` fresh `r=0`.

## 8. Files changed

- `code/gpu31/gem/panfrost7580_ioctl.c` — ABI gate + GET_PARAM order.
- `code/gpu31/gem/panfrost7580_syncobj.c` — ABI gate, SYNC_FILE
  tolerance, truncation/capped copy, comment fixes.
- `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` — regen of
  the 4 GEM hunks (ioctl.c +46, syncobj.c +104 net; other 15 files
  byte-identical).
- `code/gpu31/client/{pf7580.c,pf7580.h,pf7580-selftest.c}` — DESTROY
  direction, 16 B handle mirror, HTOF/FTOH wrappers, new tests.
- this report.

Checks: host `make` + `make check` (mock incl. new tests) PASS;
aarch64 static cross-build PASS (`/tmp/pf7580-abialign` 936264 B);
`patch -p1 --dry-run` of regen'd 0001 clean; device mock+ABI PASS,
device GET_PARAM + pre-fix HTOF-fails-as-reported proven above. NOT
done (needs integrator/Main): Image rebuild + flash + post-flash rerun
of `/tmp/abialign/pf7580-abialign --device` (expect PASS incl. SYNC_FILE
round-trip) and `egltest` (expect SUBMIT rows, non-zero pixels).
