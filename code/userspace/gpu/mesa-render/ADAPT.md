# Mesa render-node adaptation (j7elte / Mali-T720)

How the `kbase_kmod` backend from `patches/gpu-mesa-kbase/` (pan_kmod ops,
`dev_create`, `bo_alloc/mmap`, `vm_bind`, `submit_job`, sync provider) maps
onto a DRM render node (`/dev/dri/renderD128`), and what replaces it when the
render node speaks the panfrost UAPI instead of kbase UK.

Read-only references (do NOT modify): `patches/gpu-mesa-kbase/0001*` (backend:
`pan_kmod_ops`, `dev_create`, `bo_alloc/mmap`, `vm_bind`, `submit_job`, sync
provider), `0002` (JM context/submit neutrality), `0003` (skeleton),
`code/userspace/gpu/kbase-winsys/include/kbase_winsys.h` (API shape),
`code/userspace/gpu/kbase/kbase_uapi.h` (UK 10.6 wire ABI).

## 0. Source availability (honest status)

- Mesa sources (`27.0.0-devel @ 02f1337` per `apply.sh`) are **absent** on this
  host: no `~/mesa`, `/tmp/mesa`, `/opt/mesa` checkout. The full backend build
  below is therefore **pending-Mesa-source**; fetch + build commands are given
  in §5 so the integrator can run them verbatim.
- Present on host: `meson 1.12.1`, `gcc 16.2.1`, `aarch64-linux-gnu-gcc`,
  `libdrm 2.4.134` with `/usr/include/drm/panfrost_drm.h` (full JM UAPI:
  `SUBMIT 0x00` … `JM_CTX_CREATE 0x0a`, `JM_CTX_DESTROY 0x0b`,
  `drm_panfrost_submit`, `drm_panfrost_jm_ctx_create/destroy`, full
  `drm_panfrost_param` enum), binary Mesa `26.2.6` shipping
  `/usr/lib/dri/panfrost_dri.so`, and host render nodes
  (`/dev/dri/renderD128/129` exist on the build host, unrelated GPU).
- Device-side Mesa version (`apk list mesa*`) is device-side: the integrator
  runs it (command in §5). Nothing here touches the device.

## 1. The one decision that matters

There are two mutually exclusive render-node designs. Which one the
integrator's new DRM driver implements decides whether the kbase backend
applies at all:

- **H1 — render node speaks panfrost UAPI** (`DRM_IOCTL_PANFROST_*`:
  `GET_PARAM`, `CREATE_BO`, `MMAP_BO`, `GET_BO_OFFSET`, `SUBMIT`,
  `JM_CTX_CREATE/DESTROY`, …). Then the `kbase_kmod` backend **does NOT
  apply** — not adapted, not compiled in. What replaces it is **stock
  `panfrost_dri.so`** (the already-built `panfrost_kmod.c` /
  `panthor_kmod.c` DRM backends selected by `pan_kmod.c`'s backend table):
  zero Mesa patches needed for the winsys layer. Requirement: a Mesa whose
  panfrost supports Midgard/T720 — observed present in host Mesa 26.2.6
  (`/usr/lib/dri/panfrost_dri.so`); the `27.0.0-devel` tree from `apply.sh`
  builds the same driver. `0001`/`0002`/`0003` become unnecessary (0002's
  optional hooks simply stay NULL and the DRM ioctl paths run, which is
  exactly their `"Behaviour on the DRM backend is unchanged"` case).
- **H2 — render node tunnels kbase UK** (a shim exposing
  `/dev/dri/renderD128` whose ioctl surface is still the UK 10.6 in-buffer
  ABI: `UKP_FUNC_ID_CHECK_VERSION`, `KBASE_FUNC_SET_FLAGS`,
  `GPU_PROPS_REG_DUMP`, `STREAM_CREATE`, `JOB_SUBMIT`, SAME_VA `mmap`). Then
  the `kbase_kmod` backend applies with a **small fd-adoption change** (§3,
  shipped as `patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff`,
  dry-run verified against 0001's `kbase_kmod.c`).

Run `render-node-probe` (§4) on the device first: it classifies the node
into H1 / H2 / neither before any Mesa work.

## 2. Op-by-op mapping: kbase backend → render node

`dev_create` rejects non-kbase fds today: `kbase_kmod_dev_create` calls
`drmGetVersion(fd)` and returns NULL for any DRM fd *before* the kbase
handshake (`0001`, `kbase_kmod_dev_create`). Everything below follows from
which ioctl language the render node answers.

| `pan_kmod_ops` entry (0001 impl) | H1 (panfrost UAPI) | H2 (kbase UK over render node) |
|---|---|---|
| `dev_create` (`kbase_kmod_dev_create`: drmGetVersion-reject → `kbase_handshake` → `GPU_PROPS_REG_DUMP` → `STREAM_CREATE` → event thread) | **Not used.** `panfrost_kmod_ops.dev_create` owns the fd; props come from `DRM_IOCTL_PANFROST_GET_PARAM`. | **Change (one hunk).** Accept the DRM fd when it answers the UK handshake: keep the `drmGetVersion` early-out for real DRM drivers, but gate it behind `PAN_KMOD_DEV_FLAG_KBASE_SHIM` — i.e. try `kbase_handshake()` first-or-instead and only reject when the handshake fails. Today a shim fd is refused without ever being probed. See §3. |
| `dev_destroy` (`kbase_kmod_dev_destroy`) | N/A (DRM backend) | Identical. Same fd/timeline/thread teardown. |
| `dev_query_user_va_range` (= NULL) | N/A | Identical (still NULL). |
| `bo_alloc` (`kbase_kmod_bo_alloc`: `MEM_ALLOC` + immediate `mmap`, SAME_VA `gpu_va == cpu`) | N/A — `CREATE_BO` returns a GEM handle + `offset`; no CPU mapping at alloc time. | **Identical**, provided the shim preserves SAME_VA `mmap` semantics (cookie-as-offset `mmap`). If the shim instead returns GEM handles + `MMAP_BO` offsets, this op needs a rewrite to the `panfrost_kmod.c` shape — that half-shim is the worst case; prefer pure H1 or pure H2. |
| `bo_free` / `bo_mmap` (munmap / return cached mapping) | N/A | Identical. |
| `bo_wait` (always true; ordering via fences) | N/A | Identical. |
| `flush_bo_map_syncs` (no-op) | N/A | Identical. |
| `bo_import` (NULL) / `bo_export` (-1) — no dma-buf path on r15p0 | **Solved by H1**: stock DRM backend does PRIME import/export; WSI/scanout interop comes free. | **Identical stubs** (still NULL/-1). A UK-tunelling shim cannot add dma-buf prime by itself; display interop still needs a gralloc/kernel export path — out of scope, same as 0001 documents. |
| `vm_create`/`vm_destroy` (single VM per kbase ctx) | N/A — real `vm_bind` against the DRM VM. | Identical. |
| `vm_bind` (`kbase_kmod_vm_bind`: MAP only legal when `va == gpu_va` or AUTO_VA; UNMAP no-op; SPARSE rejected) | N/A | **Identical.** No MAP legality change: the address is still assigned at `mmap` time, so callers must still pass AUTO_VA/exact VA. Any caller passing explicit non-SAME_VA addresses was already failing and keeps failing. |
| `submit_job` (`kbase_kmod_submit_job`: `drm_panfrost_submit` → wait-atom + job-atom + fence-trigger-atom, `jc` passed through, `bo_handles` ignored) | N/A — `panfrost_kmod_submit()` issues the ioctl directly. | **Identical.** `jc` is a SAME_VA address either way; `bo_handles` still need no action. |
| `jm_ctx_create`/`jm_ctx_destroy` (handle 0, no-op) + 0002 hooks in `pan_jm.c` | N/A — 0002's hooks stay NULL, DRM ioctls run (unchanged behaviour). | Identical, no 0002 change: hooks already optional and NULL-tolerant. |
| sync provider (`kbase_sync_*`: table slots, handle 0 reserved, fence-fd in/out, wait/reset/signal, timeline wait/signal, query/transfer) | N/A — `drmSyncobj` provider. | Identical. |
| `query_timestamp` (0) / `perf_*` (NULL/-1) / `bo_set_label` (no-op) | N/A | Identical (still unsupported). |
| 0003 skeleton (`panfrost_kbase_skeleton.c`: open path → `panfrost_create_screen(fd)`) | Replaced by the normal DRI loader path (`panfrost_dri.so` + `panfrost_create_screen` on the render fd). | **No code change needed**: the path argument already accepts any node (`./panfrost-kbase-skeleton /dev/dri/renderD128`); only the usage string mentions `/dev/maliN` (§3, cosmetic hunk included). |

Net for H2: **one functional hunk** (`dev_create` fd adoption) + one cosmetic
hunk (skeleton usage). Every BO / VM / submit / fence op is byte-identical
because the UK ABI — not the device-node name — is what the backend speaks.

## 3. Minimal diff list (H2 only; H1 needs no diff at all)

Against `0001-panfrost-kmod-add-a-kbase-UK-10.6-backend.patch`:

- `src/panfrost/lib/kmod/kbase_kmod.c`, `kbase_kmod_dev_create`: replace the
  unconditional `drmGetVersion` → NULL rejection with handshake-first probing
  (shipped patch: try `kbase_handshake()` even when `drmGetVersion`
  succeeds; reject only when the handshake fails). All later setup
  (`GPU_PROPS_REG_DUMP`, `kbase_props_from_kernel`, `STREAM_CREATE`, event
  thread) is untouched.
- `src/panfrost/lib/kmod/kbase_kmod_test.c`: extend the selftest — a DRM fd
  that answers the UK handshake must now be *accepted* (shim case); a DRM fd
  that answers `drmGetVersion` and fails the handshake must still be rejected
  (real-DRM case, e.g. host `/dev/dri/card0`).
- No change: `pan_kmod.h` op signatures, `pan_kmod.c` backend table,
  `panfrost_kmod.c` dispatch, `meson.build` entries.

Against `0002-…-backend-neutral….patch`: **no change** (hooks optional).

Against `0003-…-skeleton….patch`:
`src/gallium/drivers/panfrost/panfrost_kbase_skeleton.c`, usage string
`/dev/maliN` → `/dev/maliN | /dev/dri/renderD128`. Functional code unchanged.

The shipped `patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff`
implements exactly the `kbase_kmod.c` hunk above against 0001's
`kbase_kmod.c`; it was verified with `patch -p1 --dry-run` from a Mesa-tree layout
(`mesa/src/panfrost/lib/kmod/kbase_kmod.c` reconstructed from 0001) (see §5). The test/skeleton hunks are one-liners
described here, not shipped, to avoid unverifiable churn.

## 4. What is delivered here (and what builds now)

- `code/userspace/gpu/mesa-render/ADAPT.md` (this file).
- `code/userspace/gpu/mesa-render/render-node-probe.c` — self-contained C
  (C99, only `<drm/drm.h>` + `<drm/panfrost_drm.h>` + libc; **no libdrm
  needed**, so it cross-builds with plain `aarch64-linux-gnu-gcc`). Given a
  node path it prints: `DRM_IOCTL_VERSION` driver name (H1 signal:
  `panfrost`); `PANFROST_GET_PARAM` answers for `GPU_PROD_ID`, `REVISION`,
  `SHADER/TILER/L2_PRESENT`, `MMU_FEATURES`, `SELECTED_COHERENCY` (H1
  confirmed); and the kbase `UKP_FUNC_ID_CHECK_VERSION` 10.6 handshake result
  (H2 signal; on a real DRM fd the kernel answers `ENOTTY`, harmless). Exit:
  `0` classified (prints `H1`/`H2`/neither), `2` no such device, `1` error.
- `code/userspace/gpu/mesa-render/Makefile` — `make` (native),
  `make CROSS_COMPILE=aarch64-linux-gnu-` (device binary),
  `make check` (native + `qemu-aarch64` smoke, no hardware).

## 5. Build / verify commands (integrator)

Mesa fetch (sources absent on host — run once):

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git /tmp/mesa
cd /tmp/mesa && git checkout 02f1337   # 27.0.0-devel era per apply.sh
/path/to/patches/gpu-mesa-kbase/apply.sh /tmp/mesa
```

Backend build (flags from `apply.sh`):

```sh
meson setup /tmp/mesa/build-kbase -Dgallium-drivers=panfrost -Dvulkan-drivers= \
  -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled \
  -Dpanfrost-rust=false -Dbuild-tests=true
ninja -C /tmp/mesa/build-kbase src/panfrost/lib/libpankmod_lib.a \
  src/gallium/drivers/panfrost/libpanfrost.a \
  src/panfrost/lib/kbase-kmod-selftest
/tmp/mesa/build-kbase/src/panfrost/lib/kbase-kmod-selftest
# exit 2 = no /dev/mali0 (device checks skipped), 0 = pass, 1 = fail
```

Adapt-patch dry-run (no Mesa tree needed; reconstructs 0001's file):

```sh
cd /tmp && rm -rf mr-check && mkdir mr-check && cd mr-check
sed -n '/^+++ b\/src\/panfrost\/lib\/kmod\/kbase_kmod.c$/,/^diff --git/p' \
  /tmp/j7repo/patches/gpu-mesa-kbase/0001-*.patch | head -n -1 \
  | sed 's/^+//' | tail -n +2 > kbase_kmod.c
patch -p1 --dry-run < /tmp/j7repo/patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff
```

Probe build + classify (host smoke, then device):

```sh
make -C /tmp/j7repo/code/userspace/gpu/mesa-render        # native
make -C /tmp/j7repo/code/userspace/gpu/mesa-render check  # native smoke
make -C /tmp/j7repo/code/userspace/gpu/mesa-render CROSS_COMPILE=aarch64-linux-gnu- clean all check-qemu
make -C /tmp/j7repo/code/userspace/gpu/mesa-render CROSS_COMPILE=aarch64-linux-gnu- clean all
./render-node-probe /dev/dri/renderD128    # on device (integrator)
apk list mesa\*                              # on device (integrator)
```

H1 follow-up (no Mesa patches): point `panfrost_dri.so` at the render node
via the normal DRI/GBM loader and run the glmark harness
(`code/userspace/gpu/glmark-harness/`); H2 follow-up: apply the §3 diff on
top of `apply.sh` output and rebuild.
