# GPU RE / port assessment — hardware 3D on Mali-T720 (j7elte, Exynos 7580)

Scope: the ROM `SM-J700M J700MUBU2BRC2` (Android 6, kernel 3.10.61) GL stack vs
the Nura tree (`android_kernel_samsung_universal7580-…` @ 3.10.108, panfrost
reference `linux-7.3`). Static analysis plus host-side execution under
`qemu-arm`; **no j7elte hardware was available, so no hardware test and no
acceleration claim is made**. Software GL is already packaged
(`j7elte-gpu-soft`); this report covers only the hardware path.

**Verdict**

| Path | Result | Evidence |
|---|---|---|
| **(a) Panfrost/DRM on 3.10** | Not feasible as a backport; the sustainable route is a mainline Exynos 7580 + panfrost port, which does not exist upstream (6–12+ person-months) | §2 |
| **(b) libhybris / 32-bit Android compatibility** | **Technically demonstrable, not turnkey.** The ROM blob executes as ARM32 and speaks the kbase UK ABI byte-for-byte (prototype, §3.3). But it is an Android-23 bionic HAL stack with Android surfaces, the ROM has **no 32-bit userland** in Nura, and the kernel's DDK (r15p0) renumbers the vendor UK block the blob uses (r7p0) | §3 |
| **(c) ARM Linux Midgard DDK** | Not viable: no T720 artifact exists; the full DDK is commercial-licence only and the public binaries are board-specific EULA builds | §4 |

Recommendation: **path (a)** is the only realistic route to maintainable
hardware 3D. Path (b) is viable only as a stopgap that drags a 32-bit Android
container and the closed HAL stack along, plus a kernel DDK-revision change
(§5).

---

## 1. The ROM's GL stack (extracted, hashed, ABI-checked)

All paths below are the on-device ones. In `system.raw` the **image root is
`/system`** (`/bin/linker` is `/system/bin/linker`); `/vendor` is a separate
tree inside the same partition. Reproduce with
`parity/userspace/gpu/re-hybris/extract-rom-gles.sh`; full manifest in
`extract-manifest.txt`.

| On-device path | Class / ABI | Size | sha256 (first 16) | Interp | NEEDED |
|---|---|---|---|---|---|
| `/vendor/lib/egl/libGLES_mali.so` | ELF32 ARM EABI5, DYN, BIND_NOW | 23,368,296 | `62e10725f1077ffa…` | `/system/bin/linker` | 12 libs (below) |
| `/system/bin/linker` | ELF32 ARM EABI5, DYN | 190,292 | `601ffb95aec1d509…` | — | none |
| `/system/lib/hw/gralloc.exynos5.so` | ELF32 ARM EABI5, DYN | 26,576 | `9a8223432a1e1231…` | `/system/bin/linker` | liblog, libcutils, libion, libutils, libc++, libdl, libc, libm |
| `/system/lib/hw/hwcomposer.exynos5.so` | ELF32 ARM EABI5, DYN | 30,336 | `6d46e5d5e283802f…` | `/system/bin/linker` | 20 libs (below) |
| `/system/lib/libc.so` | ELF32 ARM EABI5, DYN | 678,392 | `204785e4dafadfc0…` | `/system/bin/linker` | libdl |
| `/system/lib/libdl.so` | ELF32 ARM EABI5, DYN | 9,492 | `391cae6ad2d31de2…` | `/system/bin/linker` | none |

`libGLES_mali.so` NEEDED (in order): `libz, libdl, libhardware, libui,
libutils, libcutils, libbinder, liblog, libc++, libion, libc, libm`.
`hwcomposer.exynos5.so` NEEDED: `liblog, libcutils, libEGL, libGLESv1_CM,
libhardware, libhardware_legacy, libutils, libsync, libexynosv4l2,
libexynosutils, libhwcutils, libdisplay, libhdmi, libmpp, libexynosgscaler,
libExynosHWCService, libc++, libdl, libc, libm` — i.e. the whole Samsung
display/HWC stack plus a **binder client** (`libExynosHWCService`). All 20 are
present in `/system/lib`.

Other facts:

* The blob is **full-relocation (`BIND_NOW`, `DF_1_NOW`)**: 96,168 packed
  `.rel.dyn` entries (Android **APS2**, SLEB128) = 88,977 `R_ARM_RELATIVE` +
  6,437 `R_ARM_ABS32` + 754 `R_ARM_GLOB_DAT`, plus 10,825 `R_ARM_JUMP_SLOT`
  from `.rel.plt`; 15,008 dynsyms of which **284 are undefined**. It is
  stripped, carries `GNU_HASH` and `VERDEF/VERNEED` (3 needs).
* Build identity: `strings` → `U:r7p0-03rel0`,
  `hardware/samsung_slsi/MaliT720_r7p0_Insel/product/...`; the library's own
  error text says *"The DDK was built for 0x720 r1p0 status range [0..15]"*.
* `/lib/egl/egl.cfg` = `0 0 mali`; the Android loaders `libEGL.so`,
  `libGLESv1_CM.so`, `libGLESv2.so`, `libGLESv3.so` exist as ARM32 in
  `/system/lib`.
* **There is no 64-bit GL stack:** `/system/lib64` contains only
  `libSecMMCodec.so`; there is no `/system/lib64/egl`, no `/vendor/lib64`.
  Every GL/HAL object in this ROM is ELF32/ARM Android-23.

## 2. Path (a) — Panfrost/DRM

Panfrost is the only open-source Midgard driver, and mainline does claim the
T720 (`linux-7.3/drivers/gpu/drm/panfrost/panfrost_drv.c:1167`,
`{ .compatible = "arm,mali-t720" }`). It requires the `panfrost` DRM driver and
`DRM_IOCTL_PANFROST_*` UAPI, none of which exists in this 3.10 tree.

What the 3.10 tree actually has:

| Panfrost prerequisite | In the Nura 3.10 tree | Status |
|---|---|---|
| `CONFIG_DRM` | `# CONFIG_DRM is not set` (`config-nura-j7elte:2262`) | missing |
| `drm_sched` | no `include/drm/drm_sched.h`, no `drivers/gpu/drm/drm_sched/` | missing |
| GEM shmem helper | `include/drm/` has only `drm_gem_cma_helper.h`; no `drm_gem_shmem_helper.h` | missing |
| `io-pgtable-arm` | `drivers/iommu/` has only `omap-iopgtable.h`; no `io-pgtable*.c` | missing |
| `panfrost` Kconfig/DT match | 0 `panfrost` references in the whole tree | missing |
| KMS display driver for the panel | 3.10 `drivers/gpu/drm/exynos/` = FIMD/MIXER/HDMI/G2D/GSC/FIMC only; **no DECON/DSIM** (grep `decon` in `drivers/gpu/drm/` hits only a TTM comment); the 7580 panel path is fbdev (`decon_7580/`, `exynos_mipi_dsi*`) | missing |
| GPU free for another driver | kbase (r15p0) owns `mali@0x11400000`, IRQ 281/282/283, `pd-g3d`, clocks and `vdd_g3d` | conflict |

Mainline 7.3 does **not** support Exynos 7580: there is no `exynos7580.dtsi`
and no `clk-exynos7580.c`. The nearest sibling is Exynos 7870
(`arch/arm64/boot/dts/exynos/exynos7870.dtsi:240`, GPU
`"samsung,exynos7870-mali","arm,mali-t830"`), which is a *different* SoC/GPU
generation.

**Design sketch (not implemented, per assignment).** A mainline-style port
would need, in order: (1) `clk-exynos7580` + PMU/pinctrl; (2) DECON 7580 +
DSIM + ea8061 panel as a DRM/KMS driver (the fbdev driver is not reusable);
(3) a `gpu@11400000` node with `"arm,mali-t720"`, GPU power domain and
DVFS/OPP; (4) kbase removed from the build so panfrost can own the block;
(5) Mesa panfrost validation. Steps 1–2 alone are the bulk of the work; with
7870 as the template, **6–12 person-months** for a minimal SoC+KMS port and
**12–18 person-months** for a usable GLES2/GL2.1 stack is the honest range
(this is an estimate, not a measurement). A 3.10 backport is strictly worse:
it would also require modernising DRM core (dma-fence, atomic helpers,
drm_sched, GEM shmem), and would have no upstream.

## 3. Path (b) — libhybris / 32-bit Android compatibility

### 3.1 Kernel prerequisites are present

`CONFIG_COMPAT=y` (419), `CONFIG_ANDROID_BINDER_IPC=y` (3444), `CONFIG_ASHMEM=y`
(3167), `CONFIG_ION=y` (3177), `CONFIG_SYNC=y`/`CONFIG_SW_SYNC=y` (3173–3174).
A 32-bit Android userland *can* run on this kernel. The blocker is the
userspace, not the kernel ABI.

### 3.2 The blob is 32-bit-only, bionic-only

* ELF32/EM_ARM/EABI5 with `PT_INTERP=/system/bin/linker`: an aarch64 process
  cannot `dlopen` it (wrong ELF class), so **libhybris cannot load it
  in-process into any Nura aarch64 application**. libhybris's native-libc
  support (README: "glibc, musl") is irrelevant here — the architecture must
  match the blob, i.e. a 32-bit ARM process.
* Its EGL surface path is Android (`ANativeWindow`/gralloc): imports include
  `hw_get_module`, `ion_open/ion_alloc_fd`, `libui`, `libbinder`, `libhardware`.
  There is no Wayland/X11 code in it.
* The ROM's `hwcomposer.exynos5.so` is a binder client of the Samsung HWC
  service and pulls in `libexynosv4l2`, `libdisplay`, `libmpp`, `libhdmi`,
  `libExynosHWCService`.

### 3.3 What we actually proved (host, qemu-arm, no hardware)

**(i) The ROM's bionic runtime does not come up under qemu.**
A freestanding ARM32 executable (`proto/`) linked against
`libGLES_mali.so` + the ROM's `linker`/`libc`/`libdl`/`libm` makes the ROM's
own `/system/bin/linker` map the blob and then every NEEDED library from
`/system/lib` (`logs/bionic-linker.log`: `openat("/vendor/lib/egl/libGLES_mali.so")`,
then `libc.so`, `libz.so`, `libhardware.so`, `libui.so`, `libutils.so`, …).
Startup then fails inside bionic, before any blob code runs. Two failure
modes were observed, both reproduced from a clean extraction: the linker
blocks on a futex at `linker_base + 0x1e110`, which its symbol table names
`__dl__ZL10g_dl_mutex` (base derived from a traced PC `0x40810f30 - 0xff30`,
the offset of `__dl__ZL33__pthread_mutex_lock_with_timeout`) during a nested
`dlopen("libnetd_client.so")` from libc's constructor; and, with the
unmodified rootfs and the current harness, `page record for 0x… was not found
(block_size=16)` followed by `exit_group(1)`. The stock ROM binary
`app_process32` fails the same way in this rootfs. This is a qemu-user/bionic
limitation, **not a device finding**. Consequence: no EGL call can be made
through the ROM's bionic runtime under qemu; only the standalone loader
(§3.3 ii) can execute the blob on the host.

**(ii) A freestanding loader runs the blob anyway** (`miniloader/`,
`run-prototype.sh`, `logs/miniloader.log`). It maps the image at a fixed base,
applies all 96,168 APS2 + 10,825 PLT relocations, resolves the 284 undefined
symbols to local stubs, and calls the exports:

```
exports eglGetDisplay=0x20469910 eglQueryString=0x2046ae14 eglGetProcAddress=0x2046af1c eglInitialize=0x204699a8
eglGetDisplay(EGL_DEFAULT_DISPLAY) = 0x44802580      # display object allocated by the blob
eglQueryString(NO_DISPLAY, EGL_CLIENT_APIS) = (null) # uninitialised display, as expected
eglInitialize(dpy) = 0                                # no driver: no GPU properties
```

The blob's own log (its `__android_log_print` output, captured by the
miniloader) confirms the build and the failure point:

```
CDBG: … mali_cdbg_env.c line: 586 cdbgp_populate_from_system_environment … failed (3)
ERROR: The DDK is not compatible with any of the Mali GPUs on the system.
The DDK was built for 0x720 r1p0 status range [0..15], but none of the GPUs matched:
```

This is exactly the expected result with no kbase: it proves the ARM32
userspace executes and reaches the GPU-compatibility gate, and it gives us the
blob's kbase behaviour without a device.

**(iii) The kbase UK ABI, captured byte-for-byte.** The same run intercepts the
blob's `/dev/mali0` traffic (the miniloader fakes the fd and the ioctls):

```
open("/dev/mali0", O_RDWR|O_CLOEXEC)
fstat(fd) -> S_IFCHR
ioctl(fd, 0xC0108000, 16)  id=0   UKP_FUNC_ID_CHECK_VERSION  major=10 minor=0
mmap(0, 4096, PROT_NONE, MAP_SHARED, fd, 0)
ioctl(fd, 0xC0108212, 16)  id=530 KBASE_FUNC_SET_FLAGS      create_flags=0
```

So: the blob is **UK 10.0** (matches r7p0), it sends `major=10 minor=0`, and
the r15p0 kernel's handshake would answer `10.0` — **the handshake is
compatible**. The cmd encoding matches `kbase_ioctl`'s
`_IOC(_IOC_READ|_IOC_WRITE, type, nr, size)` (`drivers/gpu/arm/t7xx/r15p0/mali_kbase_core_linux.c:1341`).

### 3.4 UK id analysis — where r7p0 vs r15p0 actually breaks

Standard ids (512+0 … 512+36) are identical in r7p0
(`drivers/gpu/arm/t72x/r7p0/mali_kbase_uku.h:408-468`) and r15p0
(`drivers/gpu/arm/t7xx/r15p0/mali_kbase_uku.h:480-536`). The Samsung vendor
block is **renumbered**:

| id | r7p0 (blob) | r15p0 (Nura kernel) | Consequence on r15p0 |
|---|---|---|---|
| 549 | `HWCNT_UTIL_SETUP` | `SET_PRFCNT_VALUES` | different function |
| 550 | `HWCNT_GPR_DUMP` | `SOFT_EVENT_UPDATE` | different function |
| 551 | `VSYNC_SKIP` | `MEM_JIT_INIT` | different function |
| **552** | **`CREATE_SURFACE`** | `TLSTREAM_ACQUIRE` | size check: 16 vs 24 → `-EINVAL` |
| **553** | **`DESTROY_SURFACE`** | `HWCNT_UTIL_SETUP` | **not dispatched at all** with `CONFIG_MALI_SEC_HWCNT` off → `-EINVAL` |
| 554 | `SET_MIN_LOCK` | `HWCNT_GPR_DUMP` | — |
| 555 | `UNSET_MIN_LOCK` | `VSYNC_SKIP` | — |
| 556 | `TMU_SKIP` | `CREATE_SURFACE` | — |
| 567/568 | `SECURE/NON_SECURE_WORLD_RENDERING` | same | identical |

The blob **does** use 553: at blob vaddr `0x46e3c8` it builds
`{id=0x229 (553), size=0x18}` and issues the UK ioctl. A static scan of all 23
call sites of the blob's UK ioctl wrapper (`0x265480`) recovered ids
`0, 513, 514, 515, 517, 518, 521, 522, 526, 530, 535, 539, 553, 567, 568`;
**553 is the only one whose meaning changed**, and 552 (CREATE_SURFACE) is in
the same vendor block.

Severity: on the Nura kernel (`CONFIG_MALI_SEC_HWCNT` **not set**,
`config-nura-j7elte:2298`), r15p0's `kbase_dispatch` only compiles the
vendor-block cases for CREATE/DESTROY/SET_MIN_LOCK/UNSET_MIN_LOCK/TMU/SECURE
(`mali_kbase_core_linux.c:1019-1034`); 553 falls to
`default: dev_err("unknown ioctl 553")` → `-EINVAL`. 552 reaches
`KBASE_FUNC_TLSTREAM_ACQUIRE`, whose `sizeof` check
(`mali_kbase_core_linux.c:929-940`, struct = 16 bytes,
`mali_kbase_uku.h:356-362`) rejects the blob's 24-byte message. Both are
**graceful errors, not memory corruption** — but surface create/destroy
silently stops working. Exact DDK parity requires building the ROM's revision:
`CONFIG_DDK_VERSION_OS="m"`, `CONFIG_MALI_T72X=y`, `CONFIG_MALI_R7P0_T72X=y`
(reported in `gpu.md` §7 R1; this is a shared-config change, not applied here).

Note also that our runtime trace stops after `SET_FLAGS`; the blob never
reaches `GET_VERSION`/job submission because the miniloader cannot supply GPU
properties. The ioctl sequence beyond that point is **not** observed, only
statically identified.

### 3.5 What a libhybris / 32-bit bridge would actually need

1. **A 32-bit ARM userspace** (musl or glibc) on the device: a 32-bit
   rootfs/chroot or container, or a dedicated 32-bit helper process. Nura
   itself is aarch64; nothing in the existing image can host the blob.
2. **libhybris built for armv7** against Android-6 headers
   (`utils/extract-headers.sh`), plus the hybris bionic patches; the blob's
   `PT_INTERP` is `/system/bin/linker`, so the bionic linker + `libc.so` must
   be shipped and isolated.
3. **gralloc + hwcomposer** from the ROM: a binder namespace with
   `servicemanager`, the `hwcomposer.exynos5` service (via
   `libExynosHWCService`), `libexynosv4l2`/`libdisplay`/`libmpp` — i.e. a
   Halium-style Android container, not a single shim library.
4. **A presentation bridge**: gralloc buffers are `ion`/dma-buf; the HWC output
   must be composited into Wayland (or written to `/dev/fb0`). Halium/libhybris
   do this with a compositor plugin (e.g. `qt5-qpa-hwcomposer-plugin`) and a
   gralloc→`linux-dmabuf` path — both would themselves have to be **32-bit**,
   or split behind an IPC boundary.
5. **Kernel DDK revision parity** (§3.4) or a vendor-id remap.

### 3.6 Blockers (precise)

* B1 — blob is ELF32/ARM only; no 64-bit GL/HAL variant in the ROM; Nura has
  no 32-bit userland.
* B2 — EGL surfaces are Android `ANativeWindow`/gralloc; no Wayland/X11 path
  in the blob.
* B3 — `hwcomposer.exynos5` needs binder services + 16 Samsung display libs;
  none of that stack exists in Nura.
* B4 — r7p0 userspace vs r15p0 kernel vendor-id renumbering (553/552,
  §3.4); fixable by kernel config (build r7p0) but that is a shared-config
  change.
* B5 — no way to validate the bionic runtime under qemu (linker/`g_dl_mutex`
  deadlock, §3.3(i)); validation needs the device or a 32-bit userspace.
* B6 — no hardware available for this assessment; nothing here is a hardware
  test.

## 4. Path (c) — ARM Linux Midgard DDK

* **Closed source.** The functional GLES/CL userspace is the Mali DDK; ARM
  states it is "provided under the standard Arm commercial licence to all Mali
  GPU customers" (developer.arm.com, *Mali 5th Gen GPU Architecture*). Only
  the kernel side (kbase) is GPL.
* **Public binaries exist only for named development boards**, under a EULA,
  and only from r6p0 for Midgard: the download page lists Odroid-XU3
  (T628/Exynos 5422, armv7), Juno (T624), Firefly RK3288 (T760), HiKey
  (Mali-450), Odroid-N2+ (G52), HiKey 960 (G71), gem5 — **no T720/Exynos 7580
  package**. Older packages are under a restrictive EULA with no
  redistribution.
* **Absent from the ROM.** The ROM ships only the Android
  `/vendor/lib/egl/libGLES_mali.so`; there is no `/usr/lib/libmali.so`,
  no GLES/EGL loader for a Linux libc, and no armhf/glibc blob anywhere in
  `system.raw`.
* **Revision coupling.** Any DDK binary is tied to a kbase UK generation; the
  Nura kernel is r15p0/UK 10.6 and a Linux DDK would have to match it (or the
  kernel would have to be rebuilt as r7p0), and it is 32-bit armhf/glibc while
  Nura is aarch64/musl.
* Required artifact: a commercial ARM licence plus the r15p0-compatible
  Linux/armhf DDK — not obtainable, not redistributable in pmOS.

## 5. Recommendation

1. **Keep `j7elte-gpu-soft`** as the shipped 3D stack (Mesa llvmpipe on fbdev;
   `gpu-userspace.md` §5). It is the only stack that works today.
2. **The only realistic route to hardware 3D is (a): mainline Exynos 7580 +
   panfrost.** It is the only path that produces an open, maintainable driver
   and the only one with an upstream home. Budget 6–12 person-months for
   SoC+KMS bring-up and 12–18 for a usable GLES2 stack.
3. **Path (b) is demonstrable but not a GL solution for Nura.** The prototype
   proves the blob loads, executes EGL, and speaks kbase UK 10.0. Turning that
   into acceleration requires a 32-bit Android container (`servicemanager`,
   gralloc, hwcomposer), a 32-bit libhybris client, and a 32-bit
   hwcomposer→Wayland bridge — plus building the kernel as r7p0 (or remapping
   ids 552/553). Estimated 2–4 person-months for a hardware PoC, with a
   closed, Samsung-specific HAL stack as the long-term liability. Undertake it
   only if running the stock Android GL stack matters more than maintainability.
4. **Path (c) is not viable** and should be closed.

## 6. Artifacts

| File | What it is |
|---|---|
| `parity/userspace/gpu/re-hybris/extract-rom-gles.sh` | debugfs extractor + ELF manifest printer (read-only, no root) |
| `parity/userspace/gpu/re-hybris/extract-manifest.txt` | generated manifest: class/ABI, sha256, NEEDED for 20 objects |
| `parity/userspace/gpu/re-hybris/miniloader/` | freestanding ARM32 ELF loader: APS2 (SLEB128) + REL/PLT relocations, 284 stub symbols, EGL/kbase probes (`miniloader.c`, `common.h`, `start.S`, `Makefile`) |
| `parity/userspace/gpu/re-hybris/run-prototype.sh` | builds the loader and runs it on the extracted blob under `qemu-arm` |
| `parity/userspace/gpu/re-hybris/logs/miniloader.log` | captured run: relocations, EGL export addresses, `eglGetDisplay`, kbase ioctls, blob error text |
| `parity/userspace/gpu/re-hybris/proto/` | ARM32 bionic harness linked directly against `libGLES_mali.so` (exercises the ROM's linker; currently blocked by the qemu/bionic deadlock) |
| `parity/userspace/gpu/re-hybris/logs/bionic-linker.log` | strace of the ROM linker mapping the blob and all NEEDED libs, then its startup failure (`page record … not found`, exit 1) |

Reproduce:

```
cd parity/userspace/gpu/re-hybris
./extract-rom-gles.sh ../../../../system.raw romroot   # manifest + rootfs
./run-prototype.sh                                      # needs qemu-arm + clang/lld
```

**Patches: none.** No source change is justified for this gap: the kernel half
is already complete (kbase r15p0, `gpu.md`), and the userspace half needs
either a mainline port or a proprietary/Android stack. The one kernel-level
lever for path (b) — building the ROM's r7p0 DDK — is a shared-config change
(`CONFIG_DDK_VERSION_OS="m"`, `CONFIG_MALI_T72X=y`, `CONFIG_MALI_R7P0_T72X=y`)
and is documented, not applied (`gpu.md` §7 R1).

## 7. Verification status

* Everything here is static analysis, cross-compilation and qemu-user
  execution. **No j7elte hardware, no kernel boot, no GPU workload.**
* Proven: ELF/ABI compatibility of the blob with ARM32 Linux userspace; all
  relocations and imports; EGL entry points execute; the kbase UK handshake
  and `SET_FLAGS` messages the blob emits; the blob's DDK build identity; the
  UK-id delta between r7p0 and r15p0.
* Not proven: any ioctl against the real kbase, any GPU submission, any
  presentation, and the post-`SET_FLAGS` call sequence.
* The miniloader is diagnostic only: it fakes `/dev/mali0` and answers ioctls
  itself. It must not be described as a driver or as acceleration.
* Residual risk: the 23 UK-wrapper call sites not resolved statically (ids
  computed/loaded rather than `movw` immediates) could reveal further
  vendor-id uses, including `CREATE_SURFACE` (552). A device run with a
  logging kbase (or `trace-cmd`/ftrace on `kbase_ioctl`) would close this.
