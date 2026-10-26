# GPU path 1 — 32-bit Android GL compatibility (libhybris/Halium-style)

Scope: can the ROM's 32-bit Android GL stack — `/vendor/lib/egl/libGLES_mali.so`
(ARM Mali-T720, DDK `r7p0-03rel0`) plus `gralloc.exynos5.so` /
`hwcomposer.exynos5.so` — be driven from the Nura (aarch64/musl) userspace, the
way libhybris does it on Halium devices?

**Verdict: no, and the blocker is architectural, not a missing patch.** Every
Android GL/HAL object in this ROM is **ELF32 / EM_ARM / bionic**; Nura is
**ELF64 / EM_AARCH64 / musl**. There is no 64-bit variant anywhere in the image
(`/lib64` holds exactly one file, `libSecMMCodec.so`; `/lib/egl` holds only
`libGLES_android.so` + `egl.cfg = "0 0 mali"`). libhybris does not translate
between instruction sets — it `dlopen`s the vendor `.so` *into* the calling
process — so the only shapes this can take are (a) a 32-bit helper process with
IPC that does not exist, or (b) an ARM32 libc/bionic runtime, which does not
exist in postmarketOS. Both prototypes below measure exactly how far each gets.

Delivered: `parity/userspace/gpu/libhybris/` (two probes, build/run scripts,
ROM extraction). No package, therefore no patch: there is nothing installable to
package until a 32-bit GL server exists, and inventing one is the multi-month
project this report is meant to price.

---

## 1. The ROM's stack, as extracted (`./extract-rom.sh`, straight from `system.raw`)

All objects: **ELF32, little-endian, EM_ARM, EABI5 (`e_flags = 0x5000000`), DYN**,
and — this is Samsung's build flavour — **every one of them carries
`PT_INTERP = /system/bin/linker`**, including the shared libraries, not only the
executables. `system.raw` is the contents of `/system` (the ext4 root *is*
`/system`); `vendor/` sits at the same root.

| Object | Size | SONAME | DT_NEEDED | sha256 |
|---|---|---|---|---|
| `/vendor/lib/egl/libGLES_mali.so` | 23 368 296 | libGLES_mali.so | libz, libdl, libhardware, libui, libutils, libcutils, libbinder, liblog, libc++, libion, libc, libm | `62e10725f1077ffa483224de5a83c14a0a0ccdccb51cd094177b7bd1630be317` |
| `/system/lib/hw/gralloc.exynos5.so` | 26 576 | gralloc.exynos5.so | liblog, libcutils, libion, libutils, libc++, libdl, libc, libm | `9a8223432a1e1231860ecd001df0ef9fd879d6ed8699770190bca63c03ca5635` |
| `/system/lib/hw/hwcomposer.exynos5.so` | 30 336 | hwcomposer.exynos5.so | liblog, libcutils, **libEGL, libGLESv1_CM**, libhardware, libhardware_legacy, libutils, libsync, libexynosv4l2, libexynosutils, libhwcutils, libdisplay, libhdmi, libmpp, libexynosgscaler, libExynosHWCService, libc++, libdl, libc, libm | `6d46e5d5e283802f0d90a2842f77fedf4617c1d3df701d8fee85fee0f0e2ce36` |
| `/system/bin/linker` | 190 292 | — | — (self) | `601ffb95aec1d509e755e1ee0b27dea996c77e36a78a9b7239af7027d8c37aa2` |
| `/system/lib/libc.so` | 678 392 | libc.so | libdl.so | `204785e4dafadfc0c8195e5663aa18be24b41d38aa4f7005cd94005d4d8a29be` |
| `/system/lib/libdl.so` | 9 492 | libdl.so | — | `391cae6ad2d31de26a7cb9e53471053f891c9d9899ff6106061d01277380eee7` |
| `/system/lib/libc++.so` | 575 088 | libc++.so | libdl, libc, libm | `ec564695859380abe2892826e9ac0c3abbf7666dd105833c2e43e866da8feef7` |
| `/system/lib/libbinder.so` | 181 880 | libbinder.so | liblog, libcutils, libutils, libc++, libdl, libc, libm | `40bbe649cf9d9dc9f232134243794c40e1059beb755c7fde0cca6b7e755c7784` |
| `/system/lib/libhardware.so` | 17 916 | libhardware.so | libcutils, liblog, libdl, libc++, libc, libm | `ec1843a7a416ca2b65106458922e66a9a57ed0a23be1563b3ec5f0e85f559405` |
| `/system/lib/libion.so` | 13 820 | libion.so | liblog, libc++, libdl, libc, libm | `e28b2d6e76c8acec73f085b6613c6596a7b8e49b8ccc5c97cf714b3208e389c5` |
| `/system/lib/libui.so` | 58 980 | libui.so | libcutils, libhardware, libsync, libutils, liblog, libc++, libdl, libc, libm | `c5ad8c1d1fa1b6aa845c1faa0c6caf67ba8c2c39daa3d9575f13e2082df31de5` |
| `/system/lib/libutils.so` | 104 052 | libutils.so | libbacktrace, libcutils, libdl, liblog, libc++, libc, libm | `697751aa1ea41f1b0922144350ced6b59c54eb27f281d26a36dbe4cf4af154c1` |
| `/system/lib/libEGL.so` | 461 264 | libEGL.so | libcutils, libutils, liblog, libGLES_trace, libdl, libc++, libc, libm | `288e70cface1293931113edbf5efe3c9447a4664c87f66be13506169dfb676f6` |

Facts that decide the design:

* The **only** GL driver is `libGLES_mali.so` and it is 32-bit. `/lib64` has no
  EGL/GLES/HAL object at all, so there is nothing an aarch64 process could
  `dlopen` even if the ABI matched.
* The blob's own import list is a full Android framework surface — 285 undefined
  dynamic symbols: `__android_log_print`, `hw_get_module`, `ion_open`,
  `ion_alloc_fd`, `android::Region::*`, `libc++` (`std::locale`, `_Znwj`, …),
  `libbinder`, plus POSIX. It is not a self-contained driver.
* Its EGL surfaces are Android `ANativeWindow`/gralloc buffers; there is no
  Wayland/X11 WSI code and no `wl_display`/`wl_surface` mention (see §5).

## 2. Does libhybris help? No — and pmaports has no such package

* Checked `parity/pmaports`: `grep -rl libhybris --include=APKBUILD` → **no
  match**. There is no libhybris package in this tree to build against, and none
  of the existing packages depends on one.
* libhybris' model is *in-process*: a glibc/musl application `dlopen`s the
  vendor `EGL`/`hwcomposer` `.so` through a compat shim. That requires the vendor
  object to share the caller's ELF class and ABI. Here the vendor objects are
  ELF32/ARM/bionic and the caller is ELF64/AArch64/musl. On Halium, 64-bit
  devices ship 64-bit vendor libs precisely because of this rule; this ROM does
  not (Android 6, 32-bit-only Exynos 7580 userspace).
* There is no "32-bit libhybris on aarch64" mode: the alternatives are a 32-bit
  helper process with a GL-over-IPC protocol (nothing implements this; a GLES
  command-stream forwarder + buffer sharing would be a new project) or running
  the Android stack (surfaceflinger/gralloc/hwcomposer) in a 32-bit chroot and
  compositing its output — that gives Nura applications *nothing* and is not a
  Wayland client.
* Also unverified in either direction: the blob is DDK `r7p0` / UK 10.0 while
  any Nura kernel here builds `r15p0` / UK 10.6, whose Samsung vendor ioctl
  block is renumbered by +4 (`KBASE_FUNC_HWCNT_UTIL_SETUP` +37→+41,
  `CREATE_SURFACE` +40→+44, `VSYNC_SKIP` +39→+43, `EXT_BUFFER_LOCK` removed) —
  see `gpu.md` §7 R1. So even a working 32-bit bridge would carry an unverified
  break risk on `/dev/mali0`.

## 3. Prototype A — the honest Android path: an ELF32 bionic executable (`eglprobe`)

`eglprobe.c` is a 32-bit ARM PIE built with clang + ld.lld, with
`PT_INTERP = /system/bin/linker` and `DT_NEEDED {libdl.so, libc.so}` pointing at
the ROM's own bionic. It has no NDK sysroot, so `_start` is hand-written and
calls `__libc_init(raw_args, 0, slingshot, 0)` the way bionic's
`crtbegin_dynamic.S` does; output uses `write(2)`. It then does
`dlopen("/vendor/lib/egl/libGLES_mali.so", RTLD_LAZY)`, `dlsym` of the six EGL
entry points, and calls them.

Result under `qemu-arm` (which is the only ARM32 execution available; no
hardware):

```
linker: ./eglprobe: unsupported flags DT_FLAGS_1=0x8000000
WARNING: linker: ./eglprobe: unsupported flags DT_FLAGS_1=0x8000000
<hang>
```

The ROM's linker loads and maps the binary, maps `libc.so`, then **blocks in
`futex(FUTEX_WAIT)` before reaching any entry point**. This is not a bug in the
probe: the ROM's own `/system/bin/toybox`, extracted and run the same way,
reaches exactly the same place (`CANNOT LINK EXECUTABLE` until all of its
`DT_NEEDED` are extracted, then the identical hang). `-strace` shows the same
tail for both:

```
openat("/dev/__properties__", O_RDONLY|O_NOFOLLOW) = -1 ENOENT
openat("/system/lib/libnetd_client.so", ...) = -1 ENOENT   (x3)
brk(NULL); mmap(262144); madvise(MADV_MERGEABLE) = -1 EINVAL
openat("/proc/stat"); read(1024)
socket(PF_UNIX); connect("/dev/socket/logdw") = -1 ENOENT; close
futex(0x408b4bf8, FUTEX_PRIVATE_FLAG|FUTEX_WAIT, ...)  <-- never returns
```

`prctl(PR_SET_VMA)` and `madvise(MADV_MERGEABLE)` both fail with `EINVAL` on
this host kernel, and the sequence ends in a wait that nothing can satisfy. So
the bionic half of this experiment is **inconclusive on this host** — it is a
qemu-user/bionic-6 interaction, not a property of the blob or of j7elte. On the
device (real kernel, `/dev/__properties__`, `/dev/log*`, `/dev/socket`) the same
binary would start; what it would do next is what prototype B measures without
bionic in the way.

## 4. Prototype B — a hand-rolled loader (`mloader`), which does run

`mloader.c` is a freestanding ELF32/ARM program (raw syscalls, static, ~27 KiB)
that performs the loader's job itself:

1. maps the blob at a fixed base honouring `PT_LOAD`;
2. applies its relocations — including Android's **packed APS2** format
   (`DT_ANDROID_REL = 0x6000000f`, magic `"APS2"`, SLEB128 group encoding); the
   signed decode is validated two independent ways (every `r_offset` lands
   inside the image, and the number of type-23 entries equals `DT_RELCOUNT`);
3. resolves the 285 undefined imports against a shim layer (libc string/memory/
   printf/time, `pthread_*`, C++ `new`/`delete`/`atexit`, `dlopen`/`dlsym`/
   **`dladdr`**, `hw_get_module`, `ion_*`), with a per-symbol trap stub for
   anything else that *names the symbol at first call* instead of failing
   silently;
4. runs `DT_INIT_ARRAY`, then calls the EGL entry points.

Observed (reproducible, `./run-mloader.sh`):

```
[mloader] mapped 0x020000000 + 24117248 bytes
[mloader] APS2 packed relocations: 122176 bytes, expecting 88977 entries
[mloader] packed relocs: 96168 decoded (584 groups) = 88977 RELATIVE + 754 GLOB_DAT + 6437 ABS32
[mloader] relocations applied: 106993 (DT_RELCOUNT said 88977 for .rel.dyn)
[mloader] distinct trapped imports: 122
[mloader] running 62 constructor(s)
[mloader]  ctor 0 @ 0x02023bf04 ... ctor 61 @ 0x02025e6c0     (all 62 ran)
[mloader] eglGetDisplay     = 0x020469910
[mloader] eglQueryString    = 0x02046ae14
[mloader] eglInitialize     = 0x0204699a8
[mloader] eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS) =
          EGL_EXT_client_extensions EGL_EXT_platform_base
          EGL_KHR_client_get_all_proc_addresses EGL_KHR_platform_android
[mloader] eglGetDisplay(EGL_DEFAULT_DISPLAY) = 0x041e56ce8
[mloader] eglGetError() = 0x000003000 (EGL_SUCCESS)
[mloader] EGL_VENDOR  = (null)
[mloader] EGL_VERSION = (null)
```

with 106 993 = 96 168 (packed) + 10 825 (`JMPREL`, `PLTRELSZ/8`) relocations,
zero unhandled relocation types.

What this proves, and what it does not:

* **Proved:** the blob is a well-formed ELF32 ARM EGL implementation that
  *executes* on this kernel's 32-bit compat path (under emulation); its 62
  constructors complete; `eglGetDisplay(EGL_DEFAULT_DISPLAY)` constructs a
  display object and returns it with `EGL_SUCCESS`; and its **client extension
  string is real data out of the blob**. Relocation/APS2 handling is exact.
* **Not proved:** anything that needs the Android environment. `EGL_VENDOR` and
  `EGL_VERSION` on that display return NULL, and the next call faults on a
  pointer that only real initialisation would fill. The blob needs
  `hw_get_module("gralloc")` → `gralloc.exynos5.so` → `/dev/ion` +
  `/dev/mali0`, and `eglInitialize` would need the r7p0 kernel ABI. Our shims
  answer "no HAL here", which is the truthful answer on this host.
* **Not a success claim:** no GPU, no surface, no frame. The shim layer means
  the EGL objects above are built by the blob's own code with fake HAL returns;
  that is enough to prove the ABI/loader half works and is not evidence that a
  real EGL config/surface/context would succeed.

Loader bugs found and fixed along the way (worth recording, they are the traps
in this format): APS2 is SLEB128 with *offsets accumulating across groups and
going backwards at each type boundary*; `DT_ANDROID_REL` is a **vaddr**, and the
blob's `PT_LOAD` vaddr is `+0x9e000` from its file offset; `.init_array` also
needs the `+0x9f000` second-segment bias; the blob calls `dladdr` to find its
own base; `realloc` must preserve contents (a bump allocator silently corrupts
the blob's tables); and a heap mapped before the `MAP_FIXED` blob image can be
clobbered by it.

## 5. The Wayland/WSI gap

Nothing in this stack produces a Wayland surface. The blob's EGL is the Android
platform EGL: `eglCreateWindowSurface` expects an `ANativeWindow` (a
`Surface`/`FramebufferNativeWindow` backed by a gralloc buffer) and present is
`eglSwapBuffers` → `gralloc`/`hwcomposer`/`libui` fences. To put that on screen
in Nura one would need the missing middle piece that libhybris provides on
Halium — `libhybris`'s `ws_*`/`hwcomposer` glue creating `wl_buffer`s from
gralloc handles — and that glue too would have to run in a 32-bit ARM process
to touch these libraries. Today Nura has only llvmpipe/`swrast` over fbdev
(`gpu-userspace.md` §5); this path adds no pixels.

## 6. What is delivered, and what would have to exist first

```
parity/userspace/gpu/libhybris/
├── extract-rom.sh    # dump the minimum bionic+GL set from system.raw → ./romroot
├── eglprobe.c        # ELF32 bionic PIE: dlopen(blob) + eglGetDisplay/eglQueryString
├── mloader.c         # freestanding ELF32 loader: PT_LOAD + APS2 relocs + shims + EGL calls
├── Makefile          # clang/ld.lld, armv7a-linux-gnueabi, `make check`
├── run-qemu.sh       # run eglprobe under qemu-arm (hangs: see §3)
└── run-mloader.sh    # run mloader under qemu-arm
```

For this path to produce frames, all of the following would have to exist, in
this order: (1) a 32-bit ARM userspace (bionic or a musl-based Android-ABI
runtime) that runs on the Nura kernel; (2) inside it, a GL server exporting
GLES over IPC to aarch64 clients; (3) that server's EGL surfaces re-plumbed from
gralloc to dma-buf + Wayland (`wl_drm`/`linux-dmabuf`); (4) the r7p0↔r15p0
vendor-ioctl renumbering resolved (§2). Step (2) alone is a subsystem no
distribution ships. That is why the recommended route for 3D is Mesa/panfrost on
a kbase or DRM kernel backend, not this bridge.
