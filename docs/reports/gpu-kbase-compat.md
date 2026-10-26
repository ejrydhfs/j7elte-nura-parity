# GPU kbase r7p0 UAPI compatibility shim — state report

**Status: implementation stopped by decision.** Main chose the Mesa kbase backend
path for 3D instead of running the ROM's Android blob, so the r7p0/UK 10.0
translation shim was not wired into `kbase_dispatch()` and no kernel build was
run for it. The kernel tree is unmodified (`git status --porcelain` empty).

What is left behind is the self-contained reference header with the verified
wire layouts and translation helpers:
`parity/patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h`.
It is not included by any compiled file and has no effect on the build.

## 1. Confirmed mismatches (source-verified)

Source: `android_kernel_samsung_universal7580-2a746c1…`, paths relative to
`drivers/gpu/arm/`.

| item | r7p0 / UK 10.0 (`t72x/r7p0`) | r15p0 / UK 10.6 (`t7xx/r15p0`) | source |
|---|---|---|---|
| `struct kbase_uk_job_submit` | **32 B**: header@0, addr@8, nr_atoms@16, stride@20, gles_ctx_handle@24 | **40 B**: adds frame_number@28, surfacep@32 | `mali_kbase_uku.h:105-120` vs `:127-141` |
| `base_jd_atom_v2` | **48 B**, or **56 B** with `CONFIG_MALI_SYSTRACE_SUPPORT` (default config has it =y) | **64 B** | `mali_base_kernel.h:625-642` vs `:811-838` |
| `core_req` | `base_jd_core_req` = `u16` @34 | `u32` @44 (UK 10.3); u16 slot @34 kept as `compat_core_req` | `:363,630` vs `:450,823` |
| `pre_dep[2]` | @36, 2×2 B | @36, 2×2 B (same) | — |
| `gles_ctx_handle` | atom @48 with systrace; submit @24 | atom @48 | — |
| vendor ids | `UK_FUNC_ID+37…+44` (HWCNT_UTIL_SETUP…TMU_SKIP) | `UK_FUNC_ID+41…+48`; +38/+39/+40 became SOFT_EVENT_UPDATE / MEM_JIT_INIT / TLSTREAM_ACQUIRE | `mali_kbase_uku.h:470-478` vs `:553-560` |
| `EXT_BUFFER_LOCK` | `UK_FUNC_ID+17` | removed (UK 10.4) | `:433` |
| memory/version/set-flags/context-id | identical sizes and layouts | identical | — |

Consequence (unchanged): the blob handshakes as 10.0 (kernel answers
`min(6,0)`), sets flags, allocates memory, then its first `JOB_SUBMIT` (32 B)
is rejected by `kbase_dispatch()`'s `sizeof(*job) != args_size` check with
`-EINVAL` → ioctl `EFAULT`, before any atom is parsed. Even with the size
accepted, `kbase_jd_submit()` rejects `stride != 64`.

The shim would have been gated on the negotiated `kctx->api_version ==
KBASE_API_VERSION(10, 0)`, so the native r15p0 path (10.6) is untouched.

## 2. Translation design captured in the artifact header

`mali_kbase_uku_r7p0_compat.h` contains:

* `struct kbase_uk_job_submit_r7p0` (32 B) and `struct base_jd_atom_v2_r7p0`
  (56 B, core_req@34, pre_dep@36, gles@48) with compile-time size/offset
  checks via negative-array typedefs.
* `kbase_r7p0_job_submit_translate()`: validates `stride ∈ {48,56}` and
  `nr_atoms <= BASE_JD_ATOM_COUNT`, fills the 40-byte r15p0 submit, zeroes
  `frame_number`/`surfacep`, keeps the source stride for the user-array walk.
* `kbase_jd_atom_v2_from_r7p0()`: field-for-field expansion to the 64-byte
  atom; sets both `compat_core_req` and `core_req` so the UK<10.3 branch in
  `kbase_jd_submit()` (`BASE_LEGACY_UK10_2_SUPPORT` is unconditionally
  defined in this tree) reproduces the original value; per-atom gles handle
  used for the 56-byte variant, submit-level handle for the 48-byte variant.
* `kbase_r7p0_soft_job_translatable()`: allows soft types 1–4 only. r7p0
  `SOFT_DVFS` (type 5) collides with r15p0 `SOFT_EVENT_WAIT` and has no
  equivalent, so it must be rejected rather than misinterpreted.

The intended wiring (not applied) was: in `kbase_dispatch()` remap ids
549–556 → +4 and reject 529 for a 10.0 context, add a 32-byte branch to
`KBASE_FUNC_JOB_SUBMIT`, and give `kbase_jd_submit()` an extra
`r7p0_stride` argument (0 = native) that copies `stride` bytes and calls
`kbase_jd_atom_v2_from_r7p0()` before the existing per-atom path. Elements
that were verified safe: `base_external_resource` is a single `u64` in both
revisions; `base_dump_cpu_gpu_counters` is 64 B in both; vendor payload
structs (`hwcnt_setup`, `hwcnt_gpr_dump`, `vsync_skip`, `tmu_skip`,
`custom_command`) are identical, so the id remap needs no payload rewrite.

Not validated: no compile of the header against the kernel, no build, no
runtime (no `/dev/mali0`, no hardware).

## 3. Remaining blockers for ever running the Android blob

1. **Android userspace runtime.** `libGLES_mali.so` is an ARM32 bionic
   library; Nura is musl/glibc aarch64. It needs a 32-bit bionic rootfs and a
   `libhybris`-style bridge for `libc`, `libcutils`, `libhardware`,
   `libsync`, ion/gralloc and the binder-facing EGL platform. Nothing of that
   exists in this tree.
2. **Display/WSI.** Nura has no DRM (`# CONFIG_DRM is not set`) and the blob's
   EGL platform expects Android gralloc/framebuffer semantics; an fbdev shim
   would still be required for on-screen rendering.
3. **Config divergence.** The default build config
   (`parity/pmaports/.../config-samsung-j7elte.aarch64`, line 2299) has
   `# CONFIG_MALI_SEC_HWCNT is not set`. Even with the id remap, r7p0
   HWCNT_UTIL_SETUP / HWCNT_GPR_DUMP / VSYNC_SKIP would land on ids that
   `kbase_dispatch()` does not enumerate in this configuration and be
   rejected; only TMU_SKIP / CREATE_SURFACE / DESTROY_SURFACE / SET_MIN_LOCK /
   UNSET_MIN_LOCK have handlers.
4. **No runtime evidence.** No submission is claimed to work; nothing was
   executed against the GPU.
