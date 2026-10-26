# GPU path 3 — kbase userspace feasibility (Mali-T720, Exynos 7580, j7elte)

Scope: map the kbase UAPI that Nura actually builds, compare it with (a) the ROM
blob's expected UAPI and (b) the modern Mesa kbase backend, and decide whether
any minimal EGL/GLES can target this kbase revision. Deliverable code lives in
`parity/userspace/gpu/kbase/`; the detailed reference is
`parity/userspace/gpu/kbase/analysis.md`.

Static analysis + cross-compilation only. **No j7elte hardware and no
`/dev/mali0`**: the bring-up path was compiled and smoke-tested under
`qemu-aarch64` on the host, but has not been executed against the GPU.

**Verdict**

| Question | Answer |
|---|---|
| UAPI built | Legacy ARM **UK 10.6**, DDK **r15p0-00rel0** (`t7xx/r15p0`). One ioctl; only `_IOC_SIZE(cmd)` is used, the function id is a u32 inside the argument buffer. |
| ROM blob (`r7p0` / UK 10.0) | **Cannot drive this kernel.** `kbase_uk_job_submit` is 32 B for r7p0, 40 B for r15p0 → `kbase_dispatch()` rejects every submission with `bad_size` (`EFAULT`). Even with that fixed, the atom stride is 48/56 vs 64, and `core_req` sits at 34 (u16) vs 44 (u32). |
| Mesa experimental kbase backend | **Cannot drive this kernel.** It speaks the 2017+ numbered-ioctl UAPI (JM 11.x / CSF 1.x), needs `MEM_EXEC_INIT`/EXEC_VA and a TLV GPU-props blob; this kernel dispatches on in-buffer ids. Its first `VERSION_CHECK` is read as id `0x0006000a` and rejected. |
| Minimal EGL/GLES for this kbase? | **Not from any existing code.** In principle a new userspace KMD against UK 10.6 is possible (this report delivers the memory/job/event half); the Midgard compiler exists in Mesa, but the driver/batch/WSI layer does not. |
| ARM Linux Midgard DDK | **Would bind if it is the r15p0/UK 10.6 build**, armhf included (`CONFIG_COMPAT=y`; 32-bit AAPCS layout verified identical). But it is closed, absent from this session, non-redistributable, and needs a 32-bit glibc runtime. An r7p0 DDK would not bind. |
| Code delivered | UK 10.6 wire header + client library + bring-up/soft-job self-test + two ABI extractors (`abi-match` and `abi32` both pass). |
| Kernel/userspace patches | **None.** No defect in the Nura kernel was found; the only lever for blob compatibility is the r7p0 build/config switch already documented in `gpu.md` §7 R1 (shared config, not this task's ownership). |

---

## 1. What the kernel actually exposes

`CONFIG_MALI_T7XX=y` + `CONFIG_MALI_R15P0=y` (`config-nura-j7elte:2288,2290`);
release string `r15p0-00rel0` (`t7xx/r15p0/Kbuild:18`); UK version 10.6
(`mali_kbase_uku.h:69-70`).

### 1.1 Transport

`kbase_ioctl()` (`mali_kbase_core_linux.c:1342-1365`):

* `size = _IOC_SIZE(cmd)`; `cmd`'s type/number/direction are **ignored**.
* `size > CALL_MAX_SIZE` (= `KBASE_FUNC_MAX - 1` = 568, `:1340`) → `-ENOTTY`.
* the whole buffer is copied in/out; `kbase_dispatch()` (`:279`) reads
  `((union uk_header *)args)->id` and returns `-EINVAL` on unknown ids, which
  surfaces as `-1`/`EFAULT` (no errno detail).
* Order is enforced: `UKP_FUNC_ID_CHECK_VERSION` (id 0) first
  (`kbase_api_handshake()`, `:170-219`, answers `10.min(6, peer_minor)`), then
  `KBASE_FUNC_SET_FLAGS` (512+18) before anything else (`:317-351`).
* Device node is a misc device named `mali0` (`:3873`, `:4001`, `:4006`);
  `read()` returns 24-byte `base_jd_event_v2` records (`:1366-1402`).

### 1.2 Function ids

`UK_FUNC_ID = 512` (`mali_uk.h:85`). Key ids
(`mali_kbase_uku.h:479-565`): `MEM_ALLOC` 512, `MEM_FREE` 516, `MEM_COMMIT`
514, `MEM_QUERY` 515, `SYNC` 520, `GPU_PROPS_REG_DUMP` 526, `GET_VERSION` 528,
`SET_FLAGS` 530, `JOB_SUBMIT` 540, `GET_CONTEXT_ID` 543, `SOFT_EVENT_UPDATE`
550, `MEM_JIT_INIT` 551, `TLSTREAM_ACQUIRE` 552, Samsung vendor block 553-560,
secure-world 567-568, `KBASE_FUNC_MAX` 569.

### 1.3 Wire layout (measured from the kernel headers)

| struct | size | key offsets |
|---|---|---|
| `uku_version_check_args` | 16 | major 8, minor 10 |
| `kbase_uk_set_flags` | 16 | create_flags 8 |
| `kbase_uk_get_ddk_version` | 80 | buffer 8, size 72 |
| `kbase_uk_gpuprops` | 536 | fixed 528-byte `mali_base_gpu_props` |
| `kbase_uk_mem_alloc` | 56 | flags 32, gpu_va 40 |
| `kbase_uk_mem_free` / `_query` / `_commit` | 16 / 32 / 32 | — |
| `kbase_uk_job_submit` | **40** | addr 8, nr_atoms 16, stride 20, gles 24, frame 28, surfacep 32 |
| `base_jd_atom_v2` | **64** | jc 0, udata 8, extres 24, nr_extres 32, compat_core_req 34, pre_dep 36, atom_number 40, prio 41, device_nr 42, **core_req 44**, gles 48, frame 52, surfacep 56 |
| `base_jd_event_v2` | 24 | event_code 0, atom_number 4, udata 8 |
| `base_dump_cpu_gpu_counters` | 64 | system_time 0, cycle_counter 8, sec 16, usec 24 |

### 1.4 Memory / job / context

* 64-bit clients get `BASE_MEM_SAME_VA` forced (`mali_kbase_core_linux.c:369-374`).
  `MEM_ALLOC` returns a **cookie** `(idx + 64) << 12`
  (`mali_kbase_mem_linux.c:1313-1327`); userspace must `mmap(fd, pages, RW,
  MAP_SHARED, cookie)` and the GPU VA becomes `vma->vm_start`
  (`kbasep_reg_mmap()`, `:2058-2124`) — CPU VA == GPU VA.
* **The tracking page must be mapped first**: every other mmap fails with
  `-EINVAL` while `kctx->process_mm` is NULL (`:2160-2168`); only
  `vm_pgoff == 3` (offset 0x3000) installs it (`kbase_tracking_page_setup()`,
  `:2512-2536`). The delivered client does this in `kbase_open_dev()`.
* SAME_VA memory is released by `munmap`; `MEM_FREE` on it returns `-EINVAL`
  (`mali_kbase_mem.c:1243-1249`). Unmapped cookies can be freed.
* `VA_BITS` is 39 (`arch/arm64/Kconfig:91`); the SAME_VA zone covers the whole
  user VA space (`mali_kbase_mem.c:474-507`), so `mmap(NULL, …)` is in range.
* `JOB_SUBMIT` requires `sizeof(kbase_uk_job_submit) == args_size`
  (`mali_kbase_core_linux.c:553-567`) and `stride == sizeof(base_jd_atom_v2)`
  (`mali_kbase_jd.c:1343-1350`); atoms are copied stride-wise
  (`:1412-1416`), `atom_number` indexes 256 slots.
* Context creation is implicit on `open()`, destruction on `close()`;
  `GET_CONTEXT_ID` reports the id.

---

## 2. The ROM blob expects r7p0 / UK 10.0 — hard wire break

ROM blob: `Midgard-"r7p0-03rel0"` (see `gpu.md` §P4). Matching source:
`t72x/r7p0/` (`BASE_UK_VERSION_MINOR 0`, `mali_kbase_uku.h:48`).

| item | r7p0 | r15p0 | result |
|---|---|---|---|
| `kbase_uk_job_submit` | **32 B** (`t72x/r7p0/mali_kbase_uku.h:105-116`) | **40 B** (`t7xx/r15p0/…:127-141`) | `kbase_dispatch()` `bad_size` → ioctl `EFAULT` |
| `base_jd_atom_v2` | **48 B** (or 56 B with `CONFIG_MALI_SYSTRACE_SUPPORT`) (`mali_base_kernel.h:625-642`) | **64 B** | `stride` check fails even if the submit size were accepted |
| `core_req` | u16 @34 (`base_jd_core_req` is u16, `:363`) | u32 @44 | requirements read from the wrong field |
| vendor ids | `HWCNT_UTIL_SETUP` +37 … `TMU_SKIP` +44 (`:470-478`) | +41 … +48 (`:553-560`) | every Samsung call routed to a different handler |
| `EXT_BUFFER_LOCK` | +17 (`:433`) | removed (UK 10.4) | `-EINVAL` |
| `MEM_ALLOC/FREE/QUERY/COMMIT`, `GET_VERSION`, `SET_FLAGS`, `GET_CONTEXT_ID` | same sizes | same | memory/query work |

So the ROM userspace can handshake (kernel replies `10.0`), set flags, read
version/props and allocate memory, but the **first job submission fails
deterministically** before any payload interpretation — independent of what the
stripped blob does. This upgrades `gpu.md` §7 R1 from "renumbering risk" to a
proven ABI break; the only fixes are building the r7p0 DDK (config change,
`gpu.md` §7 R1) or a kernel compat shim for the r7p0 submit struct (not
relevant to a Linux userspace).

---

## 3. Mesa's experimental kbase backend speaks a different generation

Source: the fork's design notes
(`leegao/mesa-funnymdzz` `CLAUDE.md` @ 8dfd8210) and shipping modern UAPI
headers (`rockchip-linux/kernel develop-6.1 include/uapi/gpu/arm/bifrost/`).

* Targets kbase **r32p0–r44p0**; CSF arch ≥ 10 (UAPI 1.x) / JM arch ≤ 9
  (UAPI **11.x**); probes `VERSION_CHECK` at nr **52** then nr **0**.
* Numbered ioctls: `KBASE_IOCTL_TYPE 0x80`, `VERSION_CHECK` nr 0
  (`_IOWR`, payload `{u16 major; u16 minor}`, 4 bytes), `JOB_SUBMIT` nr 2,
  `GET_GPUPROPS` nr 3 (TLV key/size blob), `MEM_ALLOC` nr 5 (32-byte union),
  `MEM_EXEC_INIT` nr 36 (UAPI 11.13) for executable BOs, etc.
* This kernel ignores ioctl numbers and dispatches on the first u32 of the
  payload. A modern `VERSION_CHECK` therefore presents `id = 0x0006000a`,
  not 0, and is rejected before `api_version` is set
  (`mali_kbase_core_linux.c:297-315`). Even the handshake cannot succeed.
* Beyond dispatch: legacy returns a fixed 528-byte props struct (not TLV),
  has **no** `MEM_EXEC_INIT`/EXEC_VA/KCPU queues (`grep` = 0 hits in
  `t7xx/r15p0`), and a different `base_jd_atom_v2` (64 B here vs the 11.x
  format changed at 11.20/11.22/11.46).
* The fork itself gates GL submission off and has no dma-buf import/WSI, and
  it is written for CSF/Bifrost-Valhall, not Midgard.

Conclusion: not portable to this kernel by configuration. Adapting it means a
new `pan_kmod` backend for legacy UK 10.6 plus Midgard batch/atom submission and
a WSI path — a new driver, not a port.

---

## 4. Can a minimal EGL/GLES target this kbase revision?

Kernel side yes — and that half is now implemented here: handshake, SAME_VA
memory + tracking page + cookie mmap, exact 64-byte atom submission, event
delivery, soft jobs (see §6). Userspace side no: nothing existing speaks UK
10.6. A minimal GLES still needs (i) Midgard (v4) instruction/job descriptors —
only available in Mesa's panfrost compiler, which is tied to Panfrost's
DRM device/batch layer; (ii) a kbase batch/atom/memory layer for legacy UK;
(iii) an EGL platform, while Nura has **no DRM** (`# CONFIG_DRM is not set`,
`config-nura-j7elte:2262`) and only fbdev — offscreen EGL is conceivable,
on-screen presentation is a separate display gap.

No runtime claim: this is a feasibility finding, not a driver.

---

## 5. ARM Linux Midgard DDK artifact

What would be needed: an **r15p0** Linux DDK userspace built against **UK 10.6**
(same headers as §1.3). The historical armhf/glibc ABI is *not* a blocker:
`CONFIG_COMPAT=y` (`config-nura-j7elte:419`), `.compat_ioctl = kbase_ioctl`
(`mali_kbase_core_linux.c:1666`), and pointers use the 8-byte
`union kbase_pointer` (`.compat_value` for compat contexts, `mali_kbase_jd.c:66-74`).
Verified with clang `--target=armv7a-linux-gnueabihf` (`tools/verify-abi32.sh`):
atom 64, submit 40, mem_alloc 56, `core_req` @44, `surfacep` @56 — identical to
the 64-bit kernel's `sizeof`/`stride` expectations, so an armhf r15p0 DDK would
satisfy them.

Practical blockers: closed source / NDA; **not present in this session** (no
`libmali*`, no DDK tarball; the ROM carries only the Android bionic ARM32
`libGLES_mali.so`); non-redistributable; needs a 32-bit glibc rootfs or chroot
plus its fbdev/X11 EGL platform. An aarch64 r15p0 Linux DDK would avoid the
multilib issue but is equally unavailable. An r7p0 DDK would hit the §2 break.
No driver was fabricated.

---

## 6. Delivered artifacts and verification

`parity/userspace/gpu/kbase/`:

* `kbase_uapi.h` — self-contained UK 10.6 wire header with
  `_Static_assert` size/offset checks.
* `kbase.c` / `kbase.h` — client library: handshake, tracking page,
  props, MEM_ALLOC + cookie mmap, free, atom submit, event read, soft-job
  `DUMP_CPU_GPU_TIME` round trip.
* `kbase-selftest.c` — bring-up test plus `--print-sizes`, `--probe-legacy`
  (sends the r7p0 32-byte submit and reports the rejection on hardware),
  `--probe-modern` (sends a numbered ioctl and reports the rejection).
* `tools/kbase-layout.c`, `tools/verify-abi.sh` — compile the **kernel's own**
  r15p0 and r7p0 headers and diff the r15p0 table against the client header.
* `tools/verify-abi32.sh` — freestanding armv7a layout asserts for compat clients.
* `Makefile` — static aarch64 build (distro or session GCC 10.3), `check-abi`,
  `check-abi32`.

Observed here:

```
$ make check-abi
...
ABI MATCH: kbase_uapi.h == r15p0 kernel layout
$ make check-abi32
32-bit ARM (AAPCS) r15p0 layout: OK
  atom=64 job_submit=40 mem_alloc=56 event=24 core_req@44 surfacep@56
$ qemu-aarch64 ./kbase-selftest          # host has no /dev/mali0
kbase UK 10.6 userspace bring-up test
wire ABI: atom=64 submit=40 event=24 ...
device /dev/mali0 not present: ABI self-check passed, bring-up not executed
exit=2
```

Cross-compiles are static aarch64 ELF with both `/usr/bin/aarch64-linux-gnu-gcc`
and `toolchains/gcc-arm-10.3-…aarch64-none-linux-gnu-gcc`. The r15p0 vs r7p0
layout table printed by `check-abi` is the evidence for §2.

---

## 7. Residual risks

* **R1** The ROM kernel's `CONFIG_MALI_SYSTRACE_SUPPORT` is not recoverable from
  the artifacts (no IKCONFIG; disassembly could not be tied to the symbol
  table). r7p0 atom = 48 B or 56 B; both differ from 64, so §2's conclusion is
  unaffected. The submit-size break (32 vs 40) is independent of it.
* **R2** The on-device behaviour of `kbase-selftest` (handshake, alloc, soft-job
  completion, event) is derived line-by-line from kernel sources but was not
  executed against the GPU. Expected output is documented in the binary's text.
* **R3** The DDK characterisation (closed, armhf/glibc) comes from the task
  statement and ARM's delivery model; no artifact was available to inspect. The
  *binding* analysis (version/ABI/layout) is what was verified.
* **R4** Mesa fork details come from its own design notes and shipping UAPI
  headers; the fork was not built or run here.
