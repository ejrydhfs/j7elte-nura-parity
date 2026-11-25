# Mesa 20.3.5 kmsro build — `panfrost,kmsro` side-by-side (`mesa20-kmsro`)

Date: 2026-11-25. Slice: `MesaKmsroBuild`.
Device: SM-J700M (j7elte), Nura Alpine aarch64, `Linux nura-j7elte 3.10.108 #37`.
Related: `docs/reports/kms-mesa-pairing.md` (why kmsro is needed),
`docs/reports/gpu31-mesa20.md` (the panfrost-only build and its local patches),
`code/gpu31/kms/tools/mesa/env-kmsro.sh` (selector written by this slice),
`code/gpu31/kms/tools/mesa/run-matrix.sh` (pairing matrix cells).

## 0. Verdict

* `-Dgallium-drivers=panfrost,kmsro` builds and installs cleanly on the phone
  (gcc 15.2.0, meson 1.12.1, ninja 1.9.0, musl). New trees, nothing replaced:

  | Tree | Contents | Size |
  |---|---|---|
  | `/data/mesabuild/mesa20-kmsro` | meson + ninja build tree | 329.2 MB |
  | `/data/mesabuild/mesa20-kmsro-install` | `DESTDIR` install tree (`/usr/local`) | 80.2 MB |
  | `/data/mesabuild/mesa20-b` | untouched panfrost-only build | 349.9 MB |

* `exynos_dri.so` now exists **and is opened by the loader for `/dev/dri/card0`**
  — measured today on this boot (§4), where the panfrost-only build fails with
  `failed to open exynos: ... exynos_dri.so: No such file or directory`.
* The **`Mali T720 (Panfrost)` re-check could not be run on this boot**: the
  running kernel (int83 image) contains no GPU/render driver at all
  (`/proc/kallsyms` has 0 `p7580` symbols, no `renderD128`, `lsmod` shows only
  `exynos_drm7580` + `drm_kms_helper`). That is an image property, not a build
  result. Main owns the flash; int84 adds the render driver. The exact
  one-shot commands for the moment `renderD128` reappears are in §5.

## 1. Why kmsro had to be named explicitly

Mesa 20.3.5 only compiles kmsro when the option list contains it — it is not
implied by the render driver:

```meson
# meson.build:219
with_gallium_kmsro = gallium_drivers.contains('kmsro')
# meson.build:291 (the only sanity check)
if with_gallium_kmsro and not (... with_gallium_panfrost ...)
```

`mesa20-b` was configured with `gallium-drivers = ['panfrost']`, so
`src/gallium/targets/dri/target.c`'s `#if defined(GALLIUM_KMSRO)` block — the
one that exports `__driDriverGetExtensions_exynos` (and 20 other kmsro names) —
was compiled out. Measured on the two builds:

```
nm -D mesa20-b/.../libgallium_dri.so          -> __driDriverGetExtensions_panfrost   (only)
nm -D mesa20-kmsro/.../libgallium_dri.so      -> __driDriverGetExtensions_panfrost
                                                 __driDriverGetExtensions_exynos
                                                 + armada_drm, hx8357d, ili9225, ili9341,
                                                   imx_drm, imx_dcss, ingenic_drm, mcde,
                                                   mediatek, meson, mi0283qt, mxsfb_drm,
                                                   pl111, repaper, rockchip, st7586,
                                                   st7735r, stm, sun4i_drm
```

Name resolution after the loader has dlopened `<kernel-name>_dri.so`
(`src/gallium/auxiliary/pipe-loader/pipe_loader_drm.c:95-110`): with
`GALLIUM_STATIC_TARGETS` (the dri megadriver), an unknown driver name falls
through to

```c
   for (i = 0; i < ARRAY_SIZE(driver_descriptors); i++)
      if (!strcmp(driver_descriptors[i]->driver_name, driver_name)) return ...;
   return &kmsro_driver_descriptor;      /* <- "exynos" lands here */
```

i.e. `exynos_dri.so` is the kmsro entry point by construction, and kmsro then
does `drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER)` and creates the
panfrost screen on the renderonly (see `kms-mesa-pairing.md` §3.2/§3.3).

## 2. Build recipe (exact)

Source tree: `/data/mesabuild/mesa20` (mesa 20.3.5, unpacked from
`mesa-20.3.5.tar.xz`, already carrying the local panfrost diagnostics /
1 MB-tiler-heap patches used by the working `mesa20-b`; **not** modified by
this slice). Toolchain on device: `cc` = gcc 15.2.0 (Alpine musl), meson
1.12.1, ninja 1.9.0, 4 cores, no swap (1374 MB RAM).

```sh
cd /data/mesabuild
meson setup mesa20-kmsro mesa20 \
  -Dbuildtype=debugoptimized -Dbuild-tests=false \
  -Ddri-drivers= -Ddri3=disabled -Degl=enabled \
  -Dgallium-drivers=panfrost,kmsro -Dgallium-omx=disabled \
  -Dgbm=enabled -Dgles1=enabled -Dgles2=enabled -Dglx=dri \
  -Dllvm=disabled -Dopengl=true -Dplatforms=x11 -Dshared-llvm=disabled \
  -Dvulkan-drivers=
ninja -j3                      # in mesa20-kmsro
DESTDIR=/data/mesabuild/mesa20-kmsro-install ninja install
```

Every option matches `mesa20-b`'s `meson-info/intro-buildoptions.json`
(`buildtype=debugoptimized`, `optimization=2`, `debug=true`,
`build-tests=false`, `prefix=/usr/local`, `libdir=lib`, no `c_args`) except
`gallium-drivers`, which is `['panfrost','kmsro']`. Configure summary:

```
        Gallium drivers: panfrost kmsro
        EGL drivers:     builtin:egl_dri2
        GBM:             yes
        EGL/Vulkan/VL platforms:   x11 surfaceless drm
        DRI drivers:     no
        llvm:            no
        Build targets in project: 121
```

Build cost and one caveat: the first `ninja -j3` run was killed by the int83
flash reboot at 182/1047 targets; it was restarted in the same tree
(`setsid nohup ninja -j3`, log `/data/mesabuild/mesa20-kmsro-build.log`) and
finished 869/869 with **no `FAILED:` line**. Total compile ≈ 40 min of the 4
cores. Peak memory stayed under ~1 GB of the 1.3 GB (no OOM, no swap).

`ninja install` then ran Mesa's own megadriver script
(`mesa20/bin/install_megadrivers.py`), so the kmsro names are produced by Mesa,
not by hand:

```
install_megadrivers.py .../libgallium_dri.so /usr/local/lib/dri \
  armada-drm_dri.so exynos_dri.so hx8357d_dri.so ... panfrost_dri.so
```

## 3. Artifacts (measured on device)

```
ls -l /data/mesabuild/mesa20-kmsro/src/gallium/targets/dri/libgallium_dri.so
  -rwxr-xr-x  user  75458504  Nov 25 13:44   (mesa20-b's is 75498504 bytes)
sha256:
  e39e98716f0b5c2d76e98d3721cdd5b29f000d52374cceeb3ea20092595cc55a  mesa20-kmsro/src/gallium/targets/dri/libgallium_dri.so
  a27125168691607e5ddafdaa399c1cf95a0db9d982cac9af089cab78cf1cdb6c  mesa20-kmsro-install/usr/local/lib/dri/exynos_dri.so
  392f697457d3c7369cbadb6bfe5710290a60abd9741d7d2c7f82bcadcb2b408c  mesa20-kmsro-install/usr/local/lib/libEGL.so.1.0.0
  8b257eadb3f4947c0f9dfa1e9cac163fc2dfdbd6a774384ce347c267f627cd7c  mesa20-kmsro-install/usr/local/lib/libgbm.so.1.0.0
```

The install-tree `.so` hashes differ from the build-tree hash in the same byte
count because meson rewrote the build rpath out on install; the installed libs
carry **no** `RPATH`/`RUNPATH` at all (checked with `readelf -d`), so
`LD_LIBRARY_PATH` is the only thing that decides which stack is used.

`usr/local/lib/dri/` holds 21 names, all hardlinks to one inode
(`669868`; 75.5 MB per name, 72 MB real on disk): `armada-drm_dri.so exynos_dri.so
hx8357d_dri.so ili9225_dri.so ili9341_dri.so imx-drm_dri.so imx-dcss_dri.so
ingenic-drm_dri.so mcde_dri.so mediatek_dri.so meson_dri.so mi0283qt_dri.so
mxsfb-drm_dri.so pl111_dri.so repaper_dri.so rockchip_dri.so st7586_dri.so
st7735r_dri.so stm_dri.so sun4i-drm_dri.so panfrost_dri.so`.
There is **no** `kms_swrast_dri.so` / `swrast_dri.so` in this build (swrast was
not requested), so gbm's software fallback fails loudly instead of silently
rendering softpipe — deliberately different from `mesa20-b`, which carries a
hand-made `swrast_dri.so -> libgallium_dri.so` link to a driver it does not
contain.

## 4. Verification performed today (this boot, int83 image)

Selector used verbatim: `code/gpu31/kms/tools/mesa/env-kmsro.sh`
(copied to `/home/user/mesa/env-kmsro.sh` on the phone), run under
`flock /tmp/gpsdev.lock` as root. `mesaprobe` is the existing pairing tool; it
does **no** CRTC commit, so nothing was displayed, no flip was issued and the UI
was untouched.

| Cell | Stack | Env | Result |
|---|---|---|---|
| A | `mesa20-kmsro` (panfrost,kmsro) | `env-kmsro.sh`, **no** override, `--gbm /dev/dri/card0 --render` | loader opened `exynos_dri.so`; kmsro found no render node; gbm then failed (`kms_swrast`/`swrast` not in this build) |
| B | `mesa20-b` (panfrost only) | same, no override | `MESA-LOADER: failed to open exynos: ... exynos_dri.so: No such file or directory` |

Raw evidence:

```
### A: mesa20-kmsro (no override), card0        (rc=1)
MESA-LOADER: failed to open kms_swrast: Error loading shared library
  /data/mesabuild/mesa20-kmsro-install/usr/local/lib/dri/kms_swrast_dri.so:
  No such file or directory (search paths .../lib/dri)
failed to load driver: kms_swrast
MESA-LOADER: failed to open swrast: ... swrast_dri.so: No such file ...
failed to load swrast driver
PROBE gbm: gbm_create_device(/dev/dri/card0) FAILED (No such file or directory)
# note: no "failed to open exynos" line -> exynos_dri.so loaded and its
# screen-create ran; it returned NULL because there is no render node on int83.

### A, strace -f -e trace=openat (excerpt)
openat(AT_FDCWD, ".../dri/tls/exynos_dri.so", O_RDONLY|O_LARGEFILE|O_CLOEXEC) = -1 ENOENT
openat(AT_FDCWD, ".../dri/exynos_dri.so",     O_RDONLY|O_LARGEFILE|O_CLOEXEC) = 5
openat(AT_FDCWD, "/dev/dri/card0",            O_RDWR|O_LARGEFILE|O_CLOEXEC)  = 4
# no /dev/dri/renderD* open attempt: the node does not exist on this boot.

### B: mesa20-b panfrost-only (no override), card0  (rc=1)
MESA-LOADER: failed to open exynos: Error loading shared library
  /data/mesabuild/mesa20-b/src/gallium/targets/dri/exynos_dri.so:
  No such file or directory (search paths .../mesa20-b/src/gallium/targets/dri)
failed to load driver: exynos
... (kms_swrast, swrast) ...
PROBE gbm: gbm_create_device(/dev/dri/card0) FAILED (No such file or directory)
```

Why the renderer string is missing from this report (measured, not assumed):

```
$ grep -ci p7580 /proc/kallsyms                -> 0
$ ls /dev/dri                                  -> controlD64 card0
$ lsmod                                        -> exynos_drm7580, drm_kms_helper
$ ls /lib/modules                               -> (absent; .ko are insmod'ed by hand)
$ strings /home/user/exynos-drm7580-int83.ko | grep -E '^(panfrost|panfrost7580|exynos)$'
exynos
```

The int83 kernel has no `p7580`/panfrost driver compiled in or loadable, so
`renderD128` cannot exist and no Mesa build — this one or `mesa20-b` — can
report a renderer on either node. Main confirmed int84 will add the GPU driver.

## 5. The one test left: after int84 (`renderD128` + `card0` coexist)

Run under the shared lock, as root (`/dev/dri/card0` is `root:video 0660`):

```sh
# 0. on the phone, once (already done by this slice):
#    /home/user/mesa/env-kmsro.sh  is code/gpu31/kms/tools/mesa/env-kmsro.sh

# 1. render node only - must still be the same panfrost driver as mesa20-b.
#    The override is needed ONLY while the kernel still names the device
#    "panfrost7580" (gate 1 of kms-mesa-pairing.md); drop it after the rename.
cd /home/user/mesa
. ./env-kmsro.sh
MESA_LOADER_DRIVER_OVERRIDE=panfrost ./mesaprobe --gbm /dev/dri/renderD128 --render
#    expect: PROBE renderer: Mali T720 (Panfrost)
#            PROBE gl: vendor=Panfrost version=OpenGL ES 3.0 Mesa 20.3.5
#            PROBE gbm_surface: eglSwapBuffers ok

# 2. the kmsro pair on card0 - no override anywhere (the whole point).
unset MESA_LOADER_DRIVER_OVERRIDE           # env-kmsro.sh already unsets it
LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug \
  ./mesaprobe --gbm /dev/dri/card0 --render
#    expect: PROBE renderer: Mali T720 (Panfrost)
#    and in the log: no "failed to open exynos", no "failed to load swrast";
#    on the card0 fd: CREATE_DUMB + prime_handle_to_fd (renderonly allocate),
#    not fd_to_handle (kmscube takes __DRI_IMAGE_ATTRIB_HANDLE) -
#    see kms-mesa-pairing.md 3.3.

# 3. real client, same env
. ./env-kmsro.sh
timeout 20 kmscube -D /dev/dri/card0
#    expect: renderer: Mali T720 (Panfrost)
#    (the page flip still carries no fb until the int83+ flip fix lands -
#     separate defect, not a Mesa/kmsro one)
```

Failure signatures to watch for, and what each would mean:

| Log line | Meaning |
|---|---|
| `failed to open exynos` | the DRI dir in `LIBGL_DRIVERS_PATH` is not this install tree (check `env-kmsro.sh` printed paths) |
| kmsro screen NULL, then `failed to open kms_swrast` | card0 opened, exynos_dri.so loaded, but kmsro's `drmOpenWithType("panfrost", RENDER)` found nothing → render node missing or still named `panfrost7580` (kernel rename not in) |
| `Mali T720 (Panfrost)` on `--gbm /dev/dri/renderD128` but not on card0 | render node fine, kmsro join still failing → gate 2 of `kms-mesa-pairing.md` |
| `kernel driver is too old (requires at least 1.1 ...)` | wrong Mesa: that is the **system 26.3** message, not 20.3.5; `LD_LIBRARY_PATH` is not pointing at `mesa20-kmsro-install/usr/local/lib` |

`run-matrix.sh` was not modified: its `m20-*` cells take
`MESA20_B`/`LIBGL_DRIVERS_PATH` from the script itself, so the kmsro cells are
driven through `env-kmsro.sh` + `mesaprobe` as above until the matrix gets a
`MESA20_KMSRO` knob.

## 6. Selector: `code/gpu31/kms/tools/mesa/env-kmsro.sh`

`. env-kmsro.sh` → `LD_LIBRARY_PATH=$INSTALL/usr/local/lib`,
`LIBGL_DRIVERS_PATH=$INSTALL/usr/local/lib/dri`, and it **unsets**
`MESA_LOADER_DRIVER_OVERRIDE`, `EGL_PLATFORM`, `GBM_BACKEND`,
`*_ALWAYS_SOFTWARE`, `DRI_PRIME`, `PAN_MESA_DEBUG`, `LD_PRELOAD` first, so an
inherited `run-matrix.sh`/`env.sh` state cannot leak in. It prefers the install
tree and falls back to the build tree (`$B/src/egl`, `.../src/gbm`,
`.../src/gallium/targets/dri`, `.../src/mapi/*`, `.../src/glx`) if only the
build tree exists. `MESA_KMSRO_DEBUG=1` adds `LIBGL_DEBUG=verbose
EGL_LOG_LEVEL=debug`. Override the locations with `MESA20_KMSRO` /
`MESA20_KMSRO_INSTALL`.

Both branches measured on the phone:

```
$ . env-kmsro.sh; echo $LD_LIBRARY_PATH '|' $LIBGL_DRIVERS_PATH
/data/mesabuild/mesa20-kmsro-install/usr/local/lib | .../lib/dri
$ MESA20_KMSRO_INSTALL=/nonexistent . env-kmsro.sh; echo $LD_LIBRARY_PATH '|' $LIBGL_DRIVERS_PATH
/data/mesabuild/mesa20-kmsro/src/egl:...:src/gallium/targets/dri:...:src/glx | .../mesa20-kmsro/src/gallium/targets/dri
$ . env-kmsro.sh; echo ${MESA_LOADER_DRIVER_OVERRIDE-<unset>} ${EGL_PLATFORM-<unset>}
<unset> <unset>
```

## 7. What was not done / limits of this slice

* No kernel change, no reboot, no module load by this slice; the one reboot
  during the build was Main's int83 flash (build resumed in place afterwards).
* No change to `mesa20` sources and no reconfiguration of `mesa20-b`; the two
  builds are independent meson dirs off the same source tree.
* `build-tests=false` (same as `mesa20-b`'s current options) — no Mesa test
  binaries are produced.
* Disk: 410 MB added under `/data/mesabuild` (4.6 GB free after the build).
