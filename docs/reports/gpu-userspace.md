# GPU userspace — Mali-T720 (kbase) on j7elte / Exynos 7580

Scope: can any Linux userspace 3D stack be provided for the Mali-T720 on the Nura
3.10.108 kbase kernel? Ground truth: kernel tree
`android_kernel_samsung_universal7580-2a746c1…` @ baseline `b5e363a`,
`config-nura-j7elte`, ROM image `system.raw` (Android 6, `SM-J700M J700MUBU2BRC2`),
`parity/pmaports` @ `ba682bd`, and the mainline reference tree present in this session
(`linux-7.3`).

**Verdict: there is no Mali 3D userspace path for this kernel. 3D on j7elte is
software-only (Mesa llvmpipe).**

| Question | Answer | Evidence |
|---|---|---|
| Kernel interface present? | **kbase only**: `/dev/mali0`, ARM UK ioctl ABI, DDK r15p0 / UK 10.6 | §1 |
| Panfrost DRM available? | **No.** `# CONFIG_DRM is not set`; 0 `panfrost` references in the whole 3.10 tree; the 3.10 DRM core lacks prerequisites (drm_sched, GEM shmem helper, io-pgtable) | §2 |
| ROM `libGLES_mali.so` usable? | **No.** ELF32 ARM EABI5, `/system/bin/linker`, Android 23, 12 Android `NEEDED` libs; 32-bit only (no 64-bit EGL/HAL in the ROM) | §3 |
| Any open-source kbase userspace? | **Not for this kernel/GPU.** Upstream Mesa never spoke kbase; an experimental out-of-tree Mesa fork adds a `kbase` KMD backend, but only for the modern numbered-ioctl UAPI (r32p0–r44p0) on Bifrost/Valhall, with GL submission still gated off | §2, §4 |
| Kernel can run 32-bit ARM code? | Yes (`CONFIG_COMPAT=y`), but that is irrelevant without a bionic runtime | §3 |
| Implementable 3D today? | **Software rendering only**: Mesa `swrast`/llvmpipe over Xorg + `xf86-video-fbdev` (no DRM, no GBM) | §5 |
| Code delivered | `kbase-probe` (freestanding aarch64), `check-3d.sh` | §7 |
| Patches | **None** — no kernel/userspace change can create a GL driver here | §8 |

All verification here is **static plus cross-compilation**; no j7elte hardware was
available, so nothing was run on the target. The probe binary was smoke-tested under
`qemu-aarch64` on the host only (§7).

---

## 1. What the kernel actually exposes: kbase, not DRM

The kernel builds Samsung's downstream ARM kbase driver (DDK r15p0), as established in
`parity/reports/gpu.md`:

- `CONFIG_MALI_MIDGARD=y` (config:2284), `CONFIG_MALI_T7XX=y` (2288),
  `CONFIG_MALI_R15P0=y` (2290); release name `r15p0-00rel0`
  (`drivers/gpu/arm/t7xx/r15p0/Kbuild:18`).
- Device node: a **misc** char device, not a DRM device.
  `kbdev->mdev.name = kbdev->devname` (`mali_kbase_core_linux.c:4001`), where
  `devname = kbase_drv_name + kbase_dev_nr` (`:3873`) and
  `kbase_drv_name = "mali"` (`mali_kbase_strings.c:19`) ⇒ **`/dev/mali0`**.
  Registration is `misc_register()` (`mali_kbase_core_linux.c:4006`).
- The ABI is ARM's "UK" ioctl ABI: the file ops are
  `kbase_fops = { .unlocked_ioctl = kbase_ioctl, .compat_ioctl = kbase_ioctl, … }`
  (`:1665-1666`). `kbase_ioctl()` copies an opaque argument buffer sized by
  `_IOC_SIZE(cmd)` and dispatches on a `u32` function id inside it (`:1342-1367`);
  `kbase_dispatch()` *requires* a version handshake, then `SET_FLAGS`, before any other
  call (`:279-340`).
- UK version: `BASE_UK_VERSION_MAJOR 10`, `BASE_UK_VERSION_MINOR 6`
  (`mali_kbase_uku.h:69-70`). The ROM's userspace blob is UK 10.0 / DDK `r7p0`
  (`parity/reports/gpu.md` §7 R1). `GET_VERSION` returns
  `"K:" MALI_RELEASE_NAME "(GPL)"` (`mali_kbase_core_linux.c:111,719`), i.e.
  `K:r15p0-00rel0(GPL)` for the Nura build.
- No DRM: `# CONFIG_DRM is not set` (`config-nura-j7elte:2262`). The video path is
  fbdev: `CONFIG_EXYNOS_DECON_FB=y` (config:2358), `CONFIG_EXYNOS_MIPI_DSI=y`,
  `drivers/video/exynos/decon_7580/`. The 3.10 `drivers/gpu/drm/exynos/` driver contains
  only FIMD and MIXER (Exynos 4/5) — no DECON and no MIPI-DSI encoder at all — so even
  enabling `CONFIG_DRM` would not yield a KMS device for this panel; DSIM/panel live in
  the fbdev path (`drivers/video/exynos/exynos_mipi_dsi*.c`, `decon_7580/`).

A userspace GL driver therefore has exactly one kernel interface to talk to: the
kbase UK ABI on `/dev/mali0`. There is no render node.

## 2. Why Mesa Panfrost cannot be used

Mesa's `panfrost` Gallium driver is the only open-source Mali Midgard driver (it supports
T720 as Midgard v4 with **OpenGL ES 2.0 / OpenGL 2.1** — Mesa documentation,
<https://docs.mesa3d.org/drivers/panfrost.html>). It does not speak kbase. It requires the
kernel's **panfrost DRM driver** and its `DRM_IOCTL_PANFROST_*` UAPI
(`include/uapi/drm/panfrost_drm.h` in mainline; `DRM_PANFROST_SUBMIT`, `CREATE_BO`,
`MMAP_BO`, `GET_PARAM`, `JM_CTX_CREATE`, …). That driver first appeared in Linux 5.2
(<https://www.phoronix.com/news/Linux-5.2-DRM-Update>) and has never existed for this tree.

Checks against the 3.10 tree:

| Requirement (mainline `drivers/gpu/drm/panfrost/Kconfig`) | In 3.10 tree | Status |
|---|---|---|
| `DRM` | `# CONFIG_DRM is not set` (config:2262); subsystem present but unbuilt | missing |
| `DRM_SCHED` | `include/drm/drm_sched.h`, `drivers/gpu/drm/drm_sched/` absent | missing |
| `DRM_GEM_SHMEM_HELPER` | `include/drm/drm_gem_shmem_helper.h` absent | missing |
| `IOMMU_IO_PGTABLE_LPAE` | `drivers/iommu/io-pgtable*.{c,h}` absent (3.10 has only the vendor Exynos IOMMU) | missing |
| `PM_DEVFREQ` | `CONFIG_PM_DEVFREQ=y` (3264) | present |
| DT compatible `arm,mali-t720` | this DT says only `compatible = "arm,mali"` (`arch/arm64/boot/dts/exynos7580.dtsi:1066-1076`) | not matched |
| GPU not owned by another driver | kbase owns the same `mali@0x11400000` registers, power domain `pd-g3d`, clocks and `vdd_g3d` | conflict |

Additional structural blockers:

- Panfrost is a **render-only** DRM driver; showing anything needs a KMS display driver.
  On this kernel the display stack is fbdev (`CONFIG_EXYNOS_DECON_FB=y`), and the 3.10
  DRM/Exynos driver has no DECON/DSIM for 7580 (§1). A panfrost backport would therefore
  also require writing a DRM/KMS driver for DECON 7580 + DSIM + the ea8061 panel.
- The GPU's own MMU and job scheduling would have to move from kbase to panfrost; the two
  cannot both drive the T720. Kbase would have to be disabled — losing the only interface
  the ROM's (and any ARM) userspace DDK speaks.

There is no `panfrost` string anywhere in the 3.10 tree (`grep -ril panfrost` = 0 hits),
and no `DRM_PANFROST` Kconfig symbol.

### 2.1 Prior art: the experimental Mesa "kbase" KMD backend

The only known attempt to run Mesa's Mali driver on kbase instead of a DRM driver is an
out-of-tree Mesa fork (`-Dpanfrost-kmds=kbase,panthor`, `src/panfrost/lib/kmod/kbase_kmod.c`,
e.g. <https://github.com/leegao/mesa-funnymdzz>). It does not apply here:

- It targets **modern kbase**, described in its own notes as "r32p0–r44p0" with the
  numbered-ioctl UAPI: CSF (arch ≥ 10) `VERSION_CHECK` ioctl nr 52, JM (arch ≤ 9) nr 0.
  This kernel's kbase is the older **legacy UK** generation (single ioctl, in-buffer
  function IDs, UK 10.6). Different ABI generation, different memory/VM semantics.
- Its target device is Pixel 7 / Mali-G710 (Valhall CSF); the JM path assumes kbase
  generations from 2018+, not a 2016 r15p0 Midgard driver.
- Its own documentation says **GL submission is gated off** ("no submission support
  yet", bypass with an env var), there is no WSI/dma-buf import, and submission is a
  synchronous CPU-polled model. It is a research fork with no release and no support
  for Midgard.

So even the newest userspace-only idea does not provide a usable GL stack for this
device, and building such a backend for legacy UK 10.6 + Midgard would be a multi-month
driver project with no hardware to validate it on.

## 3. The ROM's `libGLES_mali.so` cannot be reused

Extracted from `system.raw` (Android 6 image, `/vendor/lib/egl/libGLES_mali.so`,
23,368,296 bytes, sha256 `62e10725f1077ffa483224de5a83c14a0a0ccdccb51cd094177b7bd1630be317`):

```
ELF 32-bit LSB shared object, ARM, EABI5 version 1 (SYSV), dynamically linked,
interpreter /system/bin/linker, for Android 23, stripped
Class: ELF32   Machine: ARM   Type: DYN   Flags: 0x5000000 (EABI5)
DT_NEEDED: libz, libdl, libhardware, libui, libutils, libcutils, libbinder,
           liblog, libc++.so, libion, libc, libm
undefined syms include: __android_log_print, __android_log_write,
           hw_get_module, ion_open, ion_alloc_fd, ion_close
version string: "1.4 Midgard-\"r7p0-03rel0\""
build path:     hardware/samsung_slsi/MaliT720_r7p0_Insel/…
```

Consequences:

1. **Wrong ELF class for every Nura process.** AArch64 processes cannot `dlopen` an
   EM_ARM (AArch32) object; the blob can only ever be loaded by a 32-bit ARM process.
2. **It is an Android/bionic driver, not a Linux one.** The interpreter is
   `/system/bin/linker` (bionic), and it needs Android framework libraries
   (`libbinder`, `libui`, `libutils`, `libcutils`, `libhardware`, `libion`, `libc++`),
   i.e. a full Android 6 runtime with `servicemanager`, surfaceflinger, gralloc and
   hwcomposer — its EGL surface path is Android `ANativeWindow`/gralloc, not Wayland/X11.
   This is confirmed by the blob's own imports (`hw_get_module`, `ion_alloc_fd`).
3. **The ROM has no 64-bit GL/HAL variant.** `/lib64/egl` does not exist, `/vendor/lib64`
   is empty, `/lib/egl/egl.cfg` is `0 0 mali`, and the HAL modules
   `gralloc.exynos5.so` (sha256 `9a82234…`) and `hwcomposer.exynos5.so`
   (sha256 `6d46e5d…`) are also ELF32/ARM/Android-23. There is nothing aarch64 to link
   against even if the ABI were otherwise compatible.
4. **Kernel can run AArch32, but that does not help.** `CONFIG_COMPAT=y` (config:419),
   `CONFIG_ANDROID_BINDER_IPC=y` (3444), `CONFIG_ASHMEM=y` (3167), `CONFIG_ION=y` (3177)
   mean a 32-bit Android userland could in principle boot in a chroot — but that is the
   stock Android OS, not a GPU driver for postmarketOS: no Nura application can use its
   GL, and presenting its output would need a libhybris/Halium-style bridge (gralloc +
   hwcomposer → Wayland), none of which exists for aarch64 musl.
5. **Revision mismatch on top of all this.** The Nura kernel builds DDK r15p0 / UK 10.6;
   the blob is r7p0 / UK 10.0. The standard function IDs are compatible, but the Samsung
   vendor block is renumbered (`KBASE_FUNC_HWCNT_UTIL_SETUP` +37→+41, `VSYNC_SKIP`
   +39→+43, `CREATE_SURFACE` +40→+44, …; `KBASE_FUNC_EXT_BUFFER_LOCK` +17 removed) — see
   `parity/reports/gpu.md` §7 R1. Whether the blob issues those calls is not statically
   known, so even the chroot route carries an unverified break risk.

For reference, the ARM **Linux** Midgard DDK (the only userspace that legitimately speaks
kbase outside Android) is closed-source, was distributed only as 32-bit armhf/glibc
libraries to SoC customers, is not present in the ROM, and cannot be redistributed in
postmarketOS. It would also have to match the kernel's DDK generation.

## 4. Paths considered, and why none produces hardware 3D

| Path | Needs | Result |
|---|---|---|
| **A. Mainline kernel + panfrost** | Mainline Exynos 7580 SoC support: clock/pinctrl/PMU, DECON+DSIM+panel DRM/KMS driver, GPU power/DVFS OPP, board DT; then a `gpu@11400000` node with `compatible = "…","arm,mali-t720"` | Only route to *working hardware GL*. No Exynos 7580 support exists in mainline even at 7.3 (no `exynos7580.dtsi`, no `clk-exynos7580.c`, no DECON 7580 DRM driver; nearest sibling is `exynos7870-j7xelte.dts` with a T830 node). **6–18 person-months** of SoC/display bring-up, then GLES 2.0-class acceleration. Out of scope for a userspace gap. |
| **B. Backport panfrost DRM to 3.10** | DRM core modernisation (drm_sched, GEM shmem, dma-fence, atomic helpers), `io-pgtable-arm`, a 7580 KMS display driver, and disabling kbase | **>12 person-months**, unmaintainable fork, no upstream. Not recommended. |
| **C. ARM Linux Midgard DDK over kbase** | Proprietary r7p0/r15p0 armhf/glibc blob + 32-bit rootfs; exact DDK match | Blob **not available** (not in ROM, not public, not redistributable); 32-bit only; no Wayland. Blocked. |
| **D. Android userland in a chroot / libhybris** | 32-bit bionic runtime + binder services + gralloc/hwcomposer + a bridge into a Linux compositor | Runs the ROM's Android GL in a container, gives Nura apps nothing; no musl/aarch64 libhybris; research effort. Rejected. |
| **E. DRM "shim" translating panfrost ioctls onto kbase internals** | A new in-kernel GPU driver: kbase exposes no kernel API for GPU memory/VM or job submission (region tracker/MMU/atom entry points are internal; the `KBASE_EXPORT_TEST_API` wrappers compile to nothing without `MALI_KERNEL_TEST_API`), and it owns the GPU | Not userspace work; effectively writing a second GPU driver. Rejected. |
| **F. Experimental Mesa `kbase` KMD backend (out-of-tree fork)** | Userspace support for this kernel's legacy UK 10.6 Midgard ABI (the fork covers only the modern numbered-ioctl UAPI, r32p0+, and has GL submission gated off) | **Blocked**: would be a multi-month legacy-ABI + Midgard implementation in a research fork, untestable without hardware. |
| **G. Mesa software rendering** | `mesa-dri-gallium` (swrast/llvmpipe) + Xorg + `xf86-video-fbdev` on `/dev/fb0` | **Works today, CPU-only.** Implemented/documented here (§5). |

## 5. The only working 3D path: software Mesa on fbdev

There is no DRM device, so GBM/KMS (`kms_swrast`) and all Wayland compositors (wlroots,
Weston without its removed fbdev backend, etc.) are unavailable. Mesa's classic
`swrast`/llvmpipe, however, needs no DRM: with an X server on the fbdev driver, GLX is
served by llvmpipe.

Verified package availability for Alpine/pmOS **aarch64** (thus musl, thus installable
on Nura):

- `mesa-dri-gallium` 26.2.5-r0, edge/main aarch64, contains `/usr/lib/dri/swrast_dri.so`
  (and `panfrost_dri.so`, `kms_swrast_dri.so`, …)
  — <https://pkgs.alpinelinux.org/contents?branch=edge&name=mesa-dri-gallium&arch=aarch64&repo=main&page=1>
- `xf86-video-fbdev` 0.5.0-r6, edge/community aarch64
  — <https://pkgs.alpinelinux.org/packages?name=xf86-video-fbdev&branch=edge&arch=aarch64>

Minimal Xorg configuration for fbdev (untested on hardware; the display parity report
notes the panel/fbdev kernel side is at parity):

```ini
# /etc/X11/xorg.conf.d/10-fbdev.conf
Section "Device"
    Identifier "Fbdev"
    Driver     "fbdev"
    Option     "fbdev" "/dev/fb0"
EndSection

Section "Monitor"
    Identifier "Panel"
EndSection

Section "Screen"
    Identifier "Screen0"
    Device     "Fbdev"
    Monitor    "Panel"
EndSection
```

Expected result: `glxinfo -B` reports `llvmpipe` and a software renderer; EGL/GLES work
for X clients and for headless (`EGL_PLATFORM_SURFACELESS_MESA`) use. Performance on a
Cortex-A53 at 720×1280 is CPU-bound; this is not Mali acceleration and must not be
described as such.

## 6. Effort summary for hardware 3D

- **Minimum honest route**: mainline Exynos 7580 port (SoC + DECON/DSIM/panel KMS + GPU
  node) with panfrost. No upstream 7580 support exists, so the estimate is
  **6–18 person-months** for someone experienced with Exynos mainlining, plus
  panfrost/Mesa validation. Payoff: GLES 2.0 / GL 2.1.
- **Anything on the 3.10 kbase kernel**: not achievable by userspace work at all. The
  userspace half of the Mali stack would have to be ARM's proprietary DDK, which is
  unavailable and 32-bit/glibc-only; the open-source half (Mesa) requires a kernel driver
  this kernel does not have.

## 7. Delivered code

Both files are diagnostic, not a driver:

- `parity/userspace/gpu/kbase-probe/kbase-probe.c` — freestanding aarch64 (no libc, raw
  syscalls; 3.4 KiB `.text`, static, no interpreter). It:
  1. reports `/dev/mali0`, `/dev/dri/renderD128`, `/dev/dri/card0`, `/dev/fb0`;
  2. performs the required kbase UK sequence on `/dev/mali0`:
     `UKP_FUNC_ID_CHECK_VERSION` (`mali_uk.h:85`, id 0) with major/minor 10.6,
     then `KBASE_FUNC_SET_FLAGS` (512+18) with `create_flags = 0`, then
     `KBASE_FUNC_GET_VERSION` (512+16) — all sizes matching the kernel's `sizeof()`
     checks (`struct uku_version_check_args` = 24 B, `kbase_uk_set_flags` = 24 B,
     `kbase_uk_get_ddk_version` = 88 B);
  3. prints the kernel DDK version string and a verdict about DRM absence.
  These three calls do not submit work or power the GPU cores.
  Build: `make` in that directory (default `aarch64-linux-gnu-gcc`; override with
  `CROSS_COMPILE=…`). Host smoke test under `qemu-aarch64` (x86_64 host, no Mali; the
  `/dev/dri` node present is the host's own GPU):
  ```
  kbase-probe: Mali kernel-interface probe (kbase vs DRM)
  ------------------------------------------------------
  /dev/mali0                  = -2 (open failed)
  /dev/dri/renderD128         = 3
  /dev/dri/card0              = -2 (open failed)
  /dev/fb0                    = -13 (open failed)

  kbase-probe: /dev/mali0 not available; is the kbase driver bound to 11400000.mali?
  ```
  On the device it should print `handshake 10.6 -> 10.6`, `GET_VERSION … K:r15p0-00rel0(GPL)`
  and `no DRM node (Mesa Panfrost/GBM cannot bind)`.
- `parity/userspace/gpu/check-3d.sh` — on-device, read-only status script: device nodes,
  platform device/debugfs/sysfs, loaded `mali*` modules, DRM binding, dmesg lines,
  installed Mesa/Xorg packages, `eglinfo`, then runs the probe and prints the
  interpretation.

No Xorg config file is installed by these artifacts; the snippet in §5 is documentation
only (display userspace is a separate gap).

## 8. Patches

**None.** `parity/patches/userspace/gpu/` is intentionally absent: no source change can
give this kernel a GL driver. The kernel half is already complete (kbase r15p0 —
`parity/reports/gpu.md`), and the userspace half is impossible without (a) a panfrost DRM
kernel (mainline port) or (b) a proprietary, unavailable, 32-bit Android/glibc DDK.

## 9. Verification status / residual risks

- Static analysis + cross-compilation only; no j7elte hardware, no kernel build, no
  runtime GL test. The probe's correctness on-device depends on the kbase sequence above;
  its expected output is derived from the in-tree handlers, not observed.
- If the ROM DDK is ever wanted bit-exactly, the config change in
  `parity/reports/gpu.md` §7 R1 (`DDK_VERSION_OS="m"`, `MALI_T72X=y`, `MALI_R7P0_T72X=y`)
  is the only lever — and it still yields no Linux GL userspace.
- Alpine package versions are from the live package index on 2026-10-24; pmOS uses the
  same Alpine binary packages, so availability holds unless the packages are dropped.
