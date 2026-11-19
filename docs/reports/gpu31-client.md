# gpu31-client — panfrost7580 userspace test client (six v5.2 ioctls)

Slice: `code/gpu31/client/` — a standalone userspace client that drives
the six panfrost7580 ioctls owned by `code/gpu31/gem`
(`panfrost7580_ioctl.c`) on the GEM core owned by `code/gpu31/drmcore`
(`panfrost_ioctls_310.h` rows). Style follows
`code/userspace/gpu/kbase-winsys` (thin static library + selftest with
exit 0/2/1, `--print-abi` with no device, `CROSS_COMPILE=` + qemu
`check-aarch64`, no libdrm).

Driver ABI (v5.2, byte-identical mirror `code/gpu31/gem/panfrost_drm.h`):

| # | ioctl | dir | rule (from `panfrost7580_ioctl.c`) |
|---|-------|-----|-------------------------------------|
| 0x00 | SUBMIT | IOW | `jc==0 \| (jc&7)` → EINVAL; `requirements & ~FS` → EINVAL; `in_sync_count>16` / `bo_handle_count>64` → EINVAL |
| 0x01 | WAIT_BO | IOW | `pad` → EINVAL; unknown handle → ENOENT (relative jiffies budget; ≤0 non-blocking) |
| 0x02 | CREATE_BO | IOWR | `flags` → EINVAL; `size==0` → EINVAL; returns per-fd GPU offset (always nonzero) |
| 0x03 | MMAP_BO | IOWR | `flags` → EINVAL; unknown handle → ENOENT (`map_list.hash.key << PAGE_SHIFT` fake offset) |
| 0x04 | GET_PARAM | IOWR | `pad` → EINVAL; only `GPU_PROD_ID` (0) accepted; returns `0x0720` (T720) |
| 0x05 | GET_BO_OFFSET | IOWR | `pad` → EINVAL; unknown handle → ENOENT (same value CREATE_BO returned) |

## 1. Files

- `code/gpu31/client/pf7580.h` — public API: `pf7580_open[_mock]`,
  one wrapper per ioctl, `pf7580_cpu_map/unmap`, `pf7580_mock_fail_next`
  fault injection, `pf7580_strerror`. Field-identical `pf7580_*` struct
  mirrors + `_Static_assert` wire sizes (40/16/24/16/16/16).
- `code/gpu31/client/pf7580.c` — real backend (DRM ioctl on a render-node
  fd) + mock backend (in-memory BOs: 2 MiB-spaced IOVAs from `0x10000000`
  inside the driver's 32 MiB .. 4 GiB window, `handle << 12` fake mmap
  offsets, malloc shadows). Client-side SUBMIT validation is shared so
  both backends reject identically; ioctl numbers are `_Static_assert`ed
  against the v5.2 direction/number (SUBMIT/WAIT_BO IOW, rest IOWR).
- `code/gpu31/client/pf7580-selftest.c` — mock suite (lifecycle + error
  paths + injection) and the safe real-node path (`--device PATH` /
  `PF7580_DEVICE`; GET_PARAM/CREATE/MMAP/GET_BO_OFFSET/WAIT + CPU
  map R/W, never SUBMIT — a test `jc` would fault hardware). Header-level
  `_Static_assert`s pin our mirrors to the driver's `panfrost_drm.h`
  field-for-field. (`panfrost_drm.h` gains one v5.2-later field,
  `jm_ctx_handle/pad`, in SUBMIT — 48 vs 40 bytes; out of scope for this
  v5.2 target, noted for Mesa: pass `out_sync=0`.)
- `code/gpu31/client/drm.h` — build shim redirecting the driver's
  `#include "drm.h"` to userspace `<drm/drm.h>` (types + `DRM_IOW/R`
  already provided there; no kernel tree needed).
- `code/gpu31/client/Makefile` — `make` / `make check` / `make
  check-aarch64` (static cross binary, qemu), same shape as the
  `kbase-winsys` and `probe` Makefiles.

## 2. Host test results (acceptance: build + mock tests pass)

Native `make check` and cross-under-qemu `make check-aarch64` both:

```
submit            40
wait_bo           16
create_bo         24
mmap_bo           16
get_param         16
get_bo_offset     16
T720 prod id       0x0720
-- pf7580 mock suite (v5.2 six-ioctl lifecycle) --
MOCK SUITE: PASS
```

Coverage (9 groups): T1 wire-size/field asserts at compile time; T2
GET_PARAM prod-id + bad-id/NULL; T3 CREATE (handle/offset nonzero,
size-0/NULL rejected); T4 MMAP/GET_BO_OFFSET round trip + unknown-handle
ENOENT; T5 CPU map write/read-back/persistence + bad-offset reject; T6
WAIT (0/1 s + unknown handle); T7 SUBMIT validation (null/unaligned `jc`,
bad requirements, >16 syncs, >64 BOs, NULL arrays, NULL args); T8 full
lifecycle submit→wait; T9 one-shot `EIO` injection on all six ioctls.
Device-absent path: `--device /nonexistent` → exit 2 with ABI dump.

## 3. Ready for the real node

On the phone (kernel #37, `/dev/dri` expected once the driver binds):

```
PF7580_DEVICE=/dev/dri/renderD128 ./build/pf7580-selftest --device /dev/dri/renderD128
```

expects `GPU_PROD_ID 0x0720 (T720)`, BO create/map R/W, `wait_bo(0)` —
exit 0. No reboot/flash/build on device; copy the static
`build-aarch64/pf7580-selftest` over and run under
`flock /tmp/gpsdev.lock`.
