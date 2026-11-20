# GPU31 Mesa 20.x — period-correct build vs panfrost v5.2, device result

Date: 2026-11-20. Slice: G31_Mesa20.
Device: SM-J700M (j7elte), Nura Alpine aarch64, kernel 3.10.108
(#37 render-only boot, Xorg healthy), `/dev/dri/renderD128` present.
Related: `docs/reports/gpu31-mesa.md` (kbase-backend era recipe),
`docs/reports/gpu31-target.md` (v5.2 H1 lock), `docs/reports/gpu31-gem.md`,
`docs/reports/gpu31-client.md` (six-ioctl selftest).

## 0. Verdict (honest)

The renderer string is **NOT yet `Mali-T720`** — eglinfo with our 20.3.5
build and `MESA_LOADER_DRIVER_OVERRIDE=panfrost` does not reach GL
screen creation. Two stacked blockers were isolated, neither in Mesa's
GL code:

1. **BEATEN (test-only workaround):** Mesa 20.3.5 allocates a fixed
   16 MB tiler heap at device open (`4096*4096` in
   `src/panfrost/lib/pan_props.c:231`); the port's CMA-backed
   CREATE_BO tops out between 4–8 MB (4 MB OK, 8 MB ENOMEM), so the
   very first BO fails. Local 1 MB test-only patch applied on-device
   (clearly marked, see §4).
2. **OPEN (kernel):** after (1), `panfrost_create_context`
   (`pan_context.c:1632-1633`) asserts on `drmSyncobjCreate` —
   strace shows `DRM_IOCTL_SYNCOBJ_CREATE = -1 EINVAL`. The port wires
   syncobj *objects* for SUBMIT in/out fences but does **not** expose
   the generic DRM syncobj ioctls (`CREATE/DESTROY/HANDLE_TO_FD/
   FD_TO_HANDLE/WAIT/SIGNAL/RESET`), which stock 20.3.5 expects for
   context creation. Blocker owned by G31_GemShmem / driver slices.

No GPU work was ever submitted (all probes are GET_PARAM/CREATE_BO/
MMAP_BO only, each under `timeout 20`); the SoC never wedged during
this slice. dmesg after every probe shows only routine `p7580_gem_mmap`
+ fuel-gauge lines — no submit/fence/PM warnings.

## 1. Why Mesa 20.3.5

Target was "Mesa 19.3–20.x era". 20.3.5 (tag `94d6de9`, Dec 2020) is
the last 20.x and pairs cleanly with the v5.2 kernel contract:

- `include/drm-uapi/panfrost_drm.h` SUBMIT struct is the exact 40-byte
  v5.2 shape (`jc/in_syncs/in_sync_count/out_sync/bo_handles/
  bo_handle_count/requirements`, no `jm_ctx_handle` tail).
- GET_PARAM: only `GPU_PROD_ID` is `required=true`; every other param
  (`SHADER_PRESENT`→`0xffff` default, `THREAD_TLS_ALLOC`, texture
  features) degrades gracefully to compiled-in defaults on `-EINVAL`
  (`src/panfrost/lib/pan_props.c:panfrost_query_raw`). Our kernel
  answers only `GPU_PROD_ID` — fully sufficient for T720 bring-up.
- CREATE_BO flag use (`NOEXEC`/`HEAP`) is version-gated:
  `pan_bo.c:65` sets them only when `drmGetVersion >= 1.1`. Our
  driver reports 1.0 (`panfrost7580`, `major=1, minor=0`), so stock
  20.3.5 sends `flags=0` — accepted by our strict CREATE_BO.
  (Post-1.1 kernels: GROWABLE→HEAP, non-EXECUTE→NOEXEC.)
- requirements: batches submit `0` (vertex/tiler) or
  `PANFROST_JD_REQ_FS` (fragment) — exactly the v5.2 gate.
- MADVISE calls exist in the BO-cache paths (`pan_bo.c:207-276`) but
  their return values are ignored (WILLNEED-miss → free + realloc,
  DONTNEED fire-and-forget) — safe against our `-EINVAL`.
- Driver name resolution: `MESA_LOADER_DRIVER_OVERRIDE` short-circuits
  `loader_get_driver_for_fd` (`src/loader/loader.c:477-481`), so the
  value `panfrost` maps to `panfrost_dri.so` regardless of the
  kernel's `panfrost7580` name. Proven below: without the override the
  loader looks for `panfrost7580_dri.so` and fails; with it, it loads
  our build. This is stock-Mesa behaviour, no patch needed.

## 2. Build (on-device native, aarch64 musl)

Host cross-compile was rejected: the available sysroot is glibc
(`aarch64-linux-gnu-gcc`) but the device is musl; there is no musl
cross toolchain on the build host. Native on-device build precedent
exists (`/data/build/xf86-video-fbdev-0.5.0` autotools log).

- Source: `mesa-20.3.5.tar.xz` (18 MB) from host git
  (`git fetch --depth 1 ... mesa-20.3.5`), extracted to
  `/data/mesabuild/mesa20` (140 MB). Build dir `/data/mesabuild/mesa20-b`.
- Deps installed via apk (root): `meson ninja gcc g++ musl-dev
  pkgconfig python3 py3-mako py3-setuptools flex bison`,
  plus `libxrandr-dev libxshmfence-dev` after the first configure miss.
- Configure (RC=0):
  `meson setup mesa20 mesa20-b -Dgallium-drivers=panfrost
  -Ddri-drivers= -Dvulkan-drivers= -Dplatforms=x11 -Dglx=dri
  -Degl=enabled -Dgbm=enabled -Dopengl=true -Dgles1=enabled
  -Dgles2=enabled -Dllvm=disabled -Dshared-llvm=disabled
  -Dbuild-tests=false -Ddri3=disabled -Dgallium-omx=disabled`
  (`-Dbuild-tests=false` because the glsl `list_iterators` gtest trips
  on gcc 15 headers — test-only, unrelated to the driver.)
- Build: `ninja -C mesa20-b` → full link
  `src/gallium/targets/dri/libgallium_dri.so` (75 MB),
  `src/egl/libEGL.so.1.0.0`, `src/gbm/libgbm.so.1.0.0`,
  `src/glx/libGL.so.1.2.0`.
- Build-log quirks (all recovered, none in the final binaries):
  the mid-build device outage left zero-byte `capture:true` generated
  headers (`nir_intrinsics.h`, `nir_builder_opcodes.h`,
  `nir_opt_algebraic.c`) and archives (`libxmlconfig.a`,
  `libglcpp.a`, `libcompiler.a` members); each was regenerated
  (ran the `.py` generator manually) or deleted and relinked.
  `ninja -t targets` reported the files "built" because mtime was
  newer than the outage — manual regen was the fix.

## 3. Device output (verbatim)

Staging: `LIBGL_DRIVERS_PATH=/data/mesabuild/dri-test` containing
`panfrost_dri.so -> .../libgallium_dri.so`;
`LD_LIBRARY_PATH=$B/src/egl:$B/src/gbm:$B/src/glx:
$B/src/mapi/shared-glapi`.

Without override (loader uses kernel name `panfrost7580`):

```
libEGL warning: MESA-LOADER: failed to open panfrost7580:
  Error loading shared library .../panfrost7580_dri.so: No such file ...
eglinfo: eglInitialize failed
```

With `MESA_LOADER_DRIVER_OVERRIDE=panfrost` (16 MB heap, before §4):

```
GBM platform:
DRM_IOCTL_PANFROST_CREATE_BO failed: Out of memory
BO creation failed
eglinfo: eglInitialize failed
Surfaceless platform:
DRM_IOCTL_PANFROST_CREATE_BO failed: Out of memory
BO creation failed
.../pan_bo.c:388:panfrost_bo_create: Assertion `bo' failed.
Aborted
```

With 1 MB test heap (§4), surfaceless:

```
Surfaceless platform:
Assertion failed: !ret && ctx->syncobj
  (.../pan_context.c: panfrost_create_context: 1633)
Aborted
```

strace (`-e trace=ioctl`) of that run — driver loads, GET_PARAM OK,
BOs OK, syncobj CREATE fails:

```
ioctl(5, ...DRM_IOCTL_PANFROST_GET_PARAM...) = 0        # GPU_PROD_ID
ioctl(5, ...DRM_IOCTL_PANFROST_GET_PARAM...) = -1 EINVAL # later params, defaults used
ioctl(5, ...DRM_IOCTL_PANFROST_CREATE_BO...) = 0        # x2 (1 MB heap + pool)
ioctl(5, ...DRM_IOCTL_PANFROST_MMAP_BO...) = 0
ioctl(5, DRM_IOCTL_SYNCOBJ_CREATE, ...) = -1 EINVAL    # <-- blocker (2)
```

So the requirement "report whether the renderer string becomes
Mali-T720" is answered **NO (not yet)** — eglinfo aborts before any GL
string exists. Vendor would be `Panfrost` / device vendor `Arm`
(`pan_screen.c:79-87`), model `Mali T720 (Panfrost)`
(`pan_props.c:187`), once context creation succeeds.

## 4. Safe isolation probes (no submits, all `timeout 20`)

ioctl-only C probe (`/data/mesabuild/dri-test/bosize`, kept on device):

```
GET_PARAM prod_id: r=0 value=0x720
CREATE_BO size=4096:    r=0 handle=1 offset=0x2000000
CREATE_BO size=65536:   r=0 handle=2 offset=0x2001000
CREATE_BO size=1048576: r=0 handle=3 offset=0x2011000
CREATE_BO size=4194304: r=0 handle=4 offset=0x2111000
CREATE_BO size=8388608:  r=-1 errno=1
CREATE_BO size=16777216: r=-1 errno=1
```

dmesg on the 16 MB attempt: `__alloc_pages_nodemask` warning via
`swiotlb_alloc_coherent <- __dma_alloc_noncoherent <-
p7580_gem_create` — contiguous dma_alloc_attrs cannot serve ≥8 MB.

Test-only workaround (device-local, NOT committed anywhere — the repo
has no mesa20 packaging yet, see §6):

```c
/* pan_props.c:231, was 4096 * 4096 */
dev->tiler_heap = panfrost_bo_create(dev, 1024 * 1024,
  /* G31_Mesa20 TEST-ONLY: 16MB->1MB, kernel CMA caps at 4-8MB;
     real fix is shmem GEM (G31_GemShmem) */
  PAN_BO_INVISIBLE | PAN_BO_GROWABLE);
```

Original saved as `pan_props.c.orig` beside it. Rebuild was 4 targets
(`build2.log`).

## 5. Next step

(a) **Real fix — G31_GemShmem (spawned by Main):** back CREATE_BO with
shmem/page-cache (`drm_gem_shmem`-equivalent for 3.10) for general BOs;
keep CMA only where WC/scanout needs it. Panfrost upstream uses
shmem-backed GEM — that is why stock Mesa assumes a 16 MB heap works.
Needs a new image + flash before any real submit. Tiler heap stays
16 MB in stock Mesa; no Mesa patch should be needed once shmem lands.
(b) **Syncobj ioctls (for the driver slices):** expose generic DRM
`SYNCOBJ_CREATE/DESTROY/HANDLE_TO_FD/FD_TO_HANDLE` (at minimum CREATE;
WAIT/SIGNAL/RESET as Mesa starts using timeline fences) or document
why not. Stock 20.3.5 `panfrost_create_context` hard-asserts on
CREATE — unpatched Mesa can never init EGL without it.
(c) After (a)+(b): re-run `eglinfo -p surfaceless -B` with the
*unpatched* 16 MB heap under `timeout 20`; expect
`EGL vendor: Mesa Project`, client renderer `Panfrost` /
`Mali T720 (Panfrost)`. Then glmark/es2_info offscreen only — still no
GPU submits until the PM-ordering fix lands.

## 6. Files / reproducibility

- On device (kept): `/data/mesabuild/mesa20` (source + 1-line test
  patch + `.orig`), `/data/mesabuild/mesa20-b` (build),
  `setup.log` (configure RC=0), `build.log` + `build2.log`,
  `/data/mesabuild/dri-test/` (`panfrost_dri.so` symlink, `bosize`
  + source, `getstr2` EGL probe + source, ioctl traces).
- In repo: this file only (`docs/reports/gpu31-mesa20.md`). No Mesa
  packaging/patches committed — the build is on-device native and the
  one-line heap change is explicitly test-only. If Main wants it
  reproducible, the follow-up is a `code/gpu31/mesa20/` recipe
  (fetch tag + apk list + meson flags) plus the shmem kernel design.
- Smallest prebuilt option (not used — native build succeeded): none
  exists for musl + panfrost-20.3; Alpine's `mesa-26.3` pairs with a
  far newer panfrost UAPI and cannot bind this node
  (`MESA_LOADER_DRIVER_OVERRIDE=panfrost eglinfo` on system Mesa:
  `failed to create dri2 screen`).
