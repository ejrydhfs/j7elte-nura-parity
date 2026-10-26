# GPU path — RE inventory of the ROM's Mali blob (r7p0)

Scope: static reverse engineering of `/vendor/lib/egl/libGLES_mali.so` from the j7elte
`system.raw` (Android 6.0.1, `SM-J700M J700MUBU2BRC2`), plus its Android dependencies.
Deliverable is a map, not a driver: no code was written for GL, nothing was run on a
device, and no working userspace stack is claimed. Detailed evidence:
`parity/userspace/gpu/blob/map.md`.

**Verdict**

| Question | Answer |
|---|---|
| What is the blob? | 23,368,296 B ELF32 ARM EABI5 DYN, bionic/Android 23, `Midgard-"r7p0-03rel0"`, sha256 `62e10725…30be317`. It is the whole closed Mali DDK (EGL+GLES+OpenCL+Clang 3.6/LLVM 3.6) in one DSO. |
| Does it drive kbase directly? | **Yes.** One `ioctl` site (`0x265480`) with `_IOWR(0x80, id&0xff, size)`; UK handshake 10.0 at `0x265db4`; 23 kbase call sites mapped to UAPI ids (§2). |
| Can it run on Nura? | **No.** AArch32/bionic, and the Nura kernel is r15p0/UK 10.6 while the blob is r7p0/UK 10.0: 32-byte submit/56-byte atom vs 40/64. First `JOB_SUBMIT` is rejected on size. |
| Is the compiler the gap? | **No.** Mesa Panfrost already has an open Midgard compiler and command-stream encoder. The blob's Clang 3.6 pipeline is closed and unnecessary. |
| What is actually missing for the chosen Mesa-kbase path? | The **legacy UK 10.6 kbase winsys** plus a **WSI**. In this session the Mesa tree (`parity/userspace/gpu/mesa/src`, branch `kbase-winsys`) already has `src/panfrost/lib/kmod/kbase_kmod.c` (UK 10.6, commit `79478b3`, with dma-buf/timeline/perf/eviction/timestamps returning `-ENOSYS`) and a screen/context skeleton (`4b96404`, "rendering not implemented"). Remaining: finish the sync/dma-buf surface, wire the Midgard Gallium renderer to kbase submission, provide EGL/WSI (no DRM/GBM; display is fbdev), validate. |
| Effort | ~**3.5–6 person-months** for GLES 2.0-class acceleration on kbase (finish/validate winsys 2–4 wk, Midgard renderer integration 6–12 wk, EGL/WSI 3–6 wk, validation open-ended), vs 6–18 person-months for mainline Exynos + panfrost. Static estimate; no hardware here. |

---

## 1. Extract and provenance

Extracted with `debugfs` from the ext4 `system.raw` (image root is the system partition; full hashes are in `map.md` §1):

| artifact | size | sha256 |
|---|---|---|
| `libGLES_mali.so` (`/vendor/lib/egl/`) | 23,368,296 | `62e10725f1077ffa483224de5a83c14a0a0ccdccb51cd094177b7bd1630be317` |
| `libEGL.so` (`/lib/`, framework wrapper) | 461,264 | `288e70cf…f676f6` |
| `libbinder.so` | 181,880 | `40bbe649…5c7784` |
| `libc++.so` | 575,088 | `ec564695…8feef7` |
| `libc.so` | 678,392 | `204785e4…4d8a29be` |
| `libcutils.so` | 63,204 | `d0dadf19…1a3750` |
| `libdl.so` | 9,492 | `391cae6a…0eee7` |
| `libhardware.so` | 17,916 | `ec1843a7…59405` |
| `libion.so` | 13,820 | `e28b2d6e…389c5` |
| `liblog.so` | 30,324 | `2d2e75c9…2a048` |
| `libm.so` | 132,824 | `b2a8260e…b526f` |
| `libui.so` | 58,980 | `c5ad8c1d…f31de5` |
| `libutils.so` | 104,052 | `697751aa…54c1` |
| `libz.so` | 112,224 | `7aab6f7e…c59e` |
| `gralloc.exynos5.so` (`/lib/hw/`) | 26,576 | `9a822343…ca5635` |
| `hwcomposer.exynos5.so` (`/lib/hw/`) | 30,336 | `6d46e5d5…e2ce36` |

All are ELF32/ARM/EABI5, interpreter `/system/bin/linker`; full hashes are in `map.md`
§1. Build metadata: GCC 4.9.x-google + `clang version 3.6`, gold 1.11, ARMv7-A/VFPv4/
NEON, aggressive-debug, Android SDK 23. `DT_NEEDED`: `libz, libdl, libhardware, libui,
libutils, libcutils, libbinder, liblog, libc++, libion, libc, libm`. `BIND_NOW`.

Relocation facts: `.rel.dyn` is packed `SHT_ANDROID_REL` "APS2" with **96,168** entries
(88,977 `R_ARM_RELATIVE`, 6,437 `R_ARM_ABS32`, 754 `R_ARM_GLOB_DAT`) and no stored
addends; `.rel.plt` has 10,825 slots. String references were therefore recovered from
PC-relative `ldr`+`add` sequences, not from relocations.

---

## 2. kbase ioctl client (the main RE result)

`0x265480` is the only Mali `ioctl` caller in the DSO (the other two `ioctl` sites are
LLVM's terminal-width probe at `0x1276310`/`0x1276388`):

```
265480  ldr r3,[r1]              ; uk_header.id
265490  ldr r0,[r0]              ; fd = *(kbase_context)
265494  orr r1,r3,#0xc0000000
265498  orr r4,r1,#0x8000        ; _IOC type 0x80
26549c  orr r1,r4,r2,lsl #16     ; cmd = _IOWR(0x80, id&0xff, size)
2654a4  bl  ioctl@plt
2654b4  movne r0,#3 ; moveq r0,#0
```

The kernel reads only `_IOC_SIZE(cmd)`, so the size is the real selector; the id lives in
the argument buffer. Device bring-up is `0x2654c0`: it formats `"/dev/mali%d"` from
`0x12d8900` + `"%s%d"` (`0x265538/0x26553c`), opens `O_RDWR|O_CLOEXEC`, requires a char
device, then sends `UKP_FUNC_ID_CHECK_VERSION` (id 0, 16 B) with **10.0** (built at
`0x265db4`). A mismatch takes the `"file %s is not of a compatible version"` path
(`0x2655e4`, string `0x12d8934`).

Mapped call sites (args size from the `r2` immediate at each call):

| call site | id | UAPI | size |
|---|---|---|---|
| `0x2655b4` | 0 | `UKP_FUNC_ID_CHECK_VERSION` | 16 |
| `0x26585c` | 530 | `SET_FLAGS` | 16 |
| `0x265a20`/`0x265a84` | 526 | `GPU_PROPS_REG_DUMP` | 536 |
| `0x265c10` | 520 | `SYNC` | 40 |
| `0x265c94` | **540** | `JOB_SUBMIT` | **32** |
| `0x265d1c` | 521 | `POST_TERM` | 8 |
| `0x265e4c` | 522 | `HWCNT_SETUP` | 40 |
| `0x265e94` | 516 | `MEM_FREE` | 16 |
| `0x265ebc` | 541 | `DISJOINT_QUERY` | 16 |
| `0x2660e8` | 515 | `MEM_QUERY` | 32 |
| `0x266250` | 514 | `MEM_COMMIT` | 32 |
| `0x2663ac` | 518 | `MEM_ALIAS` | 56 |
| `0x2664bc` | 513 | `MEM_IMPORT` | 48 |
| `0x266604` | 512 | `MEM_ALLOC` | 56 |
| `0x2667c0` | 517 | `MEM_FLAGS_CHANGE` | 32 |
| `0x2669b4` | 536 | `STREAM_CREATE` | 48 |
| `0x266ad4` | 535 | `FENCE_VALIDATE` | 16 |
| `0x266b10` | 539 | `DEBUGFS_MEM_PROFILE_ADD` | 24 |
| `0x473210` | **552** | `CREATE_SURFACE` (Samsung) | 24 |
| `0x46e3c8` | **553** | `DESTROY_SURFACE` (Samsung) | 24 |
| `0x466cb4`/`0x466ce8` | **567/568** | secure/non-secure world rendering | 24 |

Ids match `drivers/gpu/arm/t72x/r7p0/mali_kbase_uku.h:408-480` (UK 10.0; vendor block
549–556; secure 567/568). Memory semantics observed:

* tracking page: `mmap64(NULL, 4096, PROT_NONE, MAP_SHARED, fd, 0x3000)` at `0x266168`
  (kernel requires `PFN_DOWN(BASE_MEM_MAP_TRACKING_HANDLE)` first); unmapped at
  `0x2661d0`;
* BOs: `MEM_ALLOC` returns a cookie, `0x265fb8` maps it with
  `mmap64(NULL, pages<<12, prot, MAP_SHARED, fd, cookie)` → CPU VA == GPU VA;
* flag `0x10000` selects the ION path (`0x50e7d4`: `ion_open` → `ion_alloc_fd` with
  flags 16, heap mask `0x20000010`, out-fd as the 6th argument → `ion_close`) and then
  `MEM_IMPORT`;
* events: `0x266870` `read()`s `nr*24` bytes (`base_jd_event_v2`);
* fences: `fcntl(fd,0x406,0)` + `FENCE_VALIDATE` (`0x266ad4`), dependency `SYNC` modes
  1/2 (`0x265f04`/`0x265f50`).

### 2.1 Submit and the ABI break

`0x265c94` builds a 32-byte submit: id 540, atom array address, `nr_atoms`, **stride 56**,
`gles_ctx_handle = [atom+48]`. Stride 56 proves the blob was built with
`CONFIG_MALI_SYSTRACE_SUPPORT` (r7p0 atom 56 B, `core_req` u16 @34). Only one chain
reaches it: `0x28ea94` (cmar job-chain flush) → `0x265cec` → `0x265c94`; after submit it
walks the same groups issuing SYNC. This is the exact DDK↔kbase boundary.

Nura's kernel is r15p0/UK 10.6: `kbase_uk_job_submit` is 40 B and `base_jd_atom_v2` is
64 B (`core_req` u32 @44). The kernel's size check rejects the blob before payload
parsing. This independently confirms the r7p0↔r15p0 break established from the kernel
headers in `parity/userspace/gpu/kbase/analysis.md` §2 — here it is shown from the
userspace side, including the strlen of the blob's atom (`56`).

---

## 3. Exported/API surface

15,011 `.dynsym` entries; 13,917 defined FUNC; 271 undefined FUNC imports. Composition:

* 6,722 `clang::*`, 5,087 `llvm::*`, 587 `LLVM*` C API, 553 `std::*` — the embedded
  shader compiler (Clang/LLVM 3.6);
* 598 GL/GLES, 128 EGL, 90 OpenCL entry points;
* 87 compiler-rt/ARM runtime, 32 `osup_*`, 4 `mali_egl_*`, 28 other.

Key entry points (address/size): `eglGetDisplay 0x469910/44`, `eglInitialize
0x4699a8/2176`, `eglCreateContext 0x468814/1432`, `eglCreateWindowSurface
0x4731d4/112`, `eglDestroySurface 0x46e2bc/316`, `eglMakeCurrent 0x471ff4/2576`,
`eglSwapBuffers 0x47452c/12` → `eglp_swap_buffers 0x473ac8/2660`,
`eglGetProcAddress 0x46af1c/192`; `glCompileShader 0x484498`, `glLinkProgram
0x488e70`, `glUseProgram 0x48cf64`, `glDrawArrays 0x48e6fc`, `glDrawElements
0x48e758`, `glFinish 0x48e8a4`, `glFlush 0x48e8dc`. GL calls are 92-byte stubs through
the current-context helper `0x471d8c` and an internal dispatch index (`[ctx+16]`).

Module split from 106 embedded assert paths: `product/base` (kbase client), `egl`,
`gles`, `cmar` (command marshalling/GPU executor), `cframe`, `cctx`, `cdbg`, `cutils`,
`osu`.

---

## 4. Compiler

The blob contains a complete Clang 3.6.0/LLVM 3.6 compiler (`clang version 3.6`,
`Clang 3.6.0`), with Mali-specific strings: `support-mali@arm.com` (`0x12dda38`,
referenced at `0x2a7fa0`), `not GL_MALI_SHADER_BINARY_ARM` (`0x12f4704`), `Compiler
frontend/middle failed` (`0x142fdc0`/`0x142fe08`). Shader API identifiers are used at
`0x4baa34` (`gles2_programp_shader_source_internal`), `0x4bad2c`
(`gles2_program_compile_shader`), `0x4bb180..` (`gles2_program_shader_binary`); the
shader module registration is at `0x4d18f4`. `glCompileShader` → `0x4bac2c` →
`0x4b1830` (shader cache/lock); link path `glLinkProgram` → `0x4a95cc`. Ghidra places the
GLSL version/command-line front-end in `FUN_0030f50c` (`0x30f50c`, 6,688 B), which
references the `Language version '%s' unknown…` string at `0x310750`; the support-mali
diagnostic lives in `FUN_002a7f88` (xref at `0x2a7fa8`).

This does **not** have to be reverse engineered for Nura: Mesa's Panfrost Midgard
compiler already produces Midgard ISA from NIR (`src/panfrost/compiler/midgard_*`).
The blob compiler is closed, AArch32/bionic, and emits for the DDK's own command
descriptors.

---

## 5. WSI / Android dependencies

* `egl_winsys_get_implementation` (`0x264cb4`) returns a static descriptor table in the
  `0x16e30a8` region (12-byte records naming EGL entry points, e.g.
  `eglGetConfigAttrib` string at `0x12ec730`); `eglGetDisplay` dispatches through it at
  `0x469654`. `eglGetProcAddress` (`0x46af1c`) walks a separate runtime name→function
  table via `0x462578`.
* Window surfaces use the framework `ANativeWindow` vtable (indirect calls in
  `eglCreateWindowSurface` → `0x46db64` and `eglp_swap_buffers` `0x473ac8`); the kernel
  side also gets Samsung `CREATE_SURFACE`/`DESTROY_SURFACE` calls (552/553).
* gralloc: `hw_get_module("gralloc")` at `0x263bc4/0x263bd0`; errors
  `invalid gralloc handle (at %p)` (`0x12d85ac`) and the gralloc-private-layout version
  check. The blob reads Exynos `private_handle_t` fields directly.
* ION: `ion_open`/`ion_alloc_fd`/`ion_close` at `0x50e7e4/0x50e810/0x50e824`.
* binder: only `android::Region` methods are imported (via `libui`), for damage-region
  bookkeeping; `libbinder.so` is a `DT_NEEDED` but no binder IPC is imported.
* `hwcomposer.exynos5.so` is present in the ROM stack but is not imported by the blob.

This is Android-only plumbing. Nura has no DRM/GBM (only fbdev), so none of it carries
over; a new EGL platform/WSI is required.

---

## 6. Reuse vs rewrite

| layer | blob | open equivalent | action |
|---|---|---|---|
| Midgard compiler | Clang 3.6/LLVM 3.6, closed | Mesa `src/panfrost/compiler` | reuse Mesa |
| Midgard descriptors/cmdstream | cmar (`0x28ea94`, draw dispatcher `0x497f00`) | Mesa `pan_cmdstream`/`pan_job` Midgard paths | reuse logic, re-target submission |
| kbase memory/VM/atoms/events | `product/base` (`0x2653d0..0x266c00`) | session Mesa: `kmod/kbase_kmod.c` (UK 10.6, `79478b3`); dma-buf/timeline/perf/eviction/timestamps `-ENOSYS` | **exists as bring-up backend**; finish/validate |
| screen/context wiring | — | `panfrost_kbase_skeleton.c` (`4b96404`) | **skeleton only**: screen+context, no rendering |
| EGL platform/WSI | `ANativeWindow`+gralloc+ION | Mesa EGL + GBM/DRM | **rewrite**: surfaceless, then fbdev; no GBM |
| GLES state machine | 598 `gl*` exports | Mesa Gallium Panfrost | reuse Mesa |

**The missing layer is the kbase winsys and WSI, not the compiler or the command
stream.** The blob's `product/base` shows exactly which UK calls such a backend must
cover (handshake, flags, props, tracking page, SAME_VA mmap, import/alias/commit,
64-byte atom submission on r15p0, SYNC/fence/event). In this session that layer now has
an implementation in the chosen Mesa tree: `kbase_kmod.c` (1,587 lines, UK 10.6) plus a
screen/context skeleton; the blob cross-checks its wire sizes (the backend uses
`_IOC(_IOC_WRITE,0,0,size)`, the blob `_IOWR(0x80,id&0xff,size)`; the kernel reads only
the size in both cases). What is left is renderer integration, WSI and validation:

* finish/validate winsys sync/dma-buf/eviction paths: 2–4 person-weeks;
* wire `pan_cmdstream.c`/`pan_job.c` Midgard paths to kbase submission: 6–12 weeks;
* EGL platform/WSI (surfaceless, then fbdev): 3–6 weeks;
* hardware validation: open-ended, +4–8 weeks.

Order of magnitude **~3.5–6 person-months**, versus 6–18 for mainline Exynos 7580 +
panfrost. Nothing here was built or run; no runtime claim.

---

## 7. Verification status / risks

* Static only: objdump disassembly, `readelf`, packed-reloc decoding, Ghidra headless
  (`ARM:LE:32:v7`). The Ghidra import (`-noanalysis`, `tools/QuickBlob.java`) recovered
  3,811 function bodies below `0x520000` and string references, including
  `FUN_00265480` (ioctl wrapper, 64 B), `FUN_002654c0` (open/handshake, 548 B),
  `FUN_00265c94` (submit, 88 B), `FUN_00266604` (alloc, 444 B), `FUN_0028ea94`
  (job-chain flush, 248 B) and `FUN_0030f50c` (compiler front-end, 6,688 B). An earlier
  full-auto-analysis pass on the 23 MB blob was aborted after 35 min because the analyzer
  log/temp-space pressure made it unusable; no decompiler output is relied upon.
  No `/dev/mali0`, no Android runtime, no GPU. Expected wire behaviour is derived from the
  in-tree r7p0/r15p0 headers, not executed.
* The blob's ABI mismatch is independent of any runtime behaviour: submit size 32 vs 40
  is a compile-time constant in both trees, and the stride 56 is read directly from the
  blob (`0x265c94`).
* Midgard descriptor byte layouts emitted by the draw path were not decoded; the map
  stops at the cmar/job-submit boundary by design.
* Ghidra function bodies are loader-recovered without full auto-analysis; they were used
  to confirm boundaries and string xrefs, not as a decompiler source. All addresses
  quoted here are independently reproducible from the ARM objdump extracts in
  `blob/ghidra-out/` and `.dynsym`, and were cross-checked against the packed relocation
  table and kernel headers.
* No userspace driver code was created; `blob/` contains only extracted binaries, RE
  scripts and analysis output.

Mesa comparison: `parity/userspace/gpu/mesa/src` branch `kbase-winsys` at `4b96404`
(read-only): `kbase_kmod.c` (UK 10.6, `79478b3`), `panfrost_kbase_skeleton.c`
(`4b96404`), `pan_cmdstream.c`/`pan_job.c` (Midgard), `midgard_compile.c`.

Artifacts: `parity/userspace/gpu/blob/map.md` (full map),
`parity/userspace/gpu/blob/` (binaries + `lib/` + `ghidra-out/` + `tools/`),
`parity/userspace/gpu/blob/ghidra-project-quick/` (Ghidra database), and
`ghidra-out/quick_functions.tsv` / `quick_strings.tsv` (Ghidra exports).
