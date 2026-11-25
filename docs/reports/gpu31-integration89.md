# integration89 — panfrost DRM 1.1 + Mesa 26 GET_PARAM/flags uapi

int89 = int88 + `code/gpu31/driver/0138-panfrost7580-mesa26-uapi.patch`.

## Why Mesa 26 refused this driver

Mesa's panfrost/kmod backend (the local 27.0-devel tree is the same code)
selects the driver by `drmGetVersion().name == "panfrost"` and then:

```c
if (!pan_kmod_driver_version_at_least(&drv_info, 1, 1)) {
   mesa_loge("kernel driver is too old (requires at least 1.1, found %d.%d)", ...);
   return NULL;
}
```

so int88's `1.0` is rejected before any ioctl.  It then queries a required
feature set through `DRM_IOCTL_PANFROST_GET_PARAM` (a failed required query is
`assert(!required)` in debug builds and zeros in release builds, which breaks
the compiler) and passes modern CREATE_BO flags.

**Version choice: 1.1 exactly, not higher.**  Mesa adds behaviour at 1.3
(SYSTEM_TIMESTAMP_FREQUENCY), 1.4 (SET_LABEL_BO), 1.6 (QUERY_BO_INFO, and
switches the import path to it; supported_bo_flags gains WB_MMAP).  This
driver's ioctl table implements none of those, so 1.1 is the highest version
that does not make Mesa call an unimplemented ioctl.

## Changes (driver 0138)

- `.major/.minor = 1/1` in `p7580_drm_driver`.
- Probe feature snapshot (`p7580_capture_features`, block powered there),
  with the mainline `panfrost_gpu_init_features()` offsets: GPU_ID,
  L2/CORE/TILER/MEM/MMU features, AS/JS present, THREAD_FEATURES,
  MAX_THREADS, max workgroup/barrier, COHERENCY_FEATURES, AFBC_FEATURES
  (0x4C; Bifrost register, reads 0 on Midgard), TEXTURE_FEATURES0..3,
  JS_FEATURES per present slot, SHADER/TILER/L2/STACK_PRESENT (LO|HI<<32),
  THREAD_TLS_ALLOC, nr_core_groups = hweight64(l2_present).  The two
  pre-existing mislabelled macros (L2 at 0x02C, CORE at 0x030 — actually
  GPU_INT_STAT/GPU_CMD) are corrected to 0x004/0x008.
- `p7580_drv_get_param()` answers every one of those ids from the snapshot;
  1.3/1.5/1.6-only ids stay -EINVAL (Mesa gates them on the version).
  GET_PARAM logs id, value and rc for every query (`p7580_get_param:`), so
  the device run shows exactly what Mesa 26 sends.  `pad != 0` is still
  rejected.
- CREATE_BO: `size == 0` and `pad != 0` rejected; `PANFROST_BO_NOEXEC` (1)
  and `PANFROST_BO_HEAP` (2) accepted (backing is pre-faulted SHMEM, so they
  are hints), `PANFROST_BO_WB_MMAP` (4) accepted as a no-op, unknown bits
  warn-ignored instead of -EINVAL.  One enter log prints flags plus
  sizeof/flags/pad/offset offsets (uapi layout evidence).
  MMAP_BO flags are warn-ignored (currently no meaning).
- uapi `panfrost_drm.h`: enum `drm_panfrost_param` extended in modern
  upstream order through `SELECTED_COHERENCY` (GPU_PROD_ID remains 0);
  `PANFROST_BO_*` defines added; struct layouts unchanged
  (create_bo is 24 B: size 0, flags 4, handle 8, pad 12, offset 16).

## What Mesa 26/27 will ask for (from its source)

Required GET_PARAM (all answered from the snapshot now):
GPU_PROD_ID (0), GPU_REVISION (1), SHADER_PRESENT (2), TILER_FEATURES (10),
MEM_FEATURES (11), MMU_FEATURES (12), L2_FEATURES (8), TEXTURE_FEATURES0..3
(18..21), AFBC_FEATURES (40), MAX_THREADS (14), THREAD_MAX_WORKGROUP_SZ (15),
THREAD_FEATURES (13), THREAD_TLS_ALLOC (38).  At version 1.1 Mesa skips
SYSTEM_TIMESTAMP_FREQUENCY (1.3), SET_LABEL_BO (1.4) and QUERY_BO_INFO (1.6;
imports keep using GET_BO_OFFSET).  ALLOWED_JM_CTX_PRIORITIES (1.5) and
SYSTEM_TIMESTAMP are queried non-required; -EINVAL there is expected and
handled.

CREATE_BO flags Mesa 1.1 sends: `PANFROST_BO_NOEXEC` for every non-executable
BO and `PANFROST_BO_HEAP` for the tiler heap (both set for the heap), i.e.
values 1 and 3 today; the old blanket `if (args->flags) return -EINVAL` would
have failed every BO creation.

## Gates (all PASS)

int88 set plus `int89 gate: panfrost DRM 1.1 + Mesa 26 GET_PARAM/flags uapi`:
minor == 1 inside `p7580_drm_driver`; modern param enum (AFBC +
SELECTED_COHERENCY present); NOEXEC/HEAP/WB_MMAP defines; capture wired and
the register offsets 0x004/0x014/0x04C checked; GET_PARAM call path uses the
snapshot; CREATE_BO no longer rejects unknown flags; vmlinux contains the
`p7580_get_param:` and `p7580_features:` strings.  All earlier gates
(llseek slot, FMODE_LSEEK, DRM_RDWR, blob lifetime, fbdev order, rename,
panfrost built-in, blobs/MODE_ID, PRIME, two .ko) unchanged.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int89` | 18191112 | `36f20590277e6a18fbc652d2e14d11984586ec5f5aeddc978b68680cbb544b13` |
| `kernel-boot-int89.img` | 27330560 | `8b14288306da6d3560ee40f82c2456f62c2ce8001ffc97a45d9fc492cff97b6b` |
| `exynos-drm7580-int89.ko` | — | `5c441907fac0027a9ad12e7c613fc244cdeb43e9d00edeeb9b93e110920f51db` |
| `drm_kms_helper-int89.ko` | — | `31220713fa1d2254548ce9a34d43616848925a7ba7d5a0dbc795af721737f0c5` |

Build log `int89-kms-build.log`: 0 errors.  Log sha256s: build `f5d98aae9513e4f46bc4491c251d7dee3ffefc5dbc9936de93439eca3b1d7d6a`, gates
`20ccdbe3cb352aae16fbed278666c19043addcc47a5aef040061c3e0347d8b68`, evidence `822253cc90fe79b3bea9b270a86f0077dc414f1eb435c5b05f932bd25ca6f63f`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Device acceptance

Flash with the two int89 .ko, then (system Mesa, no mesa20 env):

- `dmesg | grep p7580_features` — probe snapshot: expect gpu_id 0x0720xxxx
  (product 0x720 in [31:16] and the part's revision in [15:0]),
  shader_present 0x3 for MP2, l2_present 0x1, js 0x7, plus the L2/CORE/TILER/
  MEM/MMU/thread feature values that Mesa will consume;
- `mesaprobe --gbm /dev/dri/card0` and `kmscube -D /dev/dri/card0` with the
  system Mesa: renderer `Mali T720 (Panfrost)`, no "kernel driver is too old";
- `dmesg | grep p7580_get_param` — the exact ids Mesa asks for; every
  `rc=0` id above should appear, and only 1.3/1.5/1.6 ids may log rc=-11;
- `dmesg | grep p7580_create_bo` — expected flags 0x1 (NOEXEC) for ordinary
  BOs and 0x3 (NOEXEC|HEAP) for the tiler heap; layout line shows
  sizeof=24 flags_off=4 pad_off=12 offset_off=16;
- then kwin_wayland with the system Mesa.
