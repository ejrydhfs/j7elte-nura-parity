# GPU31 syncobj ioctl surface — generic DRM syncobj for Mesa 20.3.5 on 3.10

Date: 2026-11-20 (UTC). Slice: G31_SyncobjIoctl.

Device state per Main: kernel int5+mmap2 live (renderD128 only, Xorg
healthy); GET_PARAM=0x720, CREATE_BO up to 4 MiB OK; no GPU submits
(PM-ordering fix pending). Mesa 20.3.5 loads panfrost_dri.so then dies
at **DRM_IOCTL_SYNCOBJ_CREATE = EINVAL** — pan_context.c:1632-1633
asserts (`assert(!ret && ctx->syncobj)`).

Repo: /tmp/j7repo. Rules: patches only (never the shared tree); commit
own files. Phone: SSH user@172.16.42.1 (helper /tmp/ap.sh), root via
rootshell, cmds under host `flock /tmp/gpsdev.lock`; do not flash
(Main); do not run GPU submits.

## 0. Verdict

The blocker is a **missing ioctl surface, not missing objects**. The
drm-backport provides the per-file syncobj handle table (0003), but no
patch registers the generic DRM syncobj ioctl numbers on our driver's
ioctl path, so every `drmSyncobjCreate` falls through to `-EINVAL`.
Fixed in this slice: 7 generic syncobj ioctls, fops-routed, backed by
the 0003 shim, errno-exact vs Mesa 20.3.5. Host-verified (syntax +
object + contract model + apply checks); device eglinfo re-run is
Main/Integrator's after the next flash (no submits run here).

Mesa assert line satisfied: `pan_context.c:1632-1633`
(`panfrost_create_context`, stock 20.3.5):
`ret = drmSyncobjCreate(dev->fd, DRM_SYNCOBJ_CREATE_SIGNALED,
&ctx->syncobj); assert(!ret && ctx->syncobj);`
CREATE with `DRM_SYNCOBJ_CREATE_SIGNALED` now returns 0 with a
signaled syncobj instead of EINVAL.

## 1. Why the driver ioctl table cannot serve these (3.10 dispatch)

`drm_ioctl` (3.10 `drm_drv.c:381-435`, pristine copy at
`/tmp/kms-check/src/drm_drv.c`):

- `nr = DRM_IOCTL_NR(cmd)` (low 8 bits). If `nr` in
  `[DRM_COMMAND_BASE, DRM_COMMAND_END)` (0x40–0xA0) **and**
  `nr < BASE + num_ioctls`, the driver's `.ioctls` row serves it.
- Else if `nr >= DRM_COMMAND_END || nr < BASE`, the **core** table
  serves it; core has no SYNCOBJ rows on 3.10 (table ends at 0xBA),
  so `func == NULL` → `retcode = -EINVAL`.
- Generic syncobj numbers (Mesa 20.x `include/drm-uapi/drm.h`,
  identical in current `/usr/include/drm/drm.h:1254-1260`) are
  0xBF–0xC5 — all `>= DRM_COMMAND_END`. They can **never** route
  through the driver's `.ioctls` table on 3.10, no matter how many
  rows we add. (Upstream only fixed this later by adding core rows +
  `drm_syncobj.c` to the core.)
- 20.x libdrm (aarch64, on-device `/tmp/ir/usr/lib/libdrm.so.2`,
  disassembled) emits exactly these ioctl codes:
  CREATE `0xc00864bf`, DESTROY `0xc00864c0`, HANDLE_TO_FD
  `0xc01864c1`, FD_TO_HANDLE `0xc01864c2`, WAIT `0xc02864c3`,
  RESET/SIGNAL `0xc01064c4/5` (dir+size prefix + `0x64` type + nr).

So the registration is at the **fops** level: the driver's
`.unlocked_ioctl`/`.compat_ioctl` point at `panfrost_syncobj_ioctl`,
which serves nr 0xBF–0xC5 itself and forwards everything else to
`drm_ioctl()` unchanged (fast path: driver-private 0x40–0x5F numbers
go straight to `drm_ioctl` without extra work).

## 2. What was delivered

| File | Change |
|---|---|
| `code/gpu31/gem/panfrost7580_syncobj.h` | NEW (70 lines): 7 handler protos + fops dispatcher protos; UAPI notes (numbers, sizes, scope) |
| `code/gpu31/gem/panfrost7580_syncobj.c` | NEW (708 lines): 7 ioctl bodies + anon-inode FD transport + fops dispatcher with `drm_ioctl`-identical copy discipline |
| `code/gpu31/driver/0001-panfrost7580-driver-sources.patch` | +2 new-file hunks (syncobj .c/.h, @@ counts `1,70` / `1,708`); main.c: `#include "panfrost7580_syncobj.h"` + fwd decls; fops `.unlocked_ioctl = panfrost_syncobj_ioctl`, `.compat_ioctl = panfrost_syncobj_compat_ioctl` (+ `#else NULL`) |
| `code/gpu31/driver/0002-panfrost7580-build-wiring.patch` | panfrost Makefile hunk `1,7→1,8`: `panfrost7580_syncobj.o` |
| `code/gpu31/drmcore/drm_compat_310.h` | F-DRM1 (gpu31-driver-review): `DEFINE_DRM_GEM_FOPS_COMPAT` gains the guarded `.compat_ioctl = drm_compat_ioctl` entry (was a byte-copy of the plain macro) |
| `docs/reports/gpu31-syncobj.md` | this file |

No edits to any sibling-owned source: GEM `panfrost7580_{gem,ioctl}.*`
untouched; 0003 shim untouched; KMS Mode-B untouched.
`p7580_job_dev_fini` one-liner (G31_SubmitHW, committed 78e37f8)
preserved verbatim in 0001 context.

## 3. Ioctl table (numbers, structs, flags)

Numbers are core (NOT driver-private): `DRM_IOWR(0xBF–0xC5, struct …)`.

| # | ioctl | nr | struct (wire size) | flags gate |
|---|---|---|---|---|
| 1 | SYNCOBJ_CREATE | 0xBF | `drm_syncobj_create` 8 B: `__u32 handle; __u32 flags` | `DRM_UNLOCKED` (fops path; no AUTH/MASTER — render fd is always-authenticated per Mode-B) |
| 2 | SYNCOBJ_DESTROY | 0xC0 | `drm_syncobj_destroy` 8 B: `__u32 handle; __u32 pad` | `DRM_UNLOCKED` |
| 3 | SYNCOBJ_HANDLE_TO_FD | 0xC1 | `drm_syncobj_handle` 16 B: `__u32 handle, flags; __s32 fd; __u32 pad` | `DRM_UNLOCKED` |
| 4 | SYNCOBJ_FD_TO_HANDLE | 0xC2 | same 16 B | `DRM_UNLOCKED` |
| 5 | SYNCOBJ_WAIT | 0xC3 | `drm_syncobj_wait` 32 B: `__u64 handles; __s64 timeout_nsec; __u32 count, flags, first_signaled, pad` | `DRM_UNLOCKED` |
| 6 | SYNCOBJ_RESET | 0xC4 | `drm_syncobj_array` 16 B: `__u64 handles; __u32 count, pad` | `DRM_UNLOCKED` |
| 7 | SYNCOBJ_SIGNAL | 0xC5 | same 16 B | `DRM_UNLOCKED` |

NOT registered (deliberate, same omissions as the 0003 shim):
TIMELINE_WAIT 0xCA / QUERY 0xCB / TRANSFER 0xCC / TIMELINE_SIGNAL 0xCD
(timeline payloads postdate the binary subset), EVENTFD, and
`DRM_CAP_SYNCOBJ_TIMELINE`. Mesa 20.3.5 panfrost never issues them
(full caller audit: only Create/Destroy/Wait/Export/Import appear in
`pan_screen.c` + `pan_context.c` + `pan_job.c`).

Struct/flag table (3.10-era sizes — 20.x `drm.h` parity, NOT current
headers where WAIT grew `deadline_nsec` to 40 B):

| Symbol | value | notes |
|---|---|---|
| `DRM_SYNCOBJ_CREATE_SIGNALED` | `1<<0` | only valid CREATE bit; anything else → `-EINVAL` |
| `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL` | `1<<0` | honoured (ALL vs ANY) |
| `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT` | `1<<1` | `-EINVAL` (needs timeline/eventfd code) |
| `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE` | `1<<2` | `-EINVAL` (same) |
| deadline bit | `1<<3` | `-EINVAL` (needs 0001 fence deadlines; 20.x WAIT struct has no deadline field — 32 B) |
| `*_FD_*_FLAGS_*_SYNC_FILE` | `1<<0` | `-EINVAL` on HANDLE_TO_FD/FD_TO_HANDLE (no sync_file.c in 3.10) |
| handle cap per call | 64 | mirrors SUBMIT (`P7580_MAX_SUBMIT_BOS`); more → `-EINVAL` before any copy |
| `O_CLOEXEC` | — | all exported fds cloexec (`anon_inode_getfd`) |

Size proof (host, 20.x drm.h vs ours — all equal):
`create=8 destroy=8 handle=16 wait=32 array=16`.
(WARN: current `/usr/include/drm/drm.h` says `wait=40` — that is the
post-20.x deadline growth; our 32 B matches what 20.3.5 was built
against.)

## 4. errno semantics (what Mesa expects, per call)

- CREATE: unknown flag bits → `-EINVAL` (checked as
  `flags & ~SIGNALED`, mainline shape). `SIGNALED` installs an
  already-signaled private fence (0001 `dma_fence_init` + `signal`;
  released on replace) so the first WAIT returns 0. OOM → `-ENOMEM`
  (fence alloc fail rolls back the handle). No driver_priv (open never
  ran) → `-ENODEV`.
- DESTROY: `pad != 0` → `-EINVAL`; unknown handle → `-ENOENT`
  (0003 contract; mainline parity on stale handles).
- HANDLE_TO_FD: `flags != 0` → `-EINVAL` (only plain-fence transport);
  `pad` → `-EINVAL`; unknown handle → `-ENOENT`; NULL fence (never
  submitted) → `-EINVAL` (nothing to export); fd-alloc fail passes the
  negative fd error through. The fd holds a fence ref (released in
  `->release`); later replaces do NOT follow the fd (documented
  non-sync_file limit).
- FD_TO_HANDLE: `flags/pad` → `-EINVAL`; `fd < 0` → `-EINVAL`; `fget`
  NULL → `-EINVAL`; fds not created by this driver (`f_op` check) →
  `-EINVAL` (never a blind dereference — eventfd/pipe/foreign
  sync_file rejected); creates a new syncobj and snapshots the fd's
  current fence into it.
- WAIT: `count == 0 || !handles` → `-EINVAL`; unknown `flags` bits →
  `-EINVAL` (never silently ignore — Mesa would hang on an un-waited
  fence); unknown handle → `-ENOENT`; `count > 64` → `-EINVAL`;
  `copy_from_user` fault → `-EFAULT`. Timeout is ABSOLUTE
  CLOCK_MONOTONIC ns (same conversion as WAIT_BO: `ktime_get_ts` delta
  → `nsecs_to_jiffies`; past stamp = non-blocking 0). ALL: chained
  budget (each fence consumes the remainder; 0001 returns remaining).
  ANY: zero-timeout probe first, then bounded per-fence waits; none
  signaled in budget → `-ETIME`; first-signaled index in
  `first_signaled`, still 0 on success. NULL fence counts as signaled
  (0003 contract).
- RESET: `count == 0 || !handles` → `-EINVAL`; `> 64` → `-EINVAL`;
  `copy_from_user` fault → `-EFAULT`; unknown handle → `-ENOENT`.
  Drops the fence (binary reset; Mesa only resets after WAIT, next
  SUBMIT re-installs via `replace_fence`).
- SIGNAL: same validation as RESET. Unknown handle → `-ENOENT`.
  Already-signaled (or empty) → no-op 0. Else installs a pre-signaled
  fence (plain `signal`, no error state — the 0001 fence has none).
- Dispatcher copy discipline mirrors `drm_ioctl` (`drm_drv.c:444-490`):
  fixed struct kernel-copied (`asize > usize` zero-pads, so a shorter
  20.x struct keeps working); only handle arrays copied per-element;
  `IOC_OUT` copied back on success only; unknown nr → `drm_ioctl()`.
  compat32 maps to the same handlers (structs are width-clean:
  u32/__u64/__s32, no pointers).

Host contract model (`/tmp/syncstage/fullsim.c`, pure-C branch mirror):
12/12 checks PASS — bad-flags EINVAL, signaled-create→wait-t0 0,
unsignaled wait-t0 ETIME, FOR_SUBMIT/AVAILABLE/DEADLINE EINVAL,
count-0 EINVAL, destroy-stale ENOENT, destroy-twice ENOENT.

## 5. Verification evidence (host-only; no flash/submit here)

- `aarch64-linux-gnu-gcc -fsyntax-only -Wall -Werror` (real 3.10.108
  headers, `__KERNEL__`, kconfig include, 0001-0003 shims staged):
  `panfrost7580_syncobj.c` **PASS, zero warnings**.
- Real `-c` object compile, same flags: `panfrost7580_syncobj.o`
  **13696 B, PASS, zero warnings**.
- `patch -p1 --dry-run` on both driver patches: **clean**
  (0001: all 19 files incl. the 2 new syncobj files; 0002: Kconfig +
  drm Makefile + panfrost Makefile with the new `syncobj.o` line).
- `fullsim` errno contract: **12/12 PASS** (§4).
- Struct sizes vs 20.x drm.h: **all equal** (§3 table).
- `git apply --check` full-series re-run + Image build + `eglinfo`
  re-run are Integrator/Main's (int7 script already globs
  `driver/000[12]-*.patch`, so the new hunks ride the normal build).

## 6. What is NOT done (honest gaps)

- No `drmGetCap(DRM_CAP_SYNCOBJ)` advertisement: 3.10 core has no
  `DRM_CAP_SYNCOBJ` row (`drm.h` ends at PRIME/timestamp caps); Mesa
  20.3.5 panfrost never queries the cap (audit §2 — it calls CREATE
  directly), so nothing was added. If a future Mesa gates on the cap,
  add a `DRM_IOCTL_GET_CAP` shim or document why not.
- FD transport is snapshot-based (no `sync_file.c` in 3.10): an
  exported fd pins the *current* fence; later `replace_fence` on the
  source syncobj does not propagate. Sufficient for
  `panfrost_fence_create` (export ctx->syncobj → import into a fresh
  syncobj, immediately consumed by `fence_finish`/WAIT), but NOT a
  general dma-fence fd-passing layer.
- No on-device proof in this slice: no flash (Main), no submits, no
  eglinfo. Expected post-flash: `eglinfo -p surfaceless -B` passes the
  CREATE assert; renderer still gated on the GEM-SHMEM 16 MiB heap
  (G31_GemShmem dcd5171, landed) + PM-ordering (c81e8a7, landed).
- 0003 shim itself unchanged: `drm_syncobj_wait` (single-syncobj) is
  unused by the multi-handle WAIT body (which calls
  `dma_fence_wait_timeout` directly); `RESET` uses
  `replace_fence(s, NULL)` (get(NULL) is a NULL-tolerant no-op per the
  shim header; put(NULL) likewise).
