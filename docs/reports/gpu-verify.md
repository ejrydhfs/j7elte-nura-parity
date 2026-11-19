# GPU verification on j7elte — what the hardware and userspace actually do

Date: 2026-11-18/19. Device: SM-J700M (j7elte), Nura (Alpine aarch64/musl),
kernel `3.10.108 #33 SMP PREEMPT` built 2026-11-18 21:03 from
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
(boot tree under `/run/media/asdf/db69437b-…/j7k/`, image timestamp matches `#33`).

**Verdict**

| Item | Result |
|---|---|
| Kernel GPU interface | **kbase present and working**: `/dev/mali0`, DDK `r15p0-00rel0`, UK 10.6; `Mali-T72x MP2 r1p0 0x0720`, 2 shader cores, 3 job slots, 33 VA bits, coherency NONE |
| GPU silicon exercised | **yes, but only by bring-up**: kbase winsys selftest ran a GPU soft job (223 GPU cycles on the recorded run), allocated a SAME_VA BO and completed an in/out fence cycle |
| 2D (X on fbdev) | **works**: Xorg 720×1280 on `/dev/fb0`, `nura-shell` + `glxgears` windows mapped, full-screen XWD capture has 2065 distinct colours |
| 3D GL/GLES API | **works, software only**: Mesa 26.3.0 llvmpipe (LLVM 23.1.5), GL 4.6 / GLES 3.2, `Accelerated: no` |
| Measured FPS | `glxgears` 173–221 FPS @320×240; `glmark2-es2` 15–244 FPS across 29 completed scenes (FPS table below) |
| Hardware GL acceleration | **not available and not achievable without a kernel change**: `# CONFIG_DRM is not set`, no `/dev/dri`, Mesa `panfrost_dri.so` cannot create a screen; kbase's only userspace is ARM's closed DDK (absent, 32-bit Android in ROM) |
| `panfrost7580.ko` (repo M0 kmod) | **cannot be deployed**: running kernel has `# CONFIG_MODULES is not set` (`insmod` → ENOSYS), plus vermagic mismatch; and it is a bring-up module with no DRM node and no render path |
| Wayland/Weston 2D | **impossible**: no `/dev/dri` and no seat daemon (seatd/logind); `glmark2-drm` reports “Failed to find a suitable DRM device”, EGL GBM platform fails to initialise |

Two real defects were found and fixed while verifying (both in the repo, not
kernel): the `kbase-probe` UK ABI was wrong (16-byte header instead of the
8-byte `union uk_header`), so the probe never actually completed the
handshake; and `kbase-winsys` decoded the selected coherency mode as
“coherent” for value `0x1f` (31 = `COHERENCY_NONE`). Both are described in §6.

---

## 1. State before this session

| Object | State |
|---|---|
| `/dev/mali0`, `/sys/devices/11400000.mali/` | present (kbase compiled in, `CONFIG_MALI_MIDGARD/T7XX/R15P0=y`) |
| `/dev/dri/*` | absent (`# CONFIG_DRM is not set`, built `.config:2267`) |
| `/dev/fb0` | present, DECON, 720×1280, 32 bpp |
| X session | **already running**: `xinit /usr/local/bin/nura-shell -- :0 vt1 -config /etc/X11/xorg.conf.d/99-fbdev.conf` (Xorg 21.1.26 + fbdev DDX, shadow framebuffer, libinput touchscreen) |
| Packages | `mesa-dri-gallium 26.3.0-r0`, `xf86-video-fbdev 0.5.0-r6`, `xorg-server 21.1.26-r0` installed; **`j7elte-gpu-soft` NOT installed** (no `/etc/profile.d/j7elte-softgl.sh`, no `10-fbdev.conf`; only the display work's `99-fbdev.conf`) |
| Test tools | `eglinfo`, `glxinfo`, `glmark2*`, `es2_info` absent |
| `panfrost7580.ko` | not loaded (kernel cannot load modules at all) |

## 2. What was deployed now

1. **`j7elte-gpu-soft` 1-r0 — built *from the repo's APKBUILD* on the device**
   (gcc 15.2.0). abuild initially failed only because `/` (2.0 G, `mmcblk0p20`)
   is 100 % full; the build tree was moved to `/data` (7.5 G free):

   ```
   # on device, source copied from code/pmaports/device/archived/j7elte-gpu-soft
   addgroup user abuild                       # abuild requires the group
   cd /data/gpusoft && REPODEST=/data/pkgs abuild -r
   >>> j7elte-gpu-soft: Building user/j7elte-gpu-soft 1-r0 (using abuild 3.18.0-r0)
   >>> j7elte-gpu-soft: Updating the data/aarch64 repository index...
   # apk add --allow-untrusted /data/pkgs/data/aarch64/j7elte-gpu-soft-1-r0.apk
   (1/1) Installing j7elte-gpu-soft (1-r0)
   ```

   Result (`apk list -I | grep gpu-soft`):
   `j7elte-gpu-soft-1-r0 aarch64 {j7elte-gpu-soft} (MIT) [installed]`
   Files: `/etc/X11/xorg.conf.d/10-fbdev.conf` (644),
   `/etc/profile.d/j7elte-softgl.sh` (644, `LIBGL_ALWAYS_SOFTWARE=1`;
   verified present in a login shell: `shell=ash LIBGL_ALWAYS_SOFTWARE=1`).

   Note: the live session was started with `-config 99-fbdev.conf` (display
   work), so it does not read `10-fbdev.conf`; the two files are equivalent
   fbdev stanzas and only one is loaded per server instance.

2. **Verification tools only** (not part of any device package):
   `apk add mesa-utils mesa-demos glmark2`, `weston` (16.0.0) and
   `weston-backend-drm` from Alpine edge (network reachable; `apk update` OK:
   36779 packages).

3. Nothing kernel-side was deployed: no source change can make the existing
   `# CONFIG_DRM is not set` kernel expose a render node, and no module can be
   loaded (§5).

## 3. 2D and 3D runtime evidence

All commands below ran on the device; GL tests used the already-running
`DISPLAY=:0` server.

### 3.1 X server / 2D

```
$ DISPLAY=:0 xdpyinfo
name of display:    :0
version number:    11.0
number of extensions:    26
  dimensions:    720x1280 pixels (191x339 millimeters)
  depth of root window:    24 planes

$ DISPLAY=:0 xwininfo -root -tree
xwininfo: Window id: 0x220 (the root window) (has no name)
     2 children:
     0x600003 "glxgears": ()  320x240+0+0  +0+0
     0x400001 "nura-shell": ()  720x1280+0+0  +0+0
```

`/var/log/Xorg.0.log` (the running session):

```
(II) FBDEV(0): Modeline "current"x0.0  64.59  720 776 780 828  1280 1293 1295 1300 -hsync -vsync -csync
(**) FBDEV(0): using shadow framebuffer
(II) AIGLX: Screen 0 is not DRI2 capable
(II) GLX: Initialized DRISWRAST GL provider for screen 0
(II) Using input driver 'libinput' for 'sec_touchscreen'
```

Pixel proof (`/dev/fb0` reports `bits_per_pixel=32`; a raw `read()` returns
0 bytes on this DECON fb, so an XWD of the root window is the surface
evidence; sampled every 4th pixel):

```
$ cat /sys/class/graphics/fb0/bits_per_pixel
32
$ dd if=/dev/fb0 bs=4096 count=1 | wc -c
0
$ xwd -root -silent -out /tmp/root.xwd && <parse>
xwd header_size 107 version 7 format 2 depth 24 size 720 x 1280
bytes_per_line 2880 (4 bytes/pixel)
sampled pixels 172800 distinct 2065
top colours [(b'\x00\x00\x00\x00', 29714), (b'\x1b( \x1b', 23453), ...]
```

### 3.2 EGL/GL renderer

```
$ EGL_PLATFORM=surfaceless eglinfo -B
GBM platform:      eglinfo: eglInitialize failed
Wayland platform:  eglinfo: eglInitialize failed
X11 platform:      eglinfo: eglInitialize failed   # no X in this shell
Surfaceless platform:
EGL API version: 1.5
EGL vendor string: Mesa Project
EGL client APIs: OpenGL OpenGL_ES
OpenGL ES profile renderer: llvmpipe (LLVM 23.1.5, 128 bits)
OpenGL ES profile version: OpenGL ES 3.2 Mesa 26.3.0

$ DISPLAY=:0 glxinfo -B
direct rendering: Yes
    Device: llvmpipe (LLVM 23.1.5, 128 bits) (0xffffffff)
    Version: 26.3.0
    Accelerated: no
OpenGL vendor string: Mesa
OpenGL renderer string: llvmpipe (LLVM 23.1.5, 128 bits)
OpenGL core profile version string: 4.6 (Core Profile) Mesa 26.3.0
OpenGL ES profile version string: OpenGL ES 3.2 Mesa 26.3.0

$ DISPLAY=:0 es2_info
MESA-EGL: warning: DRI3 error: Could not get DRI3 device
GL_VENDOR: Mesa
GL_VERSION: OpenGL ES 3.2 Mesa 26.3.0
GL_RENDERER: llvmpipe (LLVM 23.1.5, 128 bits)
```

### 3.3 FPS

```
$ DISPLAY=:0 glxgears -info -geometry 320x240
GL_RENDERER   = llvmpipe (LLVM 23.1.5, 128 bits)
GL_VERSION    = 4.6 (Compatibility Profile) Mesa 26.3.0
869 frames in 5.0 seconds = 173.678 FPS
1105 frames in 5.0 seconds = 220.863 FPS
943 frames in 5.0 seconds = 188.528 FPS
```

`glmark2-es2` (EGL/GLES 2.0 through Mesa on the X server, 320×240,
`-b :duration=2.0 --results fps`; the 100 s timeout cut the run during the
30th scene, so 29 scenes reported FPS. Scene names below are shortened and the
lines are reflowed; the FPS values are verbatim):

```
[build] use-vbo=false: FPS: 67    [build] use-vbo=true: FPS: 87
[texture] nearest: 224           [texture] linear: 232       [texture] mipmap: 244
[shading] gouraud: 39            [shading] blinn-phong-inf: 33
[shading] phong: 20              [shading] cel: 23
[bump] high-poly: 15             [bump] normals: 110         [bump] height: 46
[effect2d] kernel 0,1,0;-4: 43   [effect2d] kernel 1x3 box: 48
[pulsar] light=false: 149
[desktop] blur: 15               [desktop] shadow: 30
[buffer] map/0.5: 21             [buffer] subdata: 32        [buffer] map/interleave: 33
[ideas] 1                        [jellyfish] 1               [terrain] 1
[shadow] 21                      [refract] 1
[conditionals] 85 / 86 / 105
[function] low: 70               (run stopped by timeout here)
```

`glmark2` (GLX path) `--validate` on three loop shaders:
`Validation: Success` (three times).

The “1 FPS” scenes (ideas/jellyfish/terrain/refract) and the heavy fragment
scenes are CPU-bound llvmpipe at 320×240 on the A53; they are not GPU stalls
(see §4, GPU utilisation stayed 0).

### 3.4 Wayland / DRM paths (negative evidence)

```
$ ls /dev/dri
ls: /dev/dri: No such file or directory

$ weston --backend=drm --socket=wtest          # weston 16, before
Loading module '/usr/lib/libweston-16/drm-backend.so'
Failed to load module: ... No such file or directory   # split package
fatal: failed to create compositor backend

$ apk add weston-backend-drm                   # 16.0.0-r0, libseat 0.9.3
$ weston --backend=drm --socket=wtest2
initializing drm backend
Trying libseat launcher...
[libseat] Could not connect to socket /run/seatd.sock: No such file or directory
[libseat] Backend 'seatd' failed to open seat, skipping
[libseat] Backend 'logind' failed to open seat, skipping
fatal: your system should either provide the logind D-Bus API, or use seatd.

$ glmark2-drm -b :duration=1
Error : Not a tty
Error: Failed to find a suitable DRM device
```

Weston's DRM backend is a separate package and additionally needs a seat
(seatd/logind, neither present); even with one it has no `/dev/dri` node to
open. There is no fbdev backend in weston 16.

## 4. GPU hardware: alive, idle during “3D”

Kernel-visible device (kbase, built-in):

```
$ cat /sys/devices/11400000.mali/gpuinfo
Mali-T72x MP2 r1p0 0x0720
$ cat /sys/devices/11400000.mali/core_mask
Current core mask (JS0) : 0x3 ... Available core mask : 0x3
$ cat /sys/devices/11400000.mali/power/runtime_status
suspended
```

`kbase-probe` (fixed ABI, see §6) as root:

```
/dev/mali0                  = 3
/dev/dri/renderD128         = -2 (open failed)
/dev/dri/card0              = -2 (open failed)
/dev/fb0                    = 3
handshake 10.6 -> (major.minor)= 10.6, ret 0
SET_FLAGS(0) ret            = 0, header.ret 0
GET_VERSION ret             = 0, header.ret 0
kernel DDK version          = K:r15p0-00rel0(GPL)
kbase-probe: kbase UK ABI present; no DRM node (Mesa Panfrost/GBM cannot bind)
kbase-probe: OK                       (exit 0)
```

The repo's `check-3d.sh` (run as root with `DISPLAY=:0` and the rebuilt probe)
reports the same state end-to-end and concludes:

```
No DRM render node: Mesa Panfrost (and any GBM/KMS client) cannot
create a screen. There is no hardware 3D on this kernel; use Mesa
llvmpipe/swrast via Xorg + xf86-video-fbdev (software rendering).
```

The repo's `kbase-winsys` selftest (built on-device with gcc 15, run as root)
went further than the probe — it queried props, allocated a BO, submitted a
soft job and completed a fence round trip **on the GPU**:

```
capabilities:
UK                   10.6
DDK                  K:r15p0-00rel0(GPL)
GPU_ID               0x07201000 (product 0x720, r1p0)
shader_present       0x0000000000000003 (2 cores)
tiler_present        0x0000000000000001
l2_present           0x0000000000000001
mem_features         0x00000001
mmu_features         0x00002821 (VA bits 33)
l2_features          0x07110206
tiler_features       0x00000809
thread_features      0x0a040400 (max 256 threads/core, 128/WG)
js_present           0x00000007 (3 job slots)
core groups          1 (1 coherent groups)
coherency mode       0x0000001f (non-coherent)
context created (own event thread + sync timeline)
BO: cpu=0x1ffffe000 gpu=0x1ffffe000 size=4096 flags=0x300f
soft job DONE: gpu cycles=223 time=1614.397168s
submit returned out fence fd 7
fence 7 signalled
in-fence submit accepted
context idle
bring-up path OK                      (exit 0)
```

So the GPU executes kbase work. It is **not** used by any GL client: with
`glxgears` running for 10 s on `:0`:

```
util=0 power=0 status_line=reset count : 0      (4 samples over 10 s)
time_in_state: 440 0 / 534 0 / 668 0            (no cycles at any OPP)
```

GPUDVFS stayed at the lowest state with zero utilization while llvmpipe was
rendering — the expected signature of software rasterization.

## 5. Why there is no hardware acceleration, and why the kmod cannot be deployed

### 5.1 No DRM, therefore no Mesa Panfrost

Two things that are *not* blockers were checked explicitly: kbase loads no
GPU firmware (`0` `request_firmware` call sites in `drivers/gpu/arm/t7xx/r15p0`,
so there is no `files-gpu.list` to add), and the deviceinfo has no GPU/DRM
knob to correct (`deviceinfo_screen_width/height = 720/1280` matches the
panel; `/etc/deviceinfo` is not even installed in this Nura image).

The built kernel `.config` (same tree that produced `#33`) contains:

```
217: # CONFIG_MODULES is not set
2267: # CONFIG_DRM is not set
2289: CONFIG_MALI_MIDGARD=y
2293: CONFIG_MALI_T7XX=y
2295: CONFIG_MALI_R15P0=y
2363: CONFIG_EXYNOS_DECON_FB=y
3178: CONFIG_SYNC=y
```

Consequences observed: no `/dev/dri/*`; `panfrost_dri.so` exists but has no
render node to bind to (GBM `eglInitialize failed`); kbase is a misc device
speaking the in-buffer UK ioctl ABI, not the `DRM_IOCTL_PANFROST_*` UAPI that
Mesa's panfrost Gallium driver requires. The ROM's `libGLES_mali.so` is
32-bit Android/bionic and cannot be loaded into this aarch64 musl userland.
The only userspace that can drive kbase is ARM's proprietary Linux DDK, which
is not present and not redistributable.

### 5.2 `panfrost7580.ko` deployment attempts

```
$ insmod /tmp/panfrost7580.ko
insmod: ERROR: could not insert module /tmp/panfrost7580.ko: Function not implemented
$ cat /proc/modules
cat: can't open '/proc/modules': No such file or directory
$ cat /proc/sys/kernel/modprobe
cat: can't open '/proc/sys/kernel/modprobe': No such file or directory
```

The running kernel was built with `# CONFIG_MODULES is not set` (`.config:217`,
confirmed by the missing `/proc/modules` and ENOSYS), so **no** module can be
loaded, whatever its vermagic. There are two further independent blockers:

* the shipped `.ko` has `vermagic: 3.10.108-gb5e363a1 SMP preempt mod_unload
  aarch64` while the running kernel's UTS release is `3.10.108`
  (`include/config/kernel.release`, `CONFIG_LOCALVERSION_AUTO=y` but the build
  tree has no git suffix); it would be rejected even if modules were enabled;
* the device has no kernel build tree or headers (`/lib/modules` absent), so an
  on-device rebuild is impossible. The source/build tree is on the host
  (`/run/media/asdf/…/j7k/android_kernel_samsung_universal7580-2a746c1…`) and
  producing a loadable module requires a `CONFIG_MODULES=y` kernel build and
  flashing a new boot image — a kernel change, not a module deployment.

Even if it loaded, `panfrost7580.ko` is the M0 milestone: it maps registers,
resets/powers the block, reads features and self-tests the page-table encoder;
it creates **no DRM node** and refuses to probe while kbase owns the GPU unless
`force=1`. It is not a rendering driver.

## 6. Defects found and fixed

### 6.1 `kbase-probe`: wrong UK header ABI (commit-blocking for the probe)

`mali_uk.h:100` defines **`union uk_header { u32 id; u32 ret; u64 sizer; }`**
(8 bytes; `id` and `ret` alias, `sizer` only forces alignment), not a 16-byte
struct. The probe declared a struct `{u32 id; u32 ret; u64 sizer;}`, making
`uku_version_check_args` 24 bytes instead of 16 and
`kbase_uk_get_ddk_version` 88 instead of 80. `kbase_dispatch()` rejects a
mismatched size (`mali_kbase_core_linux.c:300`), and `kbase_ioctl()` maps any
dispatch error to `-EFAULT` (`:1356-1357`). Observed before the fix:

```
handshake 10.6 -> (major.minor)= 10.6, ret 0      # echoed its own input
SET_FLAGS(0) ret            = -14, header.ret 0
kbase-probe: context setup failed
```

After replacing the struct with the union (plus compile-time size asserts and
a `header.ret` check) the probe completes the sequence and returns exit 0
(§4). `code/userspace/gpu/kbase-probe/kbase-probe` was rebuilt with
`aarch64-linux-gnu-gcc` from the fixed source; the host smoke test under
qemu-aarch64 still correctly reports “/dev/mali0 not available”.

### 6.2 `kbase-winsys`: selected coherency mode misread as “coherent”

`raw_props.coherency_mode` is the **selected** mode: `kbase_gpuprops_set_features()`
writes the `coherency_features` register value first, then
`kbase_device_coherency_init()` overwrites it with `kbdev->system_coherency`
(`mali_kbase_core_linux.c:3577,3605`). `mali_midg_coherency.h` defines
`ACE_LITE=0`, `ACE=1`, `NONE=31`, and the j7elte DT has no
`system-coherency` property, so the device reports `0x1f` = **NONE =
non-coherent**. The winsys used `coherency_mode != 0`, i.e. it reported
“coherent”, which would tell a Panfrost-style MMU backend that page-table
walks are coherent when they are not. Fixed to `!= 31` (winsys and the
`is_io_coherent` initialisation in `patches/gpu-mesa-kbase/0001`), with the
kernel provenance documented; the on-device selftest now prints
`coherency mode 0x0000001f (non-coherent)`.

## 7. Remaining blockers (with evidence) and realistic next steps

1. **Hardware GL cannot exist on this kernel configuration.** `CONFIG_DRM=n`,
   no `/dev/dri`, kbase UK ABI only. The repo's Mesa-on-kbase series
   (`patches/gpu-mesa-kbase/0001-0003`) is the only open path, but it needs a
   full Mesa 27 build (LLVM for `mesa_clc`, meson, an aarch64 rootfs) and still
   has no WSI/display integration; nothing of it is deployed. Building and
   validating it is a multi-week project, not a deployment step.
2. **Panfrost DRM offload (M0→M1…) needs a new kernel**: `CONFIG_DRM` and
   `CONFIG_MODULES` are off, and panfrost's 3.10 prerequisites (dma-fence,
   dma-resv, drm_sched, drm_gem_shmem, io-pgtable, drm_dev_alloc) are absent
   (documented in `gpu-panfrost.md`). Flashing a new kernel is outside this
   verification and was not attempted.
3. **Wayland** is impossible without DRM: the DRM backend needs both a seat
   (seatd/logind, absent) and a DRM device (`/dev/dri`, absent), and weston 16
   has no fbdev backend. X + fbdev + llvmpipe remains the 2D and 3D (software)
   path.
4. **`/` is 100 % full** on `mmcblk0p20` (2.0 G, 0 available) — the reason the
   first abuild run failed. It does not block GL (packages land on `/usr`,
   which is `/data`), but any future on-device build must use `/data` or
   `/tmp` (`/tmp` is tmpfs, 682 M free).

## 8. Files changed in this task

```
docs/reports/gpu-verify.md                                  (this report)
code/userspace/gpu/kbase-probe/kbase-probe.c                UK union header + size asserts + ret check
code/userspace/gpu/kbase-probe/kbase-probe                  rebuilt aarch64 static binary
code/userspace/gpu/kbase-winsys/src/kbase_winsys.c          coherency NONE decode (31)
code/userspace/gpu/kbase-winsys/include/kbase_winsys.h      field semantics documented
patches/gpu-mesa-kbase/0001-...patch                        same coherency fix in the Mesa backend
```

Runtime deployment (not a file change): `j7elte-gpu-soft-1-r0` built from the
repo APKBUILD and installed; `mesa-utils`, `mesa-demos`, `glmark2`, `weston`
installed (plus `weston-backend-drm`) to run the tests above.
