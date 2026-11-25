# DECON KMS ↔ Mesa pairing: why `kmscube -D /dev/dri/card0` renders on llvmpipe

Status 2026-11-25. Owner slice: `MesaPair`.

**Measured on the phone** (`Linux nura-j7elte 3.10.108 #37`), no flash, no reboot,
no kernel change. The render-node half of the matrix ran for real; the `card0`
half is **blocked on this boot** because the int82 KMS module pair cannot be
loaded into kernel `#37` (see §5, evidence: vermagic mismatch), and per
`G31_Integrator41` the int82 `.ko` pair must not be force-loaded into a
different build (struct layout ABI). The exact one-boot commands that finish
the matrix after the next paired image are in §7.

Tools: `code/gpu31/kms/tools/mesa/` (`mesaprobe.c`, `drmshim.c`, `Makefile`,
`run-matrix.sh`).

---

## 1. Verdict

`llvmpipe` is not a Mesa bug and not a gbm-backend problem: **kmsro pairs the
KMS device with a render device purely by the kernel driver name, and our
render node is called `panfrost7580`, not `panfrost`.**

| Gate | What Mesa compares | Our value | Needed | Fixable by env? |
|---|---|---|---|---|
| 1. DRI module for the KMS fd | `drmGetVersion(card0)->name` → `<name>_dri.so` | `exynos` | `exynos` | n/a — **already correct** |
| 2. kmsro render-device search (Mesa 20) | `drmOpenWithType("panfrost", DRM_NODE_RENDER)` exact name match on every render node | `panfrost7580` | `panfrost` | **no** |
| 2'. kmsro render-device search (Mesa 26) | platform render nodes whose `drmGetVersion()->name` ∈ {`panfrost`, `panthor`, `lima`, `msm`, `etnaviv`, `v3d`, `vc4`, `asahi`, `rocket`, `ethosu`, `zink`} | `panfrost7580` | `panfrost` | **no** |
| 3. panfrost screen on the render node (Mesa 26 only) | `pan_kmod_drm_drv_match(fd,"panfrost")` **and** DRM version ≥ 1.1 | `panfrost7580`, 1.0 | `panfrost`, ≥1.1 | name via gate 2'; version no |
| 4. panfrost ioctl ABI (Mesa 26 only) | `PANFROST_CREATE_BO` with the 6.x argument set | 3.10-era ABI → `EINVAL` | newer ABI | no |

Mesa 20.3.5 (`/data/mesabuild/mesa20-b`) stops at gate 2 and has **no** kmsro
driver compiled in at all, so it cannot be the pairing side either.

**Minimal fix:** change the render driver's `drm_driver.name` from
`"panfrost7580"` to `"panfrost"` (`code/gpu31/driver/0001-panfrost7580-driver-sources.patch:5856`;
the platform driver name at `:6301` may stay). The KMS side already answers to
`exynos` (`code/gpu31/kms/exynos_drm7580_main.c:56`, verified as the literal
`exynos` in the built `exynos-drm7580-int82.ko`). No environment variable and no
Mesa build flag can substitute for that name.

---

## 2. What is installed (measured)

```
/usr/lib/dri/exynos_dri.so    -> libdril_dri.so   (megadriver)
/usr/lib/dri/panfrost_dri.so  -> libdril_dri.so
$ strings libdril_dri.so | grep __driDriverGetExtensions_ | grep -c .   # exynos + panfrost present
mesa-26.3.0-r0, libdrm-2.4.134-r0, kmscube-0_git20220902-r1
```

`exynos_dri.so` **is** the kmsro entry point (kmsro + panfrost compiled in),
`panfrost_dri.so` is the render driver. So the stock Mesa on the image is
already the right architecture: *exynos kmsro → panfrost render node*.

Mesa 20.3.5 diagnostic build (`/data/mesabuild/mesa20-b`, from
`intro-buildoptions.json`):

```
gallium-drivers = ['panfrost']     <-- no 'kmsro' -> no exynos_dri.so
platforms       = ['x11']
llvm            = disabled
src/gallium/targets/dri/: libgallium_dri.so  panfrost_dri.so  swrast_dri.so
                          (no exynos_dri.so, no kms_swrast_dri.so)
```

In Mesa 20.3.5 `meson.build:219` `with_gallium_kmsro = gallium_drivers.contains('kmsro')`,
so exynos/kmsro can only appear with `-Dgallium-drivers=panfrost,kmsro`.

Device names actually reported by the kernel:

```
/dev/dri/renderD128: drmGetVersion -> panfrost7580 "Mali-T720 render-only (Exynos 7580)"
                     drmDevice bustype = DRM_BUS_PLATFORM (2)
                     DUMB_BUFFER=0  PRIME=import|export
/dev/dri/card0 (int82): .name = "exynos" (source + string in the .ko)
```

---

## 3. Mesa decision points (source)

### 3.1 Which DRI module is opened for the KMS fd

Mesa 20.3.5 `src/loader/loader.c:468-500` (same logic in 26.3): the driver name
is `MESA_LOADER_DRIVER_OVERRIDE` → `drirc` `dri_driver` → PCI id map → **the
kernel driver name**; then `loader_open_driver()` (`:529-570`) dlopens
`<name>_dri.so`. Then gbm:

```c
/* Mesa 20.3.5 src/gbm/backends/dri/gbm_dri.c:1386-1401 */
ret = dri_screen_create(dri);          /* <name>_dri.so, then "zink" in 26.x */
if (ret) ret = dri_screen_create_sw(dri);   /* "kms_swrast", then "swrast" */
```

Mesa 26.3 also inserts a zink attempt (`gbm_dri.c:273-302`) and its sw fallback
is `kms_swrast` → `swrast`; the final `swrast` renderer is **llvmpipe**. That is
exactly the observed line.

### 3.2 kmsro: which render device is accepted

Mesa 20.3.5 `src/gallium/winsys/kmsro/drm/kmsro_drm_winsys.c:90-101`:

```c
ro.gpu_fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
if (ro.gpu_fd >= 0) {
   ro.create_for_resource = renderonly_create_kms_dumb_buffer_for_resource;
   screen = panfrost_drm_screen_create_renderonly(&ro);
```

`drmOpenWithType()` walks `/dev/dri/renderD*` and compares
`drmGetVersion(fd)->name` for **string equality** (measured below). There is no
bus matching, no busid fallback: the render node must be named `panfrost`.

Mesa 26.3 `src/gallium/winsys/kmsro/drm/kmsro_drm_winsys.c:60-150`:

```c
gpu_fds = pipe_loader_get_compatible_render_capable_device_fds(kms_fd, &n);
...
render_dev_name = loader_get_kernel_driver_name(ro->gpu_fd);
if (strcmp(render_dev_name, "panfrost") == 0 || strcmp(render_dev_name, "panthor") == 0)
   screen = panfrost_drm_screen_create_renderonly(...);
```

and `pipe_loader_get_compatible_render_capable_device_fds()` →
`loader_open_render_node_platform_devices()` (`src/loader/loader.c`), which
requires `device->bustype == DRM_BUS_PLATFORM` **and** the render node's name to
be in the static driver list (`panfrost`, `panthor`, `lima`, `msm`, `etnaviv`,
`v3d`, `vc4`, `asahi`, `rocket`, `ethosu`, `zink`). Our node is platform bus
(good) but named `panfrost7580` (rejected).

Then panfrost's kmod adds gates 3/4 (`src/panfrost/lib/kmod/panfrost_kmod.c`):

```c
if (!pan_kmod_drm_drv_match(fd, "panfrost", &drv_info))  return NULL;      /* name */
if (!pan_kmod_driver_version_at_least(&drv_info, 1, 1)) {                  /* version */
   mesa_loge("kernel driver is too old (requires at least 1.1, found %d.%d)", ...);
```

Our render driver is `.name = "panfrost7580", .major = 1, .minor = 0`
(`code/gpu31/driver/0001-panfrost7580-driver-sources.patch:5856-5861`).

### 3.3 What kmsro needs from the scanout driver

`renderonly_create_kms_dumb_buffer_for_resource()` (Mesa 26.3
`src/gallium/auxiliary/renderonly/renderonly.c:66-115`): `CREATE_DUMB` on the
KMS fd, then `drmPrimeHandleToFD()` on the KMS fd, then panfrost imports that fd
on the render node; rendering happens on the render GPU into that (CMA, physically
contiguous) buffer. The client then creates the scanout fb on the KMS node from
the gbm BO. Our driver already provides all four primitives:

| Requirement | Our driver | Where |
|---|---|---|
| `CREATE_DUMB` / `DUMB_MAP` / `DESTROY_DUMB` | yes (`drm_gem_cma_dumb_create` …) | `exynos_drm7580_main.c:404-406` |
| `PRIME` handle→fd export (renderonly needs it) | yes, `DRIVER_PRIME` + `.gem_prime_export = drm_gem_prime_export` | `exynos_drm7580_main.c:390,399` |
| `PRIME` fd→handle import (clients that pass fds) | yes, `exynos7580_gem_prime_import` | `exynos_drm7580_dmabuf.c:495+` |
| `ADDFB2` flags=0 with a GEM handle | yes, `drm_gem_object_lookup()` | `exynos_drm7580_dmabuf.c:580-607` |
| `IN_FORMATS` + modifiers | **no** — non-zero `flags` rejected, no blob | `exynos_drm7580_dmabuf.c:43-45,143` |
| vblank / page-flip event | present but the flip carries no fb | open int83 item |

Consequences:
* `kmscube` (20220902) tries `AddFB2WithModifiers` and, on failure, retries
  `drmModeAddFB2` with `flags = 0` (`kmscube drm-common.c:112`), so the missing
  `IN_FORMATS` does **not** block kmscube.
* `kmscube` builds the fb from `gbm_bo_get_handle()`; with renderonly that is
  the **KMS dumb handle**, so the stock kmscube path does not call
  `PRIME_FD_TO_HANDLE` on `card0` at all. `dmesg | grep fd_to_handle` is
  therefore *not* a reliable "kmsro paired" signal for kmscube — the reliable
  signals are `CREATE_DUMB` + `prime_handle_to_fd` on `card0` and the renderer
  string. (Weston/KWin do pass dma-buf fds and will need the import path.)

---

## 4. Measured matrix (this boot, render node only)

Cell names are `run-matrix.sh` cells; the renderer line is the raw
`glGetString(GL_RENDERER)` from `mesaprobe --gbm|--default`.

| # | Cell | Mesa | Node | Env | Renderer |
|---|---|---|---|---|---|
| 1 | `sys-render-noovr` | system 26.3.0 | renderD128 | — | `llvmpipe (LLVM 23.1.5, 128 bits)` |
| 2 | `sys-render-ovr-panfrost` | system 26.3.0 | renderD128 | `MESA_LOADER_DRIVER_OVERRIDE=panfrost` | `llvmpipe (LLVM 23.1.5, 128 bits)` |
| 3 | `sys-render-gbm-shim` | system 26.3.0 | renderD128 | name shim `panfrost7580=panfrost` | `llvmpipe (LLVM 23.1.5, 128 bits)` + `kernel driver is too old (requires at least 1.1, found 1.0)` |
| 4 | `sys-render-gbm-ovr-exynos` | system 26.3.0 | renderD128 | `MESA_LOADER_DRIVER_OVERRIDE=exynos`, `LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug` | `llvmpipe (LLVM 23.1.5, 128 bits)`; log: only `ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)` — kmsro never even reached panfrost |
| 5 | `sys-render-gbm-ovr-exynos-shim` | system 26.3.0 | renderD128 | same + name shim | `llvmpipe (LLVM 23.1.5, 128 bits)`; log: **`kernel driver is too old (requires at least 1.1, found 1.0)`** — proof kmsro's render-node search accepted the node once it is named `panfrost` |
| 6 | `sys-render-gbm-ovr-exynos-shimver` | system 26.3.0 | renderD128 | same + `DRM_VER_SHIM=1.1` | `llvmpipe (LLVM 23.1.5, 128 bits)`; log: `DRM_IOCTL_PANFROST_CREATE_BO failed (err=22)` — gate 4 |
| 7 | `sys-render-surfaceless` | system 26.3.0 | renderD128 | `EGL_PLATFORM=surfaceless` | `llvmpipe (LLVM 23.1.5, 128 bits)` |
| 8 | `sys-render-surfaceless-ovr-panfrost` | system 26.3.0 | renderD128 | + `MESA_LOADER_DRIVER_OVERRIDE=panfrost` | `llvmpipe (LLVM 23.1.5, 128 bits)` |
| 9 | `sys-render-surfaceless-shim` | system 26.3.0 | renderD128 | + name shim | `llvmpipe (LLVM 23.1.5, 128 bits)` + `kernel driver is too old` |
| 10 | `m20-render-noovr` | mesa20-b 20.3.5 | renderD128 | — | `<none>`: `gbm_create_device` FAILED — `failed to open panfrost7580` and no `kms_swrast_dri.so` in that build |
| 11 | `m20-render-ovr-panfrost` | mesa20-b 20.3.5 | renderD128 | `MESA_LOADER_DRIVER_OVERRIDE=panfrost` | **`Mali T720 (Panfrost)`** (gbm surface + swap OK) |
| 12 | `m20-render-shim` | mesa20-b 20.3.5 | renderD128 | name shim only (no override) | **`Mali T720 (Panfrost)`** |
| 13 | `m20-render-kmsro` | mesa20-b 20.3.5 | renderD128 | `MESA_LOADER_DRIVER_OVERRIDE=exynos` + shim | `<none>`: `failed to open exynos` / `exynos_dri.so: No such file` — kmsro is not in this build |
| 14 | `m20-render-surfaceless-ovr-panfrost` | mesa20-b 20.3.5 | renderD128 | `EGL_PLATFORM=surfaceless` + override | **`Mali T720 (Panfrost)`** |
| 15 | `render-opens` (real names) | — | renderD128 | — | `drmOpenWithType("panfrost", RENDER)` → **-1**; `("panfrost7580")` → 4 |
| 16 | `render-opens-shim` | — | renderD128 | name shim | `drmOpenWithType("panfrost", RENDER)` → **4** |

`EGL_PLATFORM=gbm` (the kmscube default) is rows 1–6 and 10–13; `GBM_BACKEND=dri`
is the only backend the image has (`gbm: backend=drm`), so `GBM_BACKEND=dri`
changes nothing (the card0 cell for it is in the cell list and is a no-op).

Interpretation, one line each:

* Mesa 26 never gets past the render-node *name*: rows 2, 4 show no panfrost
  activity at all; row 5 shows panfrost (via kmsro) being reached the moment the
  name matches, and it then fails on the DRM version (row 3/5/9) and finally on
  the panfrost ioctl ABI (row 6). So for *system* Mesa 26, renaming alone is not
  enough: it would also need `.minor = 1` **and** a 6.x panfrost ioctl ABI.
* Mesa 20.3.5 renders on Panfrost with nothing but the name rewritten
  (rows 11, 12) — this is the stack that already runs at 155 fps (int49), and it
  has no DRM-version or CREATE_BO gate.
* Mesa 20.3.5 as built (`gallium-drivers=panfrost`) can **never** pair with
  card0: row 13 is the exact error the gbm path would hit on card0, because
  `exynos_dri.so` is absent.

### 4.1 The card0 rows (skipped on this boot)

`sys-card-baseline`, `sys-card-verbose`, `sys-card-gbmbackend`,
`sys-card-ovr-exynos`, `sys-card-ovr-panfrost`, `sys-card-shim`,
`sys-card-surfaceless*`, `m20-card-*`, `kmscube-baseline`, `kmscube-shim` all
printed `CELL … renderer=<skipped: no /dev/dri/card0>`.

Reason, measured:

```
# insmod /home/user/drm_kms_helper-int82.ko
drm_kms_helper: version magic '3.10.108 SMP preempt mod_unload aarch64'
                should be '3.10.108 SMP preempt aarch64'
exynos_drm7580: version magic '3.10.108 SMP preempt mod_unload aarch64'
                should be '3.10.108 SMP preempt aarch64'
```

The running kernel `#37` was built with `CONFIG_MODULE_UNLOAD=n`, the int82
modules with `mod_unload`; and the int82 `.ko` pair is image-paired (int83 adds a
field to `struct drm_mode_config`), so force-loading it is not an option. No
`/proc/config.gz`; `insmod -f` exists but is not usable here by design.

Everything else about the card0 path is deterministic from §3 and is confirmed
by the render-node rows: the same `loader_get_driver_for_fd` picks
`exynos_dri.so` for card0 (the name is already `exynos`), and the same kmsro
render-node search is what fails.

---

## 5. Minimal fix

1. **Kernel (1 string):** `p7580_drm_driver.name = "panfrost"` (instead of
   `"panfrost7580"`) in
   `code/gpu31/driver/0001-panfrost7580-driver-sources.patch:5856`. Keep the
   platform driver name, module name and DT match as they are. Nothing else in
   the driver changes; `drmGetVersion()` starts answering `panfrost`, which
   satisfies gate 2 for Mesa 20 and gates 2/2' for Mesa 26.
2. **Render stack:** keep `/data/mesabuild/mesa20-b` for rendering (rows 11/12);
   the stock Mesa 26 additionally needs `.minor = 1` and the newer
   `PANFROST_CREATE_BO` argument set (rows 3/6) — track that separately, it is
   not needed for kmscube-on-panfrost today.
3. **Mesa 20 build flag:** mesa20-b as installed cannot be the pairing side;
   either rebuild it with `-Dgallium-drivers=panfrost,kmsro` (produces
   `exynos_dri.so`; `meson.build:219`), or use the system Mesa 26 once gates 3/4
   are addressed. `-Dplatforms=x11 -Dllvm=disabled` can stay.
4. **Not a fix:** `EGL_PLATFORM=gbm|surfaceless`, `GBM_BACKEND=dri`,
   `MESA_LOADER_DRIVER_OVERRIDE=exynos|panfrost`, `LIBGL_DEBUG`/`EGL_LOG_LEVEL`/
   `PAN_MESA_DEBUG` only change *logging* or the primary driver name; none of
   them can rename the render device (rows 2–9).

Expected result after (1) + (3), with `EGL_PLATFORM=gbm`:

```
kmscube -D /dev/dri/card0
  ...
  renderer: Mali T720 (Panfrost)     <- instead of llvmpipe
```

and on `card0` the traces to look for are `CREATE_DUMB` + `prime_handle_to_fd`
(renderonly's allocation) — not `fd_to_handle` (see §3.3). The page flip still
carries no fb until the int83 fix lands; that is a separate defect.

Longer term, for Weston/KWin on the same path: add `IN_FORMATS` +
`DRM_MODE_FB_MODIFIERS`/`DRM_FORMAT_MOD_LINEAR` acceptance to
`exynos_drm7580_dmabuf.c` so modifier-aware clients do not have to fall back.

---

## 6. Why "the kernel names it `panfrost7580`" was not visible before

`egltest`/`kmsflip` on `renderD128` always set
`MESA_LOADER_DRIVER_OVERRIDE=panfrost` (`tools/env.sh` `kms|egl`), which bypasses
gate 1 for a *render-only* use and hides the name mismatch: panfrost's screen is
created directly on the render node, no kmsro, no card0. The mismatch only
matters at the kmsro join, which needs a KMS node — i.e. it stayed invisible
until card0 existed.

---

## 7. Finishing the card0 matrix (one boot, after the paired image)

`int83` (or any image whose `.ko` pair matches the running kernel) must be
flashed by Main first — the int82 `.ko` pair must **not** be loaded on it.

```sh
# 1. on the phone, as root, from the image's own module pair
echo nura | /usr/local/bin/rootshell sh /home/user/mesa/load-kms.sh int83
#    -> /dev/dri/card0 + renderD128, wdpet + logd armed

# 2. as user, serialized behind the shared lock (logd holds the lock: stop it
#    first or run with the lock already released, see note)
flock /tmp/gpsdev.lock sh /home/user/mesa/run-matrix.sh all
```

Cells and expected renderer (with today's kernel, i.e. *before* the rename):

| Cell | Expected now | Expected after rename (+ mesa20 kmsro) |
|---|---|---|
| `sys-card-baseline` | llvmpipe | Mali T720 (Panfrost) |
| `sys-card-verbose` | llvmpipe | Mali T720 (Panfrost) |
| `sys-card-gbmbackend` | llvmpipe | same as baseline |
| `sys-card-ovr-exynos` | llvmpipe (kmsro loaded, no render node found) | Mali T720 (Panfrost) |
| `sys-card-ovr-panfrost` | llvmpipe (panfrost on an exynos fd: `CREATE_BO`/screen fails) | n/a |
| `sys-card-shim` | Mali T720 (Panfrost) *if* the version/ABI gate is shimmed; with Mesa 20 + name shim: Panfrost | — (shim is only proof machinery) |
| `m20-card-ovr-panfrost` | gbm device fails (panfrost on card0) | n/a |
| `m20-card-shim-kmsro` | `<none>`: `failed to open exynos` (no kmsro in mesa20-b) | Panfrost (after rebuilding mesa20 with `kmsro`) |
| `kmscube-baseline` | `renderer: llvmpipe` | `renderer: Mali T720 (Panfrost)` |
| `kmscube-shim` | `renderer: Mali T720 (Panfrost)` with mesa20-b + name shim | — |

Note: `/home/user/logd.sh` opens `/tmp/gpsdev.lock` and holds it for its whole
life; kill it (root) before taking the lock for the matrix, then restart it if
dmesg persistence is wanted during the kmscube cells.

`mesaprobe --gbm /dev/dri/card0 --render --addfb` additionally reports
`CREATE_DUMB`/`drmPrimeFDToHandle`/`drmModeAddFB2[WithModifiers]` results
directly, without committing a CRTC — a safe way to check the scanout side
before running kmscube into the known page-flip bug.

---

## 8. Appendix — raw evidence

```
### render-opens (real names)
PROBE openWithType: name=exynos type=card -> -1 (not found)
PROBE openWithType: name=panfrost type=render -> -1 (not found)
PROBE openWithType: name=panfrost7580 type=render -> 4

### render-opens-shim
PROBE openWithType: name=panfrost type=render -> 4

### sys-render-gbm-ovr-exynos (Mesa 26, kmsro)
MESA: error: ZINK: vkCreateInstance failed (VK_ERROR_INCOMPATIBLE_DRIVER)
PROBE renderer: llvmpipe (LLVM 23.1.5, 128 bits)

### sys-render-gbm-ovr-exynos-shim (same, node renamed to panfrost)
drmshim: fd 7 name 'panfrost7580' -> 'panfrost'
MESA: error: kernel driver is too old (requires at least 1.1, found 1.0)
PROBE renderer: llvmpipe (LLVM 23.1.5, 128 bits)

### sys-render-gbm-ovr-exynos-shimver (+ DRM_VER_SHIM=1.1)
MESA: error: DRM_IOCTL_PANFROST_CREATE_BO failed (err=22)
PROBE renderer: llvmpipe (LLVM 23.1.5, 128 bits)

### m20-render-ovr-panfrost (mesa20-b)
PROBE renderer: Mali T720 (Panfrost)
PROBE gl: vendor=Panfrost version=OpenGL ES 3.0 Mesa 20.3.5
PROBE gbm_surface: eglSwapBuffers ok

### m20-render-kmsro (mesa20-b, override=exynos)
MESA-LOADER: failed to open exynos: ... /data/mesabuild/mesa20-b/.../exynos_dri.so: No such file or directory
PROBE gbm: gbm_create_device(/dev/dri/renderD128) FAILED (No such file or directory)

### card0 module load
drm_kms_helper: version magic '3.10.108 SMP preempt mod_unload aarch64'
                should be '3.10.108 SMP preempt aarch64'
```
