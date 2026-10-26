# libGLES_mali.so (r7p0) — reverse-engineering inventory

Scope: static inventory of the Android Mali userspace driver shipped in the Nura/j7elte
ROM (`system.raw`, `SM-J700M J700MUBU2BRC2`, Android 6.0.1 / SDK 23). This is a reference
map, not a driver: **nothing here was executed on a device**, and none of the blob is
reusable as a Linux GL stack (reasons in §10).

Method: ARM `objdump` (binutils 2.36.1, `elf32-littlearm`) over the full `.text`,
`readelf` dynsym/dynamic/relocations, a decoder for the file's `SHT_ANDROID_REL` "APS2"
packed relocation format, and Ghidra headless (`ARM:LE:32:v7`). The Ghidra import
(`ghidra-project-quick`, `-noanalysis`, driven by `tools/QuickBlob.java`) recovered 3,811
function bodies below `0x520000` and resolved string references; its internal boundaries
are quoted where they matter (`FUN_00265480` ioctl wrapper = 64 B, `FUN_002654c0`
open/handshake = 548 B, `FUN_00265c94` submit = 88 B, `FUN_00266604` alloc = 444 B,
`FUN_0028ea94` job-chain flush = 248 B, `FUN_0030f50c` compiler front-end = 6,688 B).
Every address below is a virtual address in the blob's own address space (first
`PT_LOAD` maps file offset 0 to vaddr `0x9e000`).

---

## 1. Provenance and hashes

Extracted from `system.raw` with `debugfs` (the image is the ext4 system partition, so the
in-image paths are `/vendor/...`, `/lib/...`, not `/system/vendor/...`).

| file | in-image path | size | sha256 |
|---|---|---|---|
| `libGLES_mali.so` | `/vendor/lib/egl/libGLES_mali.so` | 23,368,296 | `62e10725f1077ffa483224de5a83c14a0a0ccdccb51cd094177b7bd1630be317` |
| `libEGL.so` (framework wrapper) | `/lib/libEGL.so` | 461,264 | `288e70cface1293931113edbf5efe3c9447a4664c87f66be13506169dfb676f6` |
| `libbinder.so` | `/lib/libbinder.so` | 181,880 | `40bbe649cf9d9dc9f232134243794c40e1059beb755c7fde0cca6b7e755c7784` |
| `libc++.so` | `/lib/libc++.so` | 575,088 | `ec564695859380abe2892826e9ac0c3abbf7666dd105833c2e43e866da8feef7` |
| `libc.so` | `/lib/libc.so` | 678,392 | `204785e4dafadfc0c8195e5663aa18be24b41d38aa4f7005cd94005d4d8a29be` |
| `libcutils.so` | `/lib/libcutils.so` | 63,204 | `d0dadf19a45cc53dff4b535e0b68522e5a6dc80d0daf18547ea73e46291a3750` |
| `libdl.so` | `/lib/libdl.so` | 9,492 | `391cae6ad2d31de26a7cb9e53471053f891c9d9899ff6106061d01277380eee7` |
| `libhardware.so` | `/lib/libhardware.so` | 17,916 | `ec1843a7a416ca2b65106458922e66a9a57ed0a23be1563b3ec5f0e85f559405` |
| `libion.so` | `/lib/libion.so` | 13,820 | `e28b2d6e76c8acec73f085b6613c6596a7b8e49b8ccc5c97cf714b3208e389c5` |
| `liblog.so` | `/lib/liblog.so` | 30,324 | `2d2e75c95f584621b004444d58033ec4cbd4ab3b251df0fa88fb7e9e5e92a048` |
| `libm.so` | `/lib/libm.so` | 132,824 | `b2a8260e9a66687c4923c6d1a75c08a5707548561ec583b39b78766df61b526f` |
| `libui.so` | `/lib/libui.so` | 58,980 | `c5ad8c1d1fa1b6aa845c1faa0c6caf67ba8c2c39daa3d9575f13e2082df31de5` |
| `libutils.so` | `/lib/libutils.so` | 104,052 | `697751aa1ea41f1b0922144350ced6b59c54eb27f281d26a36dbe4cf4af154c1` |
| `libz.so` | `/lib/libz.so` | 112,224 | `7aab6f7e01a375bc38e1464a5e9d35681463c3c8ef6871dc13cde95ea750c59e` |
| `gralloc.exynos5.so` | `/lib/hw/gralloc.exynos5.so` | 26,576 | `9a8223432a1e1231860ecd001df0ef9fd879d6ed8699770190bca63c03ca5635` |
| `hwcomposer.exynos5.so` | `/lib/hw/hwcomposer.exynos5.so` | 30,336 | `6d46e5d5e283802f0d90a2842f77fedf4617c1d3df701d8fee85fee0f0e2ce36` |

All are `ELF32`, little-endian, `EM_ARM`, `EABI5`, `DYN`, `for Android 23`, interpreter
`/system/bin/linker` (bionic). The blob's own dynamic tag `SONAME = libGLES_mali.so`,
`DT_NEEDED` = `libz.so libdl.so libhardware.so libui.so libutils.so libcutils.so
libbinder.so liblog.so libc++.so libion.so libc.so libm.so`.

Build facts (`.comment`, `.note.gnu.gold-version`, `.note.android.ident`,
`Tag_CPU_arch`):

* compiler: `GCC: (GNU) 4.9.x-google 20140827 (prerelease)` + `clang version 3.6`;
  linked with `gold 1.11`; Android NDK API 23 (`NT_VERSION = 0x17`).
* ARM v7-A, Thumb-2, VFPv4, NEONv1+FMA, `Tag_ABI_optimization_goals: Aggressive Debug`.
* version string: `1.4 Midgard-"r7p0-03rel0"` (file offset `0x123aad8` → vaddr
  `0x12d8ad8`), build root `hardware/samsung_slsi/MaliT720_r7p0_Insel/`.

ELF layout: `.text` `0x235010..0x12ab0e4` (17,260,756 B), `.rodata` `0x12d8580`,
`.data.rel.ro.local` `0x163be70`, `.data.rel.ro` `0x16c36a0`, `.got` `0x16d784c`,
`.data` `0x16e3000`, `.bss` 0x8bf0 B. Dynamic flags `BIND_NOW`, so every PLT slot is
resolved at `dlopen` time.

Relocations: `.rel.dyn` is `SHT_ANDROID_REL` (`0x60000001`), packed "APS2" stream
96,168 entries — 88,977 `R_ARM_RELATIVE`, 6,437 `R_ARM_ABS32`, 754 `R_ARM_GLOB_DAT`.
`.rel.plt` has 10,825 `R_ARM_JUMP_SLOT`. The packer stores no addends (`REL` semantics):
the addend is the in-place word, which is why string addresses never appear as literal
constants in the file and every string xref in this map was recovered from PC-relative
`ldr`+`add` pairs rather than relocation records.

---

## 2. Exported symbols: what is in the 23 MB

`.dynsym` has 15,011 entries; 13,917 are defined `FUNC`, 271 unique undefined imports.
The blob is a statically linked amalgam of the Mali DDK **and** a full Clang 3.6 /
LLVM 3.6 toolchain (its shader compiler front/middle-end):

| group | count | notes |
|---|---|---|
| `clang` C++ (`_ZN5clang…`) | 6,722 | front-end (Sema, AST, CodeGen) |
| `llvm` C++ (`_ZN4llvm…`) | 5,087 | IR, MC, codegen, bitcode |
| GL/GLES entry points (`gl*`) | 598 | GLES 1.1/2.0/3.x |
| LLVM C API (`LLVM*`) | 587 | classic C bindings |
| libc++ (`_ZNSt…`) | 553 | STL used by compiler |
| EGL entry points (`egl*`) | 128 | incl. platform extensions |
| OpenCL (`cl*`) | 90 | OpenCL 1.x API surface |
| compiler-rt / ARM runtime | 87 | `__aeabi_*`, `__addsf3`, … |
| `osup_*` | 32 | OS abstraction (threads, mutex, dlopen) |
| `mali_egl_*` helpers | 4 | image/buffer accessors |
| other | 28 | `stdlibp_neon_*`, UTF helpers |

Two symbols matter for the WSI boundary:
`egl_winsys_get_implementation` (`0x264cb4`) is exported and called through the PLT from
the EGL front-end; the blob also exports the internal module helpers
`egl_connect_native_window_to_egl` (`0x471964`), `egl_get_winsys_display` (`0x4749f0`),
`egl_get_winsys_surface` (`0x474a0c`), `egl_magic_surface_set_winsys_surface`
(`0x4749f8`), and `eglp_swap_buffers` (`0x473ac8`).

Undefined imports (271) fall into: bionic libc/pthread/sem/stdio/math; `__android_log_print`
(27 call sites) and `__android_log_write`; `hw_get_module` (1); `ion_open`,
`ion_alloc_fd`, `ion_close` (1 each); `atrace_begin_body`/`atrace_setup`;
`android::Region` methods (via `libui`); and `dlopen`/`dlsym` (1 each, in `osup`).
There is **no** direct libbinder symbol import: binder is a `DT_NEEDED` dependency only
(the surface plumbing is `ANativeWindow` + gralloc, not raw binder transactions).

---

## 3. EGL / GLES entry points (addresses)

Exported `FUNC` addresses and sizes from `.dynsym`; bodies from the ARM disassembly:

| entry | addr | size | observed behaviour |
|---|---|---|---|
| `eglGetDisplay` | `0x469910` | 44 | calls `egl_winsys_get_implementation@plt` (`0x215b54` → `0x264cb4`), tail-branches `0x469654` |
| `eglInitialize` | `0x4699a8` | 2176 | per-display init; calls GPU init `0x26a800`; platform init via a winsys function pointer loaded at `0x469b1c` and called at `0x469b20` (`[[display+24]+4]`) |
| `eglCreateContext` | `0x468814` | 1432 | — |
| `eglCreateWindowSurface` | `0x4731d4` | 112 | **direct kbase vendor ioctl** `CREATE_SURFACE` (id 552, 24 B) at `0x473210`, then `0x46db64` |
| `eglDestroySurface` | `0x46e2bc` | 316 | vendor `DESTROY_SURFACE` id 553, 24 B at `0x46e3c8` |
| `eglMakeCurrent` | `0x471ff4` | 2576 | — |
| `eglSwapBuffers` | `0x47452c` | 12 | `b eglp_swap_buffers@plt` (`0x474534`) |
| `eglp_swap_buffers` | `0x473ac8` | 2660 | WSI swap (§8) |
| `eglGetProcAddress` | `0x46af1c` | 192 | iterates a runtime name→fn table via `0x462578`, compares with `0x45eb94` |
| `eglGetCurrentContext` | `0x472a04` | 44 | — |
| `glCompileShader` | `0x484498` | 92 | thunk: `0x471d8c` (current ctx), sets dispatch word `75`, tail `0x4bac2c` |
| `glLinkProgram` | `0x488e70` | 92 | dispatch word `346`, tail `0x4a95cc` |
| `glUseProgram` | `0x48cf64` | 92 | dispatch word `569`, tail `0x4a9bfc` |
| `glDrawArrays` | `0x48e6fc` | 92 | dispatch word `131`, tail `0x497f00` |
| `glDrawElements` | `0x48e758` | 92 | same dispatcher |
| `glFinish` / `glFlush` | `0x48e8a4` / `0x48e8dc` | 56 | — |
| `glGetStringi` | `0x487d54` | 104 | returns `OpenGL ES GLSL ES 3.10` from `0x4d6318` |
| `clGetPlatformIDs` | `0x515768` | 72 | OpenCL surface is present but unused by a GL path |
| `clEnqueueNDRangeKernel` | `0x519918` | 556 | — |

The GL entry points are 92-byte front-end stubs: they fetch the current context through
`0x471d8c`, store a DDK-internal dispatch index at `[ctx+16]`, and tail-call the real
implementation. Those indices (75, 131, 346, 569) are **not** kbase UAPI ids; they are an
internal GLES dispatch table.

---

## 4. The kbase ioctl client (`/dev/mali0`) — the core result

The blob opens and drives the ARM UK interface directly. There are three `ioctl` call
sites in the whole DSO; two are LLVM's terminal-width probe (`0x1276310`,
`0x1276388`). Exactly **one** is the Mali driver:

* **uk ioctl wrapper `0x265480`** (Ghidra `FUN_00265480`, 64 B)
  ```
  265480  ldr  r3,[r1]            ; uk_header.id (first u32 of the arg buffer)
  265490  ldr  r0,[r0]            ; fd = *(kbase_context)
  265494  orr  r1,r3,#0xc0000000
  265498  orr  r4,r1,#0x8000      ; type 0x80 << 8
  26549c  orr  r1,r4,r2,lsl #16   ; _IOWR(0x80, id & 0xff, size)
  2654a4  bl   ioctl@plt
  2654b4  movne r0,#3             ; DDK error code 3 on failure
  2654b8  moveq r0,#0
  ```
  The command word is `_IOWR(0x80, id & 0xff, size)`; the kernel `kbase_ioctl()` reads only
  `_IOC_SIZE(cmd)`, so the size is the real selector. Signature:
  `kbase_call(kbase_context *ctx, uk_args *args, u32 size)`.
* **open + handshake `0x2654c0`** (Ghidra `FUN_002654c0`, 548 B; the `"/dev/mali"`
  load is at `0x26553c` and the `"%s%d"` builder call in the preceding
  `FUN_002652e4`) — formats `"/dev/mali" + devno` with `"%s%d"`
  (`0x265538`/`0x26553c`, strings at `0x12d8900`/`0x12d88f8`), opens with
  `O_RDWR|O_CLOEXEC` (`0x26554c`: `mov r1,#2; movt r1,#8`), `fstat` and requires a char
  device (`S_IFCHR`, `0x265588`), then sends `UKP_FUNC_ID_CHECK_VERSION` (id 0, 16 B) with
  the version built at **`0x265db4`**: major 10, minor **0**. The reply is compared
  against the blob's 10.0 and a mismatch takes the
  `"file %s is not of a compatible version"` path (`0x2655e4`, string `0x12d8934`).
  A 24-byte `getpwnam`-style `pthread_mutex`-guarded context table tracks open contexts
  (`0x2653d4` close path).

Per-function kbase clients (address = call site to `0x265480`; `size` = value in `r2`):

| blob function | call site | UAPI id (r7p0) | name | args size |
|---|---|---|---|---|
| `0x2655b4` | handshake in open | 0 | `UKP_FUNC_ID_CHECK_VERSION` | 16 |
| `0x26585c` | init flags | 530 | `KBASE_FUNC_SET_FLAGS` | 16 |
| `0x265a20`, `0x265a84` | GPU props | 526 | `KBASE_FUNC_GPU_PROPS_REG_DUMP` | 536 |
| `0x265c10` | sync | 520 | `KBASE_FUNC_SYNC` | 40 |
| `0x265c94` | job submit | **540** | `KBASE_FUNC_JOB_SUBMIT` | **32** |
| `0x265d1c` | post term | 521 | `KBASE_FUNC_POST_TERM` | 8 |
| `0x265e4c` | hwcnt setup | 522 | `KBASE_FUNC_HWCNT_SETUP` | 40 |
| `0x265e94` | mem free | 516 | `KBASE_FUNC_MEM_FREE` | 16 |
| `0x265ebc` | disjoint query | 541 | `KBASE_FUNC_DISJOINT_QUERY` | 16 |
| `0x2660e8` | mem query | 515 | `KBASE_FUNC_MEM_QUERY` | 32 |
| `0x266250` | mem commit | 514 | `KBASE_FUNC_MEM_COMMIT` | 32 |
| `0x2663ac` | mem alias | 518 | `KBASE_FUNC_MEM_ALIAS` | 56 |
| `0x2664bc` | mem import | 513 | `KBASE_FUNC_MEM_IMPORT` | 48 |
| `0x266604` | mem alloc | 512 | `KBASE_FUNC_MEM_ALLOC` | 56 |
| `0x2667c0` | mem flags change | 517 | `KBASE_FUNC_MEM_FLAGS_CHANGE` | 32 |
| `0x2669b4` | stream create | 536 | `KBASE_FUNC_STREAM_CREATE` | 48 |
| `0x266ad4` | fence validate | 535 | `KBASE_FUNC_FENCE_VALIDATE` | 16 |
| `0x266b10` | debugfs profile | 539 | `KBASE_FUNC_DEBUGFS_MEM_PROFILE_ADD` | 24 |
| `0x473210` | `eglCreateWindowSurface` | **552** | `KBASE_FUNC_CREATE_SURFACE` (Samsung) | 24 |
| `0x46e3c8` | `eglDestroySurface` | **553** | `KBASE_FUNC_DESTROY_SURFACE` (Samsung) | 24 |
| `0x466cb4`, `0x466ce8` | secure color buffer | **567/568** | `SECURE_WORLD_RENDERING` / `NON_SECURE_WORLD_RENDERING` | 24 |

The blob does not call `GET_VERSION` (528) or `GET_CONTEXT_ID` (543): it derives the
DDK version from the handshake reply and the GPU identity from `GPU_PROPS_REG_DUMP`.
Ids confirmed against the in-tree r7p0 header
`drivers/gpu/arm/t72x/r7p0/mali_kbase_uku.h:408-480` (`UK_FUNC_ID = 512`; vendor block
`HWCNT_UTIL_SETUP=549 … TMU_SKIP=556`; secure 567/568). No calls to
`SET_MIN_LOCK`/`UNSET_MIN_LOCK`/`TMU_SKIP`/`VSYNC_SKIP` were found.

### 4.1 Memory model observed in the client

* **Tracking page**: `0x266168` does
  `mmap64(NULL, 4096, PROT_NONE, MAP_SHARED, fd, 0x3000)` and stores the result at
  `[ctx+0x264]`; `0x2661d0` unmaps it. Offset `0x3000` is exactly
  `PFN_DOWN(BASE_MEM_MAP_TRACKING_HANDLE)`; the kernel requires it before any other
  mapping.
* **BO mapping**: `0x265fb8` computes the VA size from the page count
  (`pages << 12`, with optional alignment) and calls
  `mmap64(NULL, len, prot, MAP_SHARED, fd, cookie)`, where `cookie` comes from the
  `MEM_ALLOC` output at `[ctx+…]`. This is the same SAME_VA cookie model the kernel
  implements: r7p0 defines `BASE_MEM_MAP_TRACKING_HANDLE (3ull<<12)` and
  `BASE_MEM_COOKIE_BASE (64ul<<12)` (`t72x/r7p0/mali_base_kernel.h:212-215`), matching
  the `0x3000` tracking mmap and the returned cookie; `MEM_ALLOC` returns the cookie,
  userspace mmaps it, and **CPU VA == GPU VA**.
* **ION path**: `0x266604` checks a flag `0x10000` in the alloc request; if set it calls
  `0x50e7d4`, which is the ION helper (`0x50e7e4`, `0x50e810`, `0x50e824`):
  `ion_open` → `ion_alloc_fd(fd, pages<<12, 4096, flags=16, heap_mask=0x20000010,
  &out_fd)` → `ion_close`; the dma-buf fd is the sixth-argument out value, not the return
  value. That fd is then wrapped by `MEM_IMPORT` (id 513).
* **Events**: `0x266870` reads from the same fd with
  `read(fd, buf, nr_atoms * 24)` (24 = `sizeof(base_jd_event_v2)`; division by 24 via the
  `0xaaaaaaab >> 4` magic at `0x2668b4`). `EAGAIN`/`EINTR` are tolerated
  (`0x2668e4`).
* **Sync/fence**: `0x266ad4` wraps an fd with `fcntl(fd, 0x406, 0)` then issues
  `FENCE_VALIDATE` (535, 16 B); `0x265f04`/`0x265f50` issue `SYNC` (520) with mode 1/2
  for dependency chains.

### 4.2 Job submission and the r7p0 wire format

`0x265c94` (Ghidra `FUN_00265c94`, 88 B) is the submit wrapper. It builds a 32-byte
`kbase_uk_job_submit` on the stack:

```
[sp+0]  id = 540
[sp+8]  atom array GPU address
[sp+16] nr_atoms
[sp+20] stride = 56
[sp+24] gles_ctx_handle = [atom + 48]
```

The `56` is decisive: the blob was built with `CONFIG_MALI_SYSTRACE_SUPPORT`, so the
r7p0 `base_jd_atom_v2` is the 56-byte variant (`mali_base_kernel.h:625-642`), not the
48-byte one. The r7p0 `kbase_uk_job_submit` is 32 bytes
(`mali_kbase_uku.h:105-120`), and `gles_ctx_handle` is at atom offset 48.

The Nura kernel is **r15p0 / UK 10.6**, where the submit struct is 40 bytes and the atom
is 64 bytes (`core_req` moved from u16 @34 to u32 @44). That is the hard ABI break
already established in `parity/userspace/gpu/kbase/analysis.md` §2; this blob map
independently confirms the blob side (32/56, id 540).

Only one call chain reaches the submit wrapper:
`0x28ea94` (job-chain flush) → `0x265cec` → `0x265c94`. `0x28ea94` walks a chain
descriptor (`[desc+4]` = array of atom pointers, `[desc+9]` = atom count,
`[desc+11]` = group count), submits the atoms, then issues the SYNC calls for the same
groups with mode 1/2 (`0x28fc04`). This is the boundary between the DDK command-stream
layer and kbase.

---

## 5. Command-stream / batch builder (`cmar`)

The DDK module is ARM's "cmar" (command marshalling / GL executor). Evidence:

* assert string `In file: …/product/cmar/src/mali_cmar_glexecutor_gpu.c line: 1509`
  (file offset `0x123c47c` → vaddr `0x12da47c`), referenced at `0x28ec98` inside the
  submission function described above;
* the same region (`0x28e9ec..0x28f0xx`) contains the job-chain flush `0x28ea94`, the
  per-group submit/sync loops, `clock_gettime`/`sem_timedwait` waits (`0x28e8e4`), and
  the GPU-executor descriptors.

The GLES side that feeds it:

* `glDrawArrays` `0x48e6fc` → context `0x471d8c` → dispatcher `0x497f00`
  (`glDrawElements` `0x48e758` takes the same path). Draw paths build Midgard job
  descriptors and place them in mapped memory; the atom's `jc` is a GPU VA, so the
  command stream lives in the same SAME_VA allocation as the CPU mapping.
* Midgard-specific descriptor work is visible in the framebuffer/tiler modules:
  `product/cframe/src/mali_cframe_mipmap.c`, `product/gles/src/fb/*`, and the vendor
  `CREATE_SURFACE`/`DESTROY_SURFACE` calls from EGL.
* Secondary buffer/fence handling: `egl_color_buffer_*` exports (`0x464560`…
  `0x466cc8`) wrap gralloc buffers, secure-world transitions and fences.

What is *not* recoverable statically with confidence is the exact byte layout of each
Midgard job descriptor emitted by `0x497f00`; that would require decompiling the draw
dispatcher, which is outside an inventory. The map only establishes where the boundary
is: GLES → cmar job chain → `JOB_SUBMIT`.

---

## 6. Midgard shader compiler

The blob embeds a complete Clang 3.6.0 / LLVM 3.6 toolchain (`.comment`, and the export
table in §2). This is the DDK shader compiler, not a generic tool: the strings include
Mali-specific diagnostics (all in `.rodata`, file offset → vaddr = +0x9e000):

| string | vaddr |
|---|---|
| `Please contact support-mali@arm.com with the shader causing` | `0x12dda38` |
| `the data pointed to by <binary> is not GL_MALI_SHADER_BINARY_ARM` | `0x12f4704` |
| `Compiler frontend failed (error code ` | `0x142fdc0` |
| `Compiler middle failed (error code ` | `0x142fe08` |
| `OpenGL ES GLSL ES 3.10` (reported by `glGetStringi`) | `0x12f2e00` |

Entry/identifier evidence:

* GLES shader API region `0x4ba900..0x4bb500`: uses the module identifiers
  `gles2_programp_shader_source_internal` (`0x4baa34`), `gles2_program_compile_shader`
  (`0x4bad2c`), `gles2_program_shader_binary` (`0x4bb180`…`0x4bb354`), all via
  PC-relative literals.
* `glCompileShader` `0x484498` → `0x4bac2c` → shader-object lock + cache lookup
  `0x4b1830`; `glLinkProgram` `0x488e70` → `0x4a95cc`.
* **GLSL front-end location:** Ghidra resolves the
  `Language version '%s' unknown, this compiler only supports up to version…` string
  (`0x12e43fc`) reference at `0x310750`, inside `FUN_0030f50c` (`0x30f50c`, 6,688 B).
  That function is the compiler's version/command-line front-end; the support-mali
  diagnostic is in `FUN_002a7f88` (string xref at `0x2a7fa8`).
* Module registration/name table for the shader module: `gles_shaders_get`
  (`0x1420328`) referenced at `0x4d18f4`, `0x4d1960`, `0x4d198c`.
* The compiler diagnostic formatter at `0x2a7f88` uses the support-mali string
  (`0x2a7fa0`) to append to the shader log.
* The compiler is compiled into the same DSO and its 11,809 C++ exports are interleaved
  with DDK code across the whole `.text`; there is no separable "compiler .so".

For Nura this whole component is **not needed**: Mesa's Panfrost already contains an
open Midgard compiler (`src/panfrost/compiler/midgard_*`, NIR → Midgard ISA). The blob's
compiler is only relevant as evidence of what the closed path did; it cannot be reused
(closed, AArch32/bionic, and its output feeds the DDK's own cmar descriptors).

---

## 7. Internal module layout (from embedded source paths)

The assert strings retain 106 full source paths, which expose the DDK module split:

| module | role | example file |
|---|---|---|
| `product/base` | kbase client (`/dev/mali`, UK calls, memory, atoms) | `mali_base_kbase.c` |
| `product/egl` | EGL front-end + native window surface | `mali_egl_window_surface.c` |
| `product/gles` | GLES 1/2/3 state, draw, program, texture, fb | `src/program/mali_gles2_program_shader_api.c` |
| `product/cmar` | command marshalling / GPU executor / job chains | `mali_cmar_glexecutor_gpu.c` |
| `product/cframe` | framebuffer/mipmap helpers | `mali_cframe_mipmap.c` |
| `product/cctx` | context bookkeeping / histogram | `mali_cctx_histogram.c` |
| `product/cdbg` | debug channels, env, failure dumps | `mali_cdbg_control.c` |
| `product/cutils` | container/string utilities | `mali_cutils_cstr.c` |
| `product/osu` | OS abstraction (threads, mutex, dlopen) | `platform_dummy/mali_osu.c` |

This is the same layering the blob's symbols show: EGL/GLES public API → cmar → base →
`/dev/mali0`.

---

## 8. Android WSI / gralloc / binder dependencies

The display path is Android-only and is the part that cannot be carried to a DRM-less
Linux system.

* **EGL winsys indirection.** `egl_winsys_get_implementation` (`0x264cb4`) returns a
  static descriptor table in the `0x16e30a8` region (PC-relative `ldr` at `0x264cb4` +
  `add` at `0x264cb8`); the records are 12 bytes and carry EGL entry-point name
  pointers/lengths (e.g. `eglGetConfigAttrib` at string `0x12ec730`,
  `eglGetConfigs` at `0x12ec744`, `eglGetCurrentDisplay` at `0x12ec754`, …).
  `eglGetDisplay` passes this pointer to `0x469654`, which dispatches through its first
  word; `eglGetProcAddress` (`0x46af1c`) walks a separate name→function table obtained
  by `0x462578` (base/count at `[ip+24]`/`[ip+28]`) and returns the third word of a
  12-byte entry. The exact record field order is not material to this inventory; the
  point is that the blob resolves EGL entry points through its own static Android
  winsys table, not through a Linux EGL platform.
* **Native window surface.** `eglCreateWindowSurface` `0x4731d4` first creates the
  kernel-side Samsung surface (id 552), then enters `0x46db64` with the `ANativeWindow*`
  in `r2`; the window's function-pointer table (`query`, `perform`, `dequeueBuffer`,
  `queueBuffer`, `cancelBuffer`) is driven indirectly from `eglp_swap_buffers`
  (`0x473ac8`). No symbol import exists for these — they are vtable calls on the
  `ANativeWindow` passed by the Android framework.
* **gralloc.** `0x263b64` allocates a 12-byte handle and calls
  `hw_get_module("gralloc", …)` (`0x263bc4`/`0x263bd0`); the module is
  `gralloc.exynos5.so` (hash in §1). Error/log strings near it:
  `invalid gralloc handle (at %p)` at `0x12d85ac`, and the gralloc-private-structure
  version check at file offset `0x123a5cc`. The blob also knows the Exynos private
  handle layout (it imports nothing from gralloc; it opens the HAL by name and uses the
  `private_handle_t` fields directly).
* **ION.** `ion_open`/`ion_alloc_fd`/`ion_close` at `0x50e7e4`/`0x50e810`/`0x50e824`
  (heap mask `0x20000010`, alignment 4096, flags `0x10`), used for allocations that must
  be shared with other Android processes/surfaces.
* **binder / libui.** Only `android::Region` methods are imported
  (`_ZN7android6RegionC1Ev`, `…D1Ev`, `…5clearEv`, `…3setEii`, `…6orSelfE…`,
  `…12subtractSelfE…`, `_ZNK…8getArrayEPj`) plus `libbinder.so` as a `DT_NEEDED`.
  That is the SurfaceFlinger/`libui` region bookkeeping used to compute swap damage —
  irrelevant to a Linux winsys.
* **hwcomposer.** Not imported by the blob; it is the other half of the Android display
  stack (`hwcomposer.exynos5.so`, hash in §1), reached by SurfaceFlinger, not by the GL
  driver. It is included in the dependency set because the surface path only works with
  it present.

Consequence: the blob's WSI is `ANativeWindow` + gralloc + ION (and optionally binder
regions), i.e. Android Bionic framework objects. A Linux driver cannot bind this layer;
it needs a new EGL platform (fbdev, surfaceless, or a compositor buffer path).

---

## 9. Reuse vs rewrite

| component | blob evidence | open equivalent | verdict for Nura |
|---|---|---|---|
| Midgard ISA + shader compiler | Clang 3.6/LLVM 3.6, 11,809 C++ exports, §6 | Mesa `src/panfrost/compiler` (Midgard) | **reuse Mesa**, blob compiler is closed/AArch32 and unwanted |
| Midgard job descriptors / tiler / FBD | cmar region `0x28e9ec..0x28f0xx`, draw dispatcher `0x497f00` | Mesa `pan_cmdstream.c`, `pan_job.c` (Midgard paths) | reuse Mesa descriptor logic; re-target submission |
| batch/atom scheduling | `0x28ea94` flush, `0x265c94` submit | Mesa `pan_job` + `kbase_kmod` `JOB_SUBMIT` | reuse `pan_job`; replace its DRM batch submission with the kbase backend (the DRM batch layer does not exist here) |
| kbase memory/VM/submit | `0x266604` alloc, `0x266168` tracking page, `0x265fb8` mmap, `0x2664bc` import, `0x265c94` submit | session Mesa tree: `src/panfrost/lib/kmod/kbase_kmod.c` (UK 10.6, commit `79478b3`) | **already written as a bring-up backend**; dma-buf import/export, timeline sync, perf, eviction, timestamps return `-ENOSYS` |
| screen/context wiring | — | `src/gallium/drivers/panfrost/panfrost_kbase_skeleton.c` (commit `4b96404`) | **skeleton only**: creates screen+context, allocates SAME_VA memory; "rendering not implemented" |
| sync/fence | `0x265c10`/`0x265f04`/`0x266ad4` | `kbase_sync_*` util_sync provider in `kbase_kmod.c`; signal/timeline/wait/query are `-ENOSYS` | finish/validate |
| EGL platform | `ANativeWindow`/gralloc/ION, §8 | Mesa EGL platform + GBM/DRM | **missing**: surfaceless first, then fbdev; no GBM/DRM here |
| GLES state machine | 598 `gl*` exports, dispatch table | Mesa Gallium `panfrost` (`pan_cmdstream.c`, `pan_job.c`, Midgard paths) | reuse Mesa; wire its submit path to the kbase backend |

The layer that had to be written for the chosen "Mesa + kbase" path is therefore
**the kbase winsys (legacy UK 10.6 pan_kmod backend) plus a WSI**. The Midgard compiler
and the Midgard command-stream encoding already exist in Mesa (`midgard_compile.c`,
`pan_cmdstream.c`, `pan_job.c`); they are not the gap. In this session the chosen Mesa
tree (`parity/userspace/gpu/mesa/src`, branch `kbase-winsys`) now contains a UK 10.6
backend at exactly that layer — `kbase_kmod.c` (1,587 lines, commit `79478b3`) —
plus a screen/context bring-up skeleton (commit `4b96404`). Cross-check with the blob:
the backend's `KBASE_UK_IOCTL(size)` differs cosmetically from the blob's
`_IOWR(0x80,id&0xff,size)` (both carry only the size, which is all the kernel reads), and
its wire sizes (16/80/56/48/40/64/24) match the r15p0 headers and the blob's r7p0
patterns. What remains is renderer integration, WSI and validation, not a new
memory/atom layer. The blob's own implementation (`product/base`, `product/egl`, cmar)
is closed/AArch32 and can only inform the design.

### 9.1 Realistic effort (static estimate, no hardware)

| work item | estimate | basis |
|---|---|---|
| finish/validate the existing UK 10.6 backend: dma-buf import/export, timeline/sync semantics, eviction, timestamps, error paths | 2–4 person-weeks | `kbase_kmod.c` already does open/version, tracking page, SAME_VA alloc/mmap, submit 40 B/stride 64, event read; the `-ENOSYS` surface is small but on the critical path |
| wire the Midgard Gallium driver to kbase submission (replace the DRM batch path; vertex/tiler/fragment chains, tiler heap, FBD, scoreboard) | 6–12 person-weeks | `pan_cmdstream.c`/`pan_job.c` exist with Midgard paths; the screen/context skeleton does not render yet |
| EGL platform/WSI: surfaceless + fbdev presentation (no DRM/GBM), buffer import without GBM | 3–6 person-weeks | display is fbdev-only (`CONFIG_EXYNOS_DECON_FB`), no KMS |
| hardware validation, tiling/format bugs, dEQP subset | open-ended, +4–8 weeks | no j7elte hardware available here |

Order of magnitude: **~3.5–6 person-months** for GLES 2.0-class acceleration on the
kbase kernel, assuming an experienced Panfrost/kbase developer and hardware access —
smaller than the 6–18 person-month mainline-Exynos route, but still a driver project.
The kbase memory/atom backend is no longer the long pole; renderer integration and WSI
are.

---

## 10. Why the blob itself is not reusable

1. **ELF class/ABI**: `EM_ARM`, AArch32, bionic (`/system/bin/linker`), Android 23.
   Nura userspace is AArch64/musl; the object cannot be loaded, and the ROM contains no
   AArch64 GL/HAL variant.
2. **UAPI generation**: blob is DDK r7p0 / UK 10.0 (submit 32 B, atom 56 B with
   SYSTRACE); the Nura kernel is r15p0 / UK 10.6 (submit 40 B, atom 64 B,
   `core_req` u32 @44). The kernel rejects the blob's first `JOB_SUBMIT` on size before
   parsing any payload. Vendor ids also differ (blob 552/553/567/568 are the r7p0
   Samsung block; r15p0 shifts the vendor block and adds entries).
3. **WSI**: `ANativeWindow` + gralloc + ION + SurfaceFlinger regions; no DRM, no GBM,
   no Linux window-system path.
4. **Closed compiler**: usable only by the matching closed cmar/GLES layers.

None of these are fixable by loading the blob under a compatibility shim in a Linux
stack; the shim would have to re-encode the entire UK 10.0→10.6 memory/atom protocol and
provide Android's surface objects.

---

## 11. Artifacts and method limits

* `parity/userspace/gpu/blob/libGLES_mali.so` — fresh extraction from `system.raw`
  (hash above, identical to the earlier `re-hybris/egl_extract` copy).
* `parity/userspace/gpu/blob/lib/` — the 12 `DT_NEEDED` libraries, the framework
  `libEGL.so` wrapper, and the two Exynos HAL modules, with hashes in §1.
* `parity/userspace/gpu/blob/ghidra-out/` — objdump extracts used for the address
  citations (`base_region.dis`, `egl*.dis`, `objdump_scan.txt`), the packed-reloc
  analysis output, and the Ghidra exports (`quick_functions.tsv` — 3,811 functions,
  `quick_strings.tsv` — verified string xrefs).
* `parity/userspace/gpu/blob/tools/` — Ghidra export script (`ExportBlobMap.java`) and
  the PC-relative xref scanners used to recover string references
  (`scan_pcrel*.py`).
* Mesa comparison tree (read-only): `parity/userspace/gpu/mesa/src` branch
  `kbase-winsys` at `4b96404` (`kbase_kmod.c` commit `79478b3`, skeleton `4b96404`).
* `parity/userspace/gpu/blob/ghidra-project-quick/` — Ghidra headless project
  (`ARM:LE:32:v7`, `-noanalysis`). An earlier full-auto-analysis pass on the 23 MB blob was
  aborted after 35 min (it had produced 183 MB of analyzer log and hit host temp-space
  pressure); the quick import is what the quoted Ghidra boundaries come from. No
  decompiler output is relied upon.

Limits: static only. No `/dev/mali0`, no GPU, no Android runtime. Function boundaries for
non-exported DDK code are inferred from prologues/call targets; decompilation was used
only to confirm the call shapes quoted above. The exact Midgard descriptor byte layouts
emitted by the draw path (`0x497f00`) were not decoded — that belongs to a driver
implementation, not this inventory. All claims about the Nura kernel's r15p0 behaviour
are from the in-tree sources cited in `parity/userspace/gpu/kbase/analysis.md`, not from
running the blob.
