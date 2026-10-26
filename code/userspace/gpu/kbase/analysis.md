# kbase userspace feasibility on j7elte (Mali-T720, Exynos 7580)

Scope: can anything userspace drive the Mali-T720 through the kbase driver that
Nura actually builds? Static analysis plus cross-compilation; **no j7elte
hardware and no `/dev/mali0` were available**, so nothing here is a runtime
result.

Sources are the kernel tree
`android_kernel_samsung_universal7580-2a746c1…` (paths below are relative to
`drivers/gpu/arm/` unless stated), `config-nura-j7elte`, and the primary UAPI
headers of the modern kbase generation (rockchip 6.1 `include/uapi/gpu/arm/`,
quoted where used). The ROM blob facts are from `parity/reports/gpu.md` /
`gpu-userspace.md`.

**Verdict**

| Question | Answer |
|---|---|
| What UAPI does the Nura kernel expose? | Legacy ARM "UK" ABI, DDK **r15p0-00rel0**, **UK 10.6**: one ioctl whose *size* selects the argument buffer, a u32 function id inside that buffer, 569 (`KBASE_FUNC_MAX`). §1 |
| Can the ROM's `libGLES_mali.so` drive it? | **No.** The blob is r7p0 / UK 10.0. Its `KBASE_FUNC_JOB_SUBMIT` buffer is 32 bytes; r15p0 requires 40 (`sizeof(kbase_uk_job_submit)`), and the atom stride is 48 or 56 vs 64. The very first draw call fails at `kbase_dispatch()`/`kbase_jd_submit()`. §2 |
| Can Mesa's experimental kbase backend drive it? | **No.** It speaks the 2017+ *numbered* ioctl UAPI (JM 11.x / CSF 1.x) and needs `MEM_EXEC_INIT`, different GPU-props encoding, different `base_jd_atom_v2`; this kernel dispatches on in-buffer ids and ignores ioctl numbers. §3 |
| Can a minimal EGL/GLES target this kbase revision? | **Only by writing a new userspace KMD + GL driver against UK 10.6.** The kernel interface is sufficient in principle (proved by the client in this directory), but no existing GL userspace implements it; the Midgard compiler in Mesa is reusable, the driver layer is not. §4 |
| Would an ARM Linux Midgard DDK bind it? | **Yes, if it is the r15p0/UK 10.6 build** — including an armhf build, because `CONFIG_COMPAT=y` and the 32-bit AAPCS layout matches (verified). It is closed, not present in this session, not redistributable, and needs a 32-bit glibc userspace. An r7p0 DDK would not bind (same break as the Android blob). §5 |
| Delivered code | Wire-accurate UK 10.6 client library + bring-up/soft-job self-test + ABI extractors: `kbase_uapi.h`, `kbase.c`, `kbase-selftest.c`, `tools/kbase-layout.c`, `tools/verify-abi.sh`, `tools/verify-abi32.sh`. §6 |

---

## 1. The kbase UAPI that is actually built

Selection chain (established in `parity/reports/gpu.md`):
`CONFIG_DDK_VERSION_OS="n"` → `drivers/gpu/arm/Kconfig:24-25` sources `t7xx/`;
`CONFIG_MALI_T7XX=y` (`config-nura-j7elte:2288`), `CONFIG_MALI_R15P0=y` (`:2290`);
release string from `t7xx/r15p0/Kbuild:18` `MALI_RELEASE_NAME ?= "r15p0-00rel0"`;
`CONFIG_MALI_SYSTRACE_SUPPORT=y` (`:2291`); `CONFIG_COMPAT=y` (`:419`).

### 1.1 Transport: one ioctl, dispatch by buffer size

`mali_kbase_core_linux.c:1342-1365`:

```c
static long kbase_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    u64 msg[(CALL_MAX_SIZE + 7) >> 3] = { 0xdeadbeefdeadbeefull };
    u32 size = _IOC_SIZE(cmd);
    ...
    if (size > CALL_MAX_SIZE) return -ENOTTY;
    copy_from_user(&msg, arg, size);
    if (kbase_dispatch(kctx, &msg, size) != 0) return -EFAULT;
    copy_to_user(arg, &msg, size);
```

* `CALL_MAX_SIZE = KBASE_FUNC_MAX - 1` (`:1340`) = 568; the ioctl *number*,
  type and direction bits are **never examined** — only `_IOC_SIZE(cmd)`.
  So "ioctl numbers" in the usual Linux sense do not exist on this interface;
  the function id is `((union uk_header *)args)->id`
  (`mali_uk.h:100-109`, `mali_uk.h:85` `UK_FUNC_ID = 512`).
* `kbase_dispatch()` (`:279-1047`) rejects an unknown id with `-EINVAL`
  (`:1040-1043`), which the ioctl wrapper reports as `-1`/`EFAULT`
  (`:1356-1357`). Errors carry no errno granularity.

State machine (`:279-352`):
1. `UKP_FUNC_ID_CHECK_VERSION` (id 0, `mali_uk.h:78`, struct
   `uku_version_check_args`, 16 B). `kbase_api_handshake()` (`:170-224`) answers
   major 10 with `minor = min(6, userspace_minor)`; it also has compatibility
   cases for UK 6/7/8/9 if those `BASE_LEGACY_UK*` options were built.
2. Every other id before the handshake → `-EINVAL` (`:314-315`).
3. The **first** post-handshake call must be `KBASE_FUNC_SET_FLAGS` (512+18,
   `kbase_uk_set_flags`, 16 B); anything else leaves setup incomplete (`:317-351`).
4. Afterwards the `switch (id)` accepts the function table below.

### 1.2 Function ids (r15p0, `mali_kbase_uku.h:479-565`)

`MEM_ALLOC` 512, `MEM_IMPORT` 513, `MEM_COMMIT` 514, `MEM_QUERY` 515,
`MEM_FREE` 516, `MEM_FLAGS_CHANGE` 517, `MEM_ALIAS` 518, `SYNC` 520,
`POST_TERM` 521, `HWCNT_SETUP/DUMP/CLEAR` 522-524, `GPU_PROPS_REG_DUMP` 526,
`FIND_CPU_OFFSET` 527, `GET_VERSION` 528, `SET_FLAGS` 530,
`FENCE_VALIDATE` 535, `STREAM_CREATE` 536, profiling controls 537-538,
`DEBUGFS_MEM_PROFILE_ADD` 539, `JOB_SUBMIT` 540, `DISJOINT_QUERY` 541,
`GET_CONTEXT_ID` 543, `TLSTREAM_ACQUIRE_V10_4` 544, `TLSTREAM_FLUSH` 547,
`HWCNT_READER_SETUP` 548, `SOFT_EVENT_UPDATE` 550, `MEM_JIT_INIT` 551,
`TLSTREAM_ACQUIRE` 552, Samsung `HWCNT_UTIL_SETUP` 553, `HWCNT_GPR_DUMP` 554,
`VSYNC_SKIP` 555, `CREATE_SURFACE` 556, `DESTROY_SURFACE` 557,
`SET_MIN_LOCK` 558, `UNSET_MIN_LOCK` 559, `TMU_SKIP` 560, secure-world 567-568,
`KBASE_FUNC_MAX` 569.

(`KBASE_FUNC_MAX` is 569 in both DDK revisions because both end at
`UK_FUNC_ID + 57`; the *names* at 553-560 differ — see §2.)

### 1.3 Wire structs (verified, `tools/kbase-layout.c`)

| struct | size | notes |
|---|---|---|
| `union uk_header` | 8 | `id`/`ret` u32 + u64 `sizer` forces 8-byte alignment in 32- and 64-bit clients |
| `uku_version_check_args` | 16 | header, u16 major, u16 minor, u8 pad[4] |
| `kbase_uk_set_flags` | 16 | `create_flags`, padding |
| `kbase_uk_get_ddk_version` | 80 | 64-byte version buffer, u32 size, u32 pad; kernel writes `"K:" MALI_RELEASE_NAME "(GPL)"` (`:111`, `:718-720`), i.e. `K:r15p0-00rel0(GPL)` |
| `kbase_uk_gpuprops` | 536 | header + **fixed** `struct mali_base_gpu_props` (528 B, `mali_base_kernel.h:1634-1646`) |
| `kbase_uk_mem_alloc` | 56 | va_pages, commit_pages, extent, flags (in/out), gpu_va (out) @40, va_alignment @48 |
| `kbase_uk_mem_free` | 16 | gpu_addr |
| `kbase_uk_mem_query` / `_commit` | 32 / 32 | queries: commit size 1, VA size 2, flags 3 |
| `kbase_uk_mem_flags_change` | 32 | gpu_va, flags, mask |
| `kbase_uk_job_submit` | **40** | header, `addr` @8, `nr_atoms` @16, `stride` @20, Samsung `gles_ctx_handle` @24, `frame_number` @28, `surfacep` @32 |
| `base_jd_atom_v2` | **64** | offsets: jc 0, udata 8, extres_list 24, nr_extres 32, `compat_core_req` 34, pre_dep[2] 36, atom_number 40, prio 41, device_nr 42, pad 43, **core_req 44**, gles_ctx_handle 48, frame_number 52, surfacep 56 |
| `base_jd_event_v2` | 24 | event_code 0, atom_number 4, udata 8 |
| `base_dump_cpu_gpu_counters` | 64 | system_time 0, cycle_counter 8, sec 16, usec 24, padding to a cache line |

The `base_jd_atom_v2` layout is the r15p0 one (`mali_base_kernel.h:811-838`):
UK ≥ 10.3 moved `core_req` from the 16-bit slot at 34 (kept as
`compat_core_req`) to a 32-bit field at 44, and Samsung's systrace fields are
**unconditional** in r15p0.

### 1.4 Memory model

* 64-bit clients: the MEM_ALLOC dispatcher forces `BASE_MEM_SAME_VA`
  (`mali_kbase_core_linux.c:369-374`; the analogous import path does the same
  at `mali_kbase_mem_linux.c:1404-1406`). The returned `gpu_va` is not a GPU
  address but a **cookie**: `gpu_va = (__ffs(cookies) + PFN_DOWN(BASE_MEM_COOKIE_BASE)) << PAGE_SHIFT`
  (`mali_kbase_mem_linux.c:1313-1327`).
* Userspace must then `mmap(fd, pages*4096, PROT_READ|PROT_WRITE, MAP_SHARED, cookie)`
  (UK 10.5 "mmap in user space", `mali_kbase_uku.h:61-63`). `kbase_mmap()`
  accepts only shared mappings (`mali_kbase_mem_linux.c:2140-2146`), routes the
  cookie through `kbasep_reg_mmap()` (`:2058-2124`), and the GPU VA becomes
  `vma->vm_start` (`:2100`). So **CPU address == GPU address** after mapping.
* **The memory tracking page must be mapped first.** `kbase_mmap()` rejects
  every non-tracking mapping while `kctx->process_mm` is NULL
  (`mali_kbase_mem_linux.c:2160-2168`), and only the mapping at
  `vm_pgoff == PFN_DOWN(BASE_MEM_MAP_TRACKING_HANDLE)` (= 3, i.e. offset 0x3000)
  calls `kbase_tracking_page_setup()`, which installs `process_mm`
  (`:2512-2536`). The delivered client does this right after `SET_FLAGS`;
  closing that VMA drains `process_mm`, so it must outlive every BO.
* `MEM_FREE` on a SAME_VA region returns `-EINVAL` with a warning
  (`mali_kbase_mem.c:1243-1249`): SAME_VA memory is freed by `munmap`.
  A still-unmapped cookie *can* be freed through `MEM_FREE` (the cookie branch,
  `mali_kbase_mem.c:1216-1231`).
* VA size: `VA_BITS` is fixed at 39 in this 3.10 arm64 tree
  (`arch/arm64/Kconfig:91`), and the SAME_VA zone covers `[1, 2^27-1]` pages
  (`mali_kbase_mem.c:474-507`), i.e. the whole 39-bit user address space, so a
  plain `mmap(NULL, …)` lands inside the zone.
* `MEM_JIT_INIT` (551) exists; JIT allocations themselves are created through
  soft jobs (`BASE_JD_REQ_SOFT_JIT_ALLOC`).

### 1.5 Job submission and completion

* `KBASE_FUNC_JOB_SUBMIT` requires `sizeof(kbase_uk_job_submit) == args_size`
  (`mali_kbase_core_linux.c:553-567`) and then
  `submit_data->stride == sizeof(base_jd_atom_v2)`
  (`mali_kbase_jd.c:1343-1350`). Atoms are copied one stride at a time from
  `addr` (`:1412-1416`); `atom_number` indexes `jctx->atoms[256]`
  (`BASE_JD_ATOM_COUNT`, `mali_base_kernel.h:69`).
* Completion is delivered through `read()` on the same fd: blocking unless
  `O_NONBLOCK`, one or more 24-byte `base_jd_event_v2`
  (`mali_kbase_core_linux.c:1366-1402`), `DRV_TERMINATED` as `-EPIPE`.
* Soft jobs need no GPU command stream. `BASE_JD_REQ_SOFT_DUMP_CPU_GPU_TIME`
  (0x201) takes `atom->jc` as the GPU address of a 64-byte result buffer and
  writes timestamps back through `kbase_vmap_prot(..., KBASE_REG_GPU_WR)`
  (`mali_kbase_softjobs.c:131-199`). This is the cheapest end-to-end exercise
  of the job + event path and is what the delivered self-test uses.
* Context lifecycle: a kbase context is created by `open()` and destroyed on
  `close()`; `GET_CONTEXT_ID` returns its id. There is no explicit
  create/destroy ioctl in this generation.

---

## 2. The ROM blob expects r7p0 / UK 10.0 — and the wire formats differ

The ROM's `libGLES_mali.so` is `Midgard-"r7p0-03rel0"` (see
`parity/reports/gpu.md` §P4/§7). The matching UAPI source is in-tree at
`t72x/r7p0/`: `BASE_UK_VERSION_MINOR 0` (`mali_kbase_uku.h:48`).

Measured differences (`tools/verify-abi.sh` prints both tables):

| item | r7p0 (ROM blob) | r15p0 (Nura kernel) | consequence |
|---|---|---|---|
| UK version | 10.0 | 10.6 | handshake passes; kernel answers `10.0` (`min(6,0)`) |
| `kbase_uk_job_submit` | **32 B** (`mali_kbase_uku.h:105-120`) | **40 B** (`:127-141`) | `kbase_dispatch` takes `bad_size` → ioctl `EFAULT` before any job is parsed |
| `base_jd_atom_v2` | **48 B**, or 56 B with `CONFIG_MALI_SYSTRACE_SUPPORT` (`mali_base_kernel.h:625-642`) | **64 B** | even with the submit size fixed, `stride != 64` → `-EINVAL` (`mali_kbase_jd.c:1343-1350`) |
| `core_req` | u16 at offset 34 (`base_jd_core_req` is `u16`, `:363`) | u32 at offset 44 | job requirements would be read from the wrong field |
| `kbase_uk_mem_alloc` / `_free` / `_query` / `_commit` | 56 / 16 / 32 / 32 | same | memory allocation still works |
| `GET_VERSION`, `SET_FLAGS`, `GET_CONTEXT_ID` | 80 / 16 / 16 | same | work |
| vendor ids | `HWCNT_UTIL_SETUP`=+37, `GPR_DUMP`=+38, `VSYNC_SKIP`=+39, `CREATE_SURFACE`=+40, `DESTROY_SURFACE`=+41, `SET_MIN_LOCK`=+42, `UNSET_MIN_LOCK`=+43, `TMU_SKIP`=+44 (`t72x/r7p0/mali_kbase_uku.h:470-478`) | +41 … +48 (`mali_kbase_uku.h:553-560`), because `SOFT_EVENT_UPDATE`/`MEM_JIT_INIT`/`TLSTREAM_ACQUIRE` were inserted at +38/+39/+40 | every vendor call the blob makes is routed to a different handler (e.g. r7p0 `VSYNC_SKIP` 551 hits r15p0 `MEM_JIT_INIT` 551) |
| `KBASE_FUNC_EXT_BUFFER_LOCK` | +17 (`:433`) | removed in UK 10.4 | r7p0-only call → `-EINVAL` |
| `TLSTREAM_ACQUIRE` | +32, no flags (`mali_kbase_uku.h`, guarded by `MALI_KTLSTREAM_ENABLED`) | +32 is `…_V10_4` (no flags), +40 is the 10.6 variant with `flags` | a mis-sized call is rejected; not fatal by itself |

So the ROM userspace can open the Nura device, negotiate UK 10.0, set flags,
query version/props and allocate memory — then fails on its first
`KBASE_FUNC_JOB_SUBMIT` with `EFAULT`, and would additionally mis-route any
Samsung vendor call. This is a **hard, deterministic wire break**, not the
"unverified vendor-renumbering risk" that `gpu.md` §7 R1 could only hypothesise:
the submit-size check happens before any payload interpretation, so it does not
depend on what the stripped blob actually issues.

Consequences:

* Running the stock Android GL stack on the r15p0 kernel is impossible without
  either (a) building the r7p0 DDK instead (config switch documented in
  `gpu.md` §7 R1), or (b) a kernel compat shim that accepts the 32-byte r7p0
  submit and translates the r7p0 atom layout. (b) is not in this task's scope
  and is irrelevant to a Linux userspace; it would only serve a bionic chroot.
* For Nura nothing changes: there is no Android userspace to feed the blob.

---

## 3. The experimental Mesa kbase backend speaks a different generation

Primary source: the fork's own design notes
(`https://raw.githubusercontent.com/leegao/mesa-funnymdzz/8dfd821085/CLAUDE.md`,
quoted) and a shipping modern UAPI header
(`rockchip-linux/kernel develop-6.1 include/uapi/gpu/arm/bifrost/mali_kbase_ioctl.h`
and `.../jm/mali_kbase_jm_ioctl.h`).

What that backend is:

* A fork of Mesa adding `src/panfrost/lib/kmod/kbase_kmod.c` to the
  `pan_kmod` vtable, so Panfrost Gallium / PanVK can run on `/dev/mali*`.
* Targets **kbase r32p0–r44p0**; CSF (arch ≥ 10) uses UAPI 1.x, JM (arch ≤ 9)
  uses UAPI **11.x**. It probes `VERSION_CHECK` at ioctl nr **52** (CSF) then
  nr **0** (JM).
* Requires `MEM_EXEC_INIT` (UAPI 11.13) for GPU-executable BOs, a key/size
  encoded GPU-props blob (`KBASE_IOCTL_GET_GPUPROPS`), SAME_VA cookies, the
  modern `base_jd_atom` v2/v3 and numbered `KBASE_IOCTL_JOB_SUBMIT`.
* GL submission is gated off upstream in that fork (`PAN_EXPERIMENTAL_KBASE_GL`),
  there is no dma-buf import/WSI, and submission is a synchronous CPU-polled
  model.

Concrete mismatches with r15p0 (all mechanical, all fatal):

1. **Dispatch model.** Modern: `_IOW/_IOWR(KBASE_IOCTL_TYPE=0x80, nr, struct)`
   with distinct numbers (`SET_FLAGS` 1, `JOB_SUBMIT` 2, `GET_GPUPROPS` 3,
   `MEM_ALLOC` 5, `MEM_FREE` 7, `GET_DDK_VERSION` 13, `MEM_JIT_INIT` 14,
   `MEM_SYNC` 15, …). Legacy r15p0: one ioctl, only the size is read, the id
   is the first u32 of the payload. A modern `VERSION_CHECK`
   (`_IOWR(0x80, 0, {u16 major; u16 minor})`, size 4) is therefore read as
   `id = (minor << 16) | major` = `0x0006000a`; that is not
   `UKP_FUNC_ID_CHECK_VERSION`, and with `api_version == 0` the kernel returns
   `-EINVAL` (`mali_kbase_core_linux.c:297-315`). The very first call fails.
2. **GPU properties.** Legacy returns the fixed 528-byte
   `mali_base_gpu_props` struct; modern returns a TLV key/size blob.
3. **Executable memory.** `MEM_EXEC_INIT` / EXEC_VA zone do not exist in
   r15p0 (`grep -r 'MEM_EXEC_INIT\|EXEC_VA' t7xx/r15p0` = 0 hits); the modern
   backend relies on it for shader BOs.
4. **Atom format.** Modern `base_jd_atom_v2` is a different struct in UAPI
   11.x (11.20 replaced `compat_core_req` with `jit_id[2]`, 11.22 added atom
   v3, 11.46 removed `renderpass_id`); r15p0's is the 64-byte layout in §1.3.
5. **Kernel feature set.** No KCPU queues, no modern JIT/`MEM_SYNC` semantics,
   no `KBASE_IOCTL_GET_CPU_GPU_TIMEINFO`, no numbered HWC interface. The
   fork's CS/tiler-heap code assumes a 2018+ firmware-driven design; T720 is
   Midgard (arch 4) with job-manager-style submission.
6. **No WSI.** Even on a matching kernel the fork has no dma-buf import; on
   Nura there is no DRM/GBM at all (`gpu-userspace.md` §2/§5).

Adapting it would not be "build the fork with a flag": it would mean writing a
second `pan_kmod` backend for legacy UK 10.6, re-deriving batch/atom submission
(including Midgard job-chain and tiler-heap descriptors) and a WSI path. That
is a new GPU driver, not a configuration change, and it cannot be validated
without j7elte hardware.

---

## 4. Can a *minimal* EGL/GLES target this kbase revision?

Kernel side: **yes, the interface is sufficient and now has a working
userspace client** (this directory) for the hard parts that are common to any
driver: handshake, SAME_VA memory + mmap, atom submission with the exact
stride/field layout, completion events, soft jobs. The delivered library is
the layer any GL driver would sit on.

Userspace side: **no existing implementation speaks UK 10.6**, and a minimal
EGL/GLES still needs the parts that are GPU-specific:

1. **Midgard ISA + job descriptors.** Vertex/tiler/fragment job chains, MFBD
   descriptors, tiler heap, framebuffer/colour-buffer descriptors for T720
   (Midgard v4). The only open implementation is Mesa's `panfrost` compiler
   (`src/panfrost/compiler` + the Midgard pieces), which is separable in
   principle but is used today only through Panfrost's DRM-based device/batch
   abstraction.
2. **KMD/batch layer.** Memory pools for heap/command buffers (legacy
   `MEM_ALLOC` + `extent`/`MEM_COMMIT`), atom dependency graph (`SYNC` /
   `soft_event`), atom numbers and event handling, cache maintenance. This is
   the layer the Mesa fork rewrote for a *different* kbase generation (§3).
3. **EGL platform/WSI.** Nura has no DRM (`# CONFIG_DRM is not set`,
   `config-nura-j7elte:2262`); display is fbdev (`CONFIG_EXYNOS_DECON_FB=y`).
   Offscreen/surfaceless EGL could be built; on-screen EGL would need an fbdev
   (or compositor) platform and a presentation path, which is a separate gap
   (`parity/reports/display.md`).

So the honest statement is: **a minimal EGL/GLES for this kbase revision is
implementable in principle, but it does not exist and is not a port of
anything available.** The two shortest routes are (a) an r15p0 ARM Linux DDK
blob (§5) or (b) a new legacy-UK KMD backend under Mesa's Midgard compiler.
Neither is deliverable from static source in this session; no runtime claim is
made.

One negative result worth recording: because the kernel ignores ioctl numbers,
a "translation shim" (LD_PRELOAD over `ioctl`) between Mesa's modern backend
and this kernel is theoretically possible — every call would have to be
re-encoded into an in-buffer id + legacy struct. But the *semantic* gaps
(EXEC_VA, props blob, atom format, submission model) remain, so such a shim
would be a driver in its own right, not a compatibility wrapper.

---

## 5. ARM Linux Midgard DDK artifact assessment

What would be needed:

* **Version:** an r15p0 user-space build (`MALI_RELEASE_NAME r15p0-*`) compiled
  against UK **10.6** — the same `mali_kbase_uku.h` layout as §1.3. An r7p0
  Linux DDK would hit exactly the §2 submit-size/stride break.
* **ABI:** the historical Linux Midgard DDK deliveries are armhf/glibc
  (32-bit). That is *not* a blocker for this kernel: `CONFIG_COMPAT=y`
  (`config-nura-j7elte:419`), `kbase_ioctl` is shared with `.compat_ioctl`
  (`mali_kbase_core_linux.c:1665-1666`), and pointers travel as the 8-byte
  `union kbase_pointer` with `.compat_value` read for compat contexts
  (`mali_kbase_jd.c:66-74`). The 32-bit AAPCS layout of the r15p0 structs was
  checked by compiling the kernel headers for `armv7a-linux-gnueabihf`
  (`tools/verify-abi32.sh`): atom = 64, submit = 40, mem_alloc = 56,
  `core_req` @44, `surfacep` @56 — identical to the 64-bit kernel's
  expectations, so `stride` and every `sizeof` check agree.
* **Kernel feature agreement:** nothing Samsung-specific is required; the
  vendor ids (553-560) are extra entry points, not replacements. The DDK does
  expect the standard set built here (`MALI_MIDGARD`, `MALI_DVFS`, JIT, …).

Could it bind this kbase? **At the UAPI level, yes** — the legacy UK interface
is exactly the interface such a DDK was built for, and this report's client
demonstrates that the kernel side accepts that protocol shape. Practical
blockers:

* It is **closed source**, NDA-only, and not redistributable with
  postmarketOS. It is **not present in this session**: no `libmali*`, no DDK
  tarball, no `mali` userspace object outside the Android blob
  (`find … -iname '*libmali*' -o -iname '*mali*ddk*'` → nothing; the ROM has
  only `/vendor/lib/egl/libGLES_mali.so`, bionic/ARM32).
* Nura is aarch64 **musl**; an armhf/glibc blob needs a 32-bit glibc multilib
  runtime (or a chroot) plus its fbdev/X11 EGL platform, and would have to be
  wired into the Nura display stack.
* An aarch64 r15p0 Linux DDK would avoid the 32-bit runtime, but is equally
  unavailable.
* No such artifact can be fabricated here; inventing a `libmali` would be a
  fake driver, which this report explicitly does not do.

Ranking of realistic routes to hardware 3D on this device is unchanged from
`gpu-userspace.md`: mainline SoC + panfrost is the only *open* route, and it is
a mainlining project, not a userspace gap.

---

## 6. Delivered code

`parity/userspace/gpu/kbase/`:

| file | purpose |
|---|---|
| `kbase_uapi.h` | Self-contained wire definitions for UK 10.6 (structs, ids, flags, event codes) with `_Static_assert` size/offset checks matching the kernel headers. |
| `kbase.h`, `kbase.c` | Client library: `kbase_open_dev` (VERSION_CHECK → SET_FLAGS → map the 0x3000 tracking page → GET_VERSION → GET_CONTEXT_ID), `kbase_get_props`, `kbase_alloc` (MEM_ALLOC + SAME_VA cookie mmap, GPU VA = CPU VA), `kbase_free`, `kbase_submit_atoms`, `kbase_read_event`, `kbase_soft_dump_time`. No libc beyond syscalls. |
| `kbase-selftest.c` | Bring-up test; `--print-sizes`, `--probe-legacy` (sends the 32-byte r7p0 submit and reports the rejection), `--probe-modern` (sends a numbered ioctl and reports the rejection). |
| `tools/kbase-layout.c` | Prints the wire layout the **kernel's own** headers compile to, for both `t7xx/r15p0` and `t72x/r7p0`. |
| `tools/verify-abi.sh` | Builds the extractor against both revisions and diffs the r15p0 table against `kbase_uapi.h`. |
| `tools/verify-abi32.sh` | Compiles the r15p0 headers for armv7a (freestanding, clang) and asserts the 32-bit AAPCS layout. |
| `Makefile` | aarch64 static build (default `aarch64-linux-gnu-gcc`, overridable), `check-abi`, `check-abi32`. |

Verification performed here (host only):

* `make check-abi` → `ABI MATCH: kbase_uapi.h == r15p0 kernel layout`
  (13 sizes + 6 offsets compared against the kernel headers).
* `make check-abi32` → 32-bit ARM layout asserts pass (atom 64, submit 40).
* Cross-build with both `/usr/bin/aarch64-linux-gnu-gcc` and the session's
  `toolchains/gcc-arm-10.3-…aarch64-none-linux-gnu-gcc`: static aarch64 ELF.
* `qemu-aarch64 ./kbase-selftest` → `ABI self-check passed, bring-up not
  executed`, exit 2 (no `/dev/mali0` on the host); `--print-sizes` exits 0.

Not verified (no hardware): the actual handshake, MMA/alloc/mmap, soft-job
completion and event read on j7elte. Expected on-device output is documented
in the self-test text; the code paths were derived line-by-line from the
kernel sources cited above.

---

## 7. Residual risks

* **R1 — r7p0 systrace width.** The ROM kernel's `CONFIG_MALI_SYSTRACE_SUPPORT`
  is not recoverable from the artifacts here (no IKCONFIG; the image's
  disassembly could not be tied to the symbol table cleanly). The r7p0 atom is
  48 B without it and 56 B with it; both differ from the r15p0 64 B, so the
  conclusion does not depend on which one the ROM used. The submit-struct size
  break (32 vs 40) is independent of this option.
* **R2 — blob behaviour on failure.** That the stock blob aborts cleanly on
  `EFAULT` from `JOB_SUBMIT` is inferred from the kernel path, not observed.
  No runtime test of the blob was possible (bionic ARM32, no Android userspace
  in Nura, no hardware).
* **R3 — DDK availability statement.** The closed/armhf/glibc characterisation
  of the ARM Linux Midgard DDK is from the task statement and general
  knowledge of ARM's delivery model; no artifact exists in this session to
  inspect. The *binding* analysis (version/ABI/layout) is what was verified.
* **R4 — Mesa fork details** are taken from its own `CLAUDE.md` at the pinned
  commit and from shipping UAPI headers; the fork was not built or run here
  (no Android kernel/device, and its target is Bifrost/Valhall).
