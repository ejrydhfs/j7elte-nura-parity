# Mesa on kbase — first open backend milestone (Mali-T720 / Exynos 7580 / j7elte)

Scope: build the first real, buildable milestone of a **kbase winsys** for
Mesa's Panfrost Gallium driver on the Nura 3.10 kernel, which exposes ARM
kbase **r15p0 / UK 10.6** through `/dev/mali0` instead of DRM.

Delivered:

| Path | What |
|---|---|
| `parity/userspace/gpu/kbase-winsys/` | standalone, ABI-verified kbase winsys library + bring-up self-test |
| `parity/patches/gpu-mesa-kbase/0001…0003` | Mesa 27.0.0-devel patch series: `pan_kmod` kbase backend, backend-neutral Gallium JM submission, screen/context bring-up skeleton |
| `parity/userspace/gpu/mesa/src` | Mesa checkout (`main` @ `02f1337`, branch `kbase-winsys` with the three commits) |

**No hardware was available. No GL/EGL renderer is claimed**, nothing was
executed against the GPU, and the winsys self-test reports "bring-up not
executed" (exit 2) on this host exactly as designed.

---

## 1. Verdict

| Question | Answer |
|---|---|
| Does the kernel expose enough for a Mesa winsys? | **Yes.** Handshake, SAME_VA memory, 64-byte atom submission, per-atom completion events, fd-backed fences (soft fence trigger/wait on an Android sync timeline), context per fd. All of it is used by the delivered winsys. |
| Where is the Mesa insertion point? | `src/panfrost/lib/kmod`: Mesa already abstracts the KMD behind `struct pan_kmod_ops` + `util_sync_provider` and already carries two backends (panfrost/DRM JM, panthor/DRM CSF). A third backend for UK 10.6 is the natural, minimal-risk path. |
| Is the winsys real? | Yes: 1,258-line library + 219-line API, verified against the kernel's own headers (`make check-abi`), built and run on x86-64 and aarch64 (static, qemu). |
| Does Mesa build with it? | `libpankmod_lib.a`, `libpanfrost.a` (Gallium driver) and `libpanfrost-v4.a` build and link; the kbase backend self-test links and runs against Mesa. The skeleton *executable* link is blocked by an environment prerequisite (Mesa's Panfrost OpenCL front end needs `mesa_clc` → LLVM, absent here), not by this code. |
| Does GL work? | **No.** There is no WSI/presentation path on this kernel (`# CONFIG_DRM is not set`, no dma-buf export usable from kbase), no hardware validation of Midgard job chains, and no draw path was claimed. |

---

## 2. Kernel interface facts this milestone is built on

All line numbers are `drivers/gpu/arm/t7xx/r15p0/`, Nura tree
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`.

### 2.1 Transport and handshake

* One ioctl (`kbase_ioctl`), cmd's number/type ignored: only `_IOC_SIZE(cmd)`
  is used (`mali_kbase_core_linux.c:1342-1365`). The function id is the first
  u32 of the argument buffer (`kbase_dispatch`, `:279`).
* `UKP_FUNC_ID_CHECK_VERSION` first, then `KBASE_FUNC_SET_FLAGS` before
  anything else (`kbase_api_handshake`, `:170-219`, `:317-351`).
* The **memory tracking page** must be mapped before any BO: mmap at
  `BASE_MEM_MAP_TRACKING_HANDLE` (`0x3000`), otherwise every BO mmap returns
  `-EINVAL` (`mali_kbase_mem_linux.c:2160-2168`, `kbase_tracking_page_setup`
  `:2512-2536`).
* `GET_GPU_PROPS_REG_DUMP` returns a fixed 528-byte `mali_base_gpu_props`.
  Its layout was measured from the kernel headers with a purpose-built probe
  (`kbase-winsys/tools/kbw-props-layout.c`): `core_props@0`, `l2@48`,
  `tiler@64`, `thread@72`, `raw@96` (`gpu_id@232` absolute), `coherency@256`,
  `sizeof = 528`, `GPU_MAX_JOB_SLOTS = 16`.

### 2.2 Memory

* 64-bit clients always get `BASE_MEM_SAME_VA`. `MEM_ALLOC` returns a cookie;
  the VA is installed by `mmap(fd, pages, RW, MAP_SHARED, cookie)` and the
  GPU address **is** `vma->vm_start` (`kbasep_reg_mmap`,
  `mali_kbase_mem_linux.c:2058-2124`). CPU VA == GPU VA.
* Consequence for a winsys: there is no "allocate GPU memory without CPU
  mapping". Every BO must be mmapped at allocation time, including
  `PAN_BO_INVISIBLE` ones. The Mesa backend does exactly that.
* `commit_pages` is the initial backing. A growable allocation (tiler heap)
  must commit one page, otherwise `MEM_ALLOC` backs the whole VA range. Both
  the winsys and the Mesa backend do this (`GROW_ON_GPF` → `commit_pages=1`).
* Free is `munmap` for SAME_VA regions; `MEM_FREE` on a SAME_VA cookie is
  rejected (`mali_kbase_mem.c:1243-1249`).

### 2.3 Jobs

* `JOB_SUBMIT` takes a stride-64 array of `base_jd_atom_v2`
  (`mali_kbase_jd.c:1330-1350`). `core_req` is the u32 at offset 44.
* Atom slots are a persistent 256-entry table indexed by `atom_number`
  (`:1420`); submitting to a busy slot **blocks** in `wait_event_killable`
  (`:1445-1475`). A clientside slot allocator is mandatory.
* Dependencies (`pre_dep[].atom_id`) resolve against that same table.
  A dependency on a slot that is `UNUSED` with `event_code != DONE` fails the
  dependent atom (`:1040-1160`). Therefore, inside one submit, the wait atom
  must precede the job atoms and the fence-trigger atom must follow them.
* Every completed atom posts one 24-byte `base_jd_event_v2` through `read()`
  unless `BASEP_JD_REQ_EVENT_NEVER`/`ONLY_ON_FAILURE` is set
  (`mali_kbase_event.c:217-247`). `read()` blocks on an empty queue
  (`mali_kbase_core_linux.c:1366-1402`); `poll()` is supported, so a winsys
  event thread can poll.
* `core_req` mapping used by the Mesa backend: `PANFROST_JD_REQ_FS` →
  `BASE_JD_REQ_FS`, otherwise `BASE_JD_REQ_CS | BASE_JD_REQ_T` (kbase only
  uses the bits for slot selection/DVFS; the real job types live in the
  Midgard job-chain headers).

### 2.4 Fences — the important finding

UK 10.6 has a usable fd-based fence primitive, which is what makes a real
Panfrost Gallium fence implementation possible:

* `KBASE_FUNC_STREAM_CREATE` (`UK_FUNC_ID + 24` = 536,
  `mali_kbase_core_linux.c:725-748`, header `mali_kbase_uku.h:213`) returns an
  Android sync-timeline fd. `CONFIG_SYNC=y` in the Nura config
  (`config-nura-j7elte:3173`).
* `BASE_JD_REQ_SOFT_FENCE_TRIGGER` (`BASE_JD_REQ_SOFT_JOB | 2`) takes
  `atom->jc = <CPU address of struct base_fence {fd, stream_fd}>`; during
  submit, the kernel creates the fence fd and writes it back
  (`mali_kbase_softjobs.c:1394-1425`, `kbase_fence_trigger`
  `:199-232`). So **the out-fence fd exists as soon as the submit ioctl
  returns**, and signals when the atom completes.
* `BASE_JD_REQ_SOFT_FENCE_WAIT` (`| 3`) waits on a fence fd; it is a soft atom
  that other atoms can depend on.
* kbase fences are Android sync fences: `poll()` reports `POLLIN` on
  completion, which is how both the winsys and Mesa's sync provider wait.
* Caveat: the Samsung-modified trigger calls `kbase_sync_signal_pt(pt, 0)`
  unconditionally (`mali_kbase_softjobs.c:214-224`), so a triggered fence
  signals "success" even if the job faulted. Errors have to be detected from
  the completion events (see §7).

---

## 3. Delivered winsys library

`parity/userspace/gpu/kbase-winsys/` (MIT, links the ABI reference client in
`../kbase/` which is GPL-2.0; the Mesa backend is independent, see §4).

```
include/kbase_winsys.h     219 lines  public API
src/kbase_winsys.c        1258 lines  device/screen/context/BO/submit/fence
tests/kbw-selftest.c       229 lines  bring-up self-test
tools/kbw-props-layout.c   110 lines  GPU-props layout probe vs kernel headers
Makefile                             host + aarch64 + check-abi
```

### 3.1 API shape (deliberately the same shape as `pan_kmod`)

* `kbw_device_open(path)` / `kbw_device_adopt_fd(fd)` — open, UK 10.6
  handshake, tracking page, DDK version, props → `struct kbw_caps`
  (gpu_id/product/revisions, shader/tiler/L2/AS/JS present, all feature
  registers, core count, VA bits, job slots, coherency).
* `kbw_screen_create()` — device + policy (one per `pipe_screen`).
* `kbw_context_create(flags)` — `KBW_CONTEXT_SHARE_SCREEN_FD` (one kbase
  context, what Mesa needs) or `KBW_CONTEXT_NEW_FD` (separate kbase context,
  standalone use).
* `kbw_bo_create(size, flags)` / `kbw_bo_cpu()` / `kbw_bo_gpu_va()` /
  `kbw_bo_destroy()` — MEM_ALLOC + SAME_VA mmap, growable handling.
* `kbw_submit()` — one `JOB_SUBMIT` batch:
  `[fence-wait atom] [job atoms…] [fence-trigger atom]`.
  `dep[]` are indexes into the batch; `in_fence_fd` becomes a wait atom that
  every job atom depends on; `out_fence_fd` is the trigger atom's fd.
* `kbw_fence_*` — wrap/wait/poll/dup an Android sync fence fd.
* `kbw_soft_dump_time()` — `BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME` round trip
  through the same submit/event machinery (the milestone's "one soft-job /
  event round trip").

### 3.2 Runtime mechanics

* One event thread per context: `poll()` on the kbase fd + a stop pipe,
  one blocking `read()` per event, frees the atom slot and records the event
  code under a mutex/condvar.
* Atom slots 1…255; slot 0 is reserved by kbase as "no dependency". Slot
  allocation waits (condvar, monotonic clock) with a 5 s default deadline so a
  wedged GPU cannot hang the caller forever.
* The submit frame is heap-allocated (≈18 KiB) because a 256-atom worst case
  does not belong on the stack.

### 3.3 Verification

```
$ make            # x86-64, no warnings
$ ./build/kbw-selftest
uk version 10.6 / base_jd_atom_v2 64 / kbase_uk_job_submit 40 / ...
device /dev/mali0 unavailable: ... bring-up not executed (no hardware)
exit 2

$ make aarch64    # static ELF aarch64, GCC 10.3-compatible sources
$ qemu-aarch64 ./build-aarch64/kbw-selftest
same output, exit 2

$ make check-abi
size struct mali_base_gpu_props             528
GPU_MAX_JOB_SLOTS 16
props layout matches winsys static asserts
```

The device-present path (handshake → props → BO → soft-job completion →
out-fence → in-fence → idle) is implemented and self-checks the SAME_VA
invariant, the CPU mapping and the counters, but was **not executed against
the GPU** (none attached). That is exactly the residual risk R1 in §7.

---

## 4. Mesa patch series

Applied to a clean Mesa `main` (`02f1337`, version 27.0.0-devel); all three
apply cleanly (`git apply --check`).

### 4.1 0001 — `panfrost/kmod`: kbase backend (self-contained, MIT)

New `src/panfrost/lib/kmod/kbase_kmod.c` (+1,599 lines) implements
`struct pan_kmod_ops` for UK 10.6, with its own UK wire definitions and
compile-time ABI asserts (atom 64, submit 40, mem_alloc 56, event 24,
props 528). It deliberately shares no code with the parity tree, so the Mesa
patch is licence-clean and self-contained.

Integration map (all inside the existing Mesa abstraction):

| `pan_kmod` hook | kbase implementation |
|---|---|
| `dev_create` | reject DRM fds via `drmGetVersion`; else handshake → tracking page → 528-byte props → `pan_kmod_dev_init`; create sync timeline + event thread |
| `dev_destroy` | stop thread, destroy VM, `pan_kmod_dev_cleanup` (closes fd if owned, finalizes sync) |
| `bo_alloc` | `MEM_ALLOC` + SAME_VA `mmap`; `commit_pages = 1` for `ALLOC_ON_FAULT`; handle = per-device counter |
| `bo_mmap` | returns the address fixed at allocation (SAME_VA) |
| `bo_free` / `bo_wait` | `munmap`; always "ready" (same pages, ordering via fences) |
| `bo_import` / `bo_export` | unsupported (no dma-buf path on this kernel) |
| `vm_create` | one VM, 4 KiB pages |
| `vm_bind` | MAP(AUTO_VA): returns the BO's SAME_VA address; UNMAP: no-op; sparse/partial: rejected |
| sync provider (`util_sync_provider`) | 127 binary slots (handle 0 = "none"); create/destroy/wait/import/export/fd_to_handle/transfer; wait = `poll()` loop; signal/timeline/query = `ENOSYS` |
| `submit_job` (new optional op) | translate `drm_panfrost_submit`: requirements → `core_req`; `in_syncs[0]` → wait atom; `out_sync` → trigger atom whose fd is installed into the sync slot; `bo_handles` need no action because every BO is already SAME_VA-mapped in the context |
| `jm_ctx_create/destroy` (new optional ops) | kbase has one context per fd; return handle 0 / no-op |
| perf, eviction, timestamp | unsupported (`NULL`/no-op) |

Also new: `kbase_kmod_test.c`, a hardware-independent test registered as
`kbase-kmod` in the `panfrost` suite. It checks that the kbase backend
rejects non-kbase fds (no crash, no consumed handshake) and, if
`/dev/mali0` exists, creates a device, a VM and a BO and validates the
SAME_VA mapping.

### 4.2 0002 — backend-neutral Gallium JM submission

`pan_jm.c` now uses the optional ops when present:

* `jm_init_context` → `ops->jm_ctx_create` if set (kbase: handle 0), else the
  existing `DRM_IOCTL_PANFROST_JM_CTX_CREATE`.
* `jm_cleanup_context` → `ops->jm_ctx_destroy` if set, else the DRM ioctl;
  the default-context path waits on the context sync object through
  `pan_kmod_sync_wait` instead of `drmSyncobjWait`.

`panfrost_kmod_submit()` dispatches to `ops->submit_job` when a backend
provides it, otherwise the DRM ioctl path is unchanged. No other Gallium
code is touched; the DRM backends keep their exact behaviour.

### 4.3 0003 — Gallium screen/context bring-up skeleton

New executable `panfrost-kbase-skeleton` (source
`src/gallium/drivers/panfrost/panfrost_kbase_skeleton.c`):

1. builds the driver option cache from Panfrost's own `driinfo_panfrost.h`
   (there is no DRI loader for a kbase fd, so the program does what the
   loader would do);
2. `panfrost_create_screen(fd, &config, NULL)` — runs the real
   `panfrost_open_device()`: `pan_kmod_dev_create` → kbase backend, T720 model
   lookup, `panfrost_device_init`, 128 MiB tiler heap + sample-position BOs
   allocated through kbase SAME_VA memory;
3. `screen->context_create()` — real `panfrost_context` creation, JM context
   through the kbase op;
4. prints `get_name/get_vendor/get_device_vendor`, then destroys everything.

It issues **no draw, clear or blit** and exposes no GL entry point.

### 4.4 Build result

Configured (see `apply.sh` for the exact command):

```
meson setup build-kbase -Dgallium-drivers=panfrost -Dvulkan-drivers= \
  -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dpanfrost-rust=false \
  -Dbuild-tests=true
```

Built and linked:

* `src/panfrost/lib/kmod/libpankmod_lib.a` — with `kbase_kmod.o`;
* `src/gallium/drivers/panfrost/libpanfrost.a` — full Gallium driver,
  including the modified `pan_jm.c`;
* `src/gallium/drivers/panfrost/libpanfrost-v4.a` — T720 arch library and the
  Midgard compiler path;
* `src/panfrost/lib/kbase-kmod-selftest` — links against Mesa and runs:

```
MESA: error: kbase: handshake failed on fd 3
ok: /dev/null rejected
no /dev/mali0: kbase device checks skipped
exit=2
```

* `panfrost-kbase-skeleton.c.o` — compiles against the driver headers.

**Not completed in this environment:** linking the skeleton executable
(and therefore any full `libgallium_dri`). Mesa 27's Panfrost enables the
`libpan` OpenCL front end unconditionally, whose shaders are built with
`mesa_clc`, which requires LLVM; this host has no LLVM (`llvm-config` absent)
and no `bindgen`. `meson setup` itself only succeeds with
`-Dmesa-clc=system` plus a stub in `PATH`. This is an environment
prerequisite, not a defect in the delivered code: `libpan` is never called by
the GL screen/context path. Installing LLVM (`mesa_clc` builds from tree) is
the exact unblock; the alternative is a Mesa configure option that disables
`libpan` for panfrost (does not exist today).

---

## 5. What a Gallium driver gets, and what it still needs

Working through the Panfrost Gallium flow against this backend:

* `panfrost_open_device` → works (props, model, core count, VM, tiler heap,
  sample positions).
* `panfrost_bo_create/mmap/free` → works (SAME_VA, GPU VA from the mapping).
* `panfrost_context_create` → works (JM handle 0).
* `panfrost_batch_submit` → translated to one kbase atom per `jm_submit_jc`
  call (vertex/tiler chain, then fragment chain) with the context syncobj as
  out-fence; in-fence from the previous submit works.
* `panfrost_fence_finish/get_fd` → works through the kbase sync provider
  (poll on the sync fence fd / dup).
* `screen->get_timestamp` → returns 0 (no timestamp ioctl; kbase exposes
  `SOFT_DUMP_CPU_GPU_TIME` instead).
* WSI, dma-buf import/export, modifiers, eviction, perf → missing (§6).

---

## 6. Remaining userspace / WSI work (ordered)

1. **Presentation (blocking for any visible GL).** `# CONFIG_DRM is not set`
   (`config-nura-j7elte:2262`) and Nura has no KMS/GBM stack; there is also no
   dma-buf export in the UK 10.6 path used here. An on-screen GL path needs
   either a DRM/KMS driver plus a display stack (see `gpu-panfrost.md`, ~7–10
   PM) or an fbdev/Android-compositor bridge with a CPU-visible scanout path.
   Offscreen EGL could be built first, but is still blocked on (2).
2. **dma-buf / buffer sharing.** `BASE_MEM_IMPORT` exists in the kernel
   (`mali_kbase_uku.h:106`, `mali_kbase_core_linux.c:385-410`) for
   ion/UMM/user-buffer handles, but this kernel exposes no dma-buf-fd import
   and the egl/gbm path needs `dmabuf` fds. Implementing `bo_import/export`
   requires a kernel-side dma-buf export/import (or a gralloc shim) — out of
   this milestone and out of this file's ownership.
3. **Sync-file interop.** kbase fences are Android sync fences. A compositor
   or Wayland/X11 client supplies dma-buf (sync_file) fences, which
   `sync_fence_fdget` rejects. Needs a translation step (userspace thread that
   waits on the sync_file then triggers a kbase fence), or a kernel bridge.
4. **Fault reporting through fences.** The Samsung fence trigger signals
   success unconditionally. To make `panfrost_fence_finish` meaningful, map
   completion events back to submit fences in the event thread and signal a
   software error state (or expose it through a `PAN_MESA_DEBUG` check).
5. **Real Midgard job-chain validation.** kbase passes `atom.jc` to the JM
   hardware unchanged; the only remaining risk on silicon is chain termination
   and the tiler-heap layout (Panfrost builds standard JT headers, so this is
   expected to work, but it is unverified here).
6. **Coherency.** BOs are requested with `BASE_MEM_COHERENT_SYSTEM` and
   `CACHED_CPU`; the kernel may drop `COHERENT_SYSTEM` if the platform does
   not support it and returns the effective flags. `pan_kmod_dev_props.is_io_coherent`
   is derived from the GPU's selected coherency mode; on a non-coherent boot
   Panfrost's userland cache maintenance path must be exercised on aarch64.
7. **Multi-context / cross-screen sharing.** One kbase fd is one context and
   owns the BOs/VM. Multiple Gallium screens would be multiple kbase contexts
   with no shared memory until (2) lands.
8. **Perf counters, timestamps, eviction, sparse VM.** Not implemented;
   `PERFETTO`/`PAN_MESA_DEBUG=perf` paths must stay disabled.
9. **Hardware bring-up.** Run `kbase-kmod-selftest`, then
   `panfrost-kbase-skeleton`, then a single `glClear`-equivalent batch with
   `PAN_MESA_DEBUG=trace,sync` to validate the chain and the fence.

---

## 7. Residual risks

* **R1** The winsys' device-present path was derived line-by-line from the
  kernel sources but not executed on the GPU (no `/dev/mali0` here). The
  self-test prints expected results and exits 2 when the node is absent.
* **R2** The Mesa backend was compile/link-validated; its runtime behaviour
  against the kernel is untested. The freestanding winsys covers the same UK
  sequences, so the first hardware test should compare the two.
* **R3** The full Mesa driver/skeleton link could not be completed because
  `libpan` needs LLVM/mesa_clc; see §4.4. No claim of a built `libgallium_dri`
  or EGL is made.
* **R4** kbase's 256 atom slots are a hard limit; a client submitting more
  than 255 atoms in flight blocks in the ioctl. The winsys bounds this with a
  timeout, but a GPU hang still surfaces as a timeout, not as a fence error.
* **R5** The Midgard compiler in this Mesa (arch 4, T720 model present) was
  built (`libpanfrost_compiler.a`, `libpanfrost-v4.a`) but never fed a shader
  through the new backend.

---

## 8. Reproduction

```
# winsys
cd parity/userspace/gpu/kbase-winsys && make && make aarch64 && make check-abi

# Mesa
cd parity/patches/gpu-mesa-kbase && ./apply.sh /path/to/mesa
cd /path/to/mesa && meson setup build-kbase ... (see §4.4)
ninja -C build-kbase src/panfrost/lib/kbase-kmod-selftest \
      src/gallium/drivers/panfrost/libpanfrost.a
./build-kbase/src/panfrost/lib/kbase-kmod-selftest    # exit 2 without hardware
```
