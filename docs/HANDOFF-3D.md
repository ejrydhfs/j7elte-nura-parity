# Hardware 3D continuation handoff — Mesa on kbase

Status: 2026-10-25. Target: **Nura 3.10 kernel with kbase** (UK 10.6 / r15p0),
Mali-T720 MP2 on Exynos 7580 (`j7elte`).

Chosen path: **Mesa Gallium Panfrost with a kbase winsys**.
Not Panfrost/DRM (mainline-only; 3.10 lacks DRM and mainline has no Exynos7580).
Not the Android blob/libhybris path (archived; blocked by 32-bit bionic and the
r7p0/r15p0 UAPI split).

## 1. Artifacts and exact state

| Path | State |
|---|---|
| `parity/userspace/gpu/kbase-winsys/` | Standalone winsys, 1,258-line `kbase_winsys.c`; API covers device/screen/context/BO/fence/submit/soft-dump. Builds host and aarch64. |
| `parity/userspace/gpu/kbase/` | ABI-verified kbase UAPI client (`kbase.c`, `kbase_uapi.h`, `kbase-selftest`). Use this as the KMD reference. |
| `parity/userspace/gpu/mesa/src/` | Mesa 27 checkout, branch `kbase-winsys`; commits `35d495f` (kmod backend), `6167ac8` (backend-neutral JM submit), `4d56405` (screen/context skeleton), base `02f1337`. |
| `parity/patches/gpu-mesa-kbase/` | `0001` kmod backend (1,871 lines), `0002` JM submit (55), `0003` skeleton (148), `apply.sh`. Apply with `./apply.sh /path/to/mesa`. |
| `parity/reports/gpu-mesa-kbase.md` | Full milestone report, integration map and remaining-work list. |
| `parity/reports/gpu-kbase.md`, `gpu-re.md`, `mali-blob-re.md` | UAPI mapping, blob evidence/limits, estimator inputs. |

Static facts from the last audit:

- No `/dev/mali0` on this host; **no GPU path has been executed**.
- `build-kbase` is configured with `egl/gles1/gles2/gbm/glx = disabled`,
  `opengl=false`, `vulkan-drivers=[]`, `llvm=disabled`, `mesa-clc=system`.
- `build-kbase/src/gallium/drivers/panfrost/libpanfrost.a` is a ~5 KB stub,
  not a full driver; the full `libgallium_dri`/EGL has **not** been linked.
- `libpan` (Panfrost OpenCL) is pulled in by `with_gallium_panfrost` and needs
  `mesa_clc`. `mesa_clc` is missing; LLVM 20 exists at `/usr/lib/llvm20` but
  there is no `llvm-config` on PATH.
- Winsys `make check-abi` passes; the no-device selftest currently returns 1
  (treat as "no /dev/mali0"; the Makefile `check` treats 2 as expected).
- Mesa patches apply to main (`git am --3way`); skeleton object compiles;
  `kbase-kmod-selftest` links and rejects `/dev/null`.

## 2. Definition of done

**Code complete (shortest visible path):** a full aarch64 Mesa build links
`libgallium_dri`, `libEGL`, `libGLESv2`; Panfrost Gallium uses the kbase winsys;
synchronous submit and event wait work in code; EGL surfaceless works; an fbdev
WSI can present by CPU-mapping a linear BO and blitting to `/dev/fb0`; unit
tests and a mock kbase device cover the ioctl sequences. No hardware claim.

**Not included initially:** dma-buf export/import, sync_file interop,
Wayland/GBM, compositor-quality fences. Those need kernel work (kbase has no
dma-buf fd import; fences are Android sync fences) and are the 6–10 week path.

## 3. Blockers in priority order

1. **Build/link blocker.** `libpan`/`mesa_clc`. Two options:
   - build `mesa_clc` from the Mesa tree with LLVM (needs `llvm-config`), or
   - add a local Mesa patch that skips `subdir('libpan')` for the GL-only build
     (it is OpenCL-only; the GL path never calls it).
   Then enable `-Degl=enabled -Dgles2=enabled` and build the full driver.
2. **Winsys/sync.** Complete event→fence mapping, BO/VM lifetime, coherency
   (SAME_VA, `BASE_MEM_COHERENT_SYSTEM`, cache maintenance). Defer dma-buf.
3. **Renderer submission.** Wire Gallium Panfrost batch/chain, tiler heap,
   uniforms, framebuffer to `kbw_submit`; validate with a headless triangle.
4. **WSI.** `EGL_PLATFORM_SURFACELESS_MESA` first; then fbdev CPU blit.
5. **Hardware.** `/dev/mali0` required for real validation; no emulator.

## 4. Five-agent split (disjoint ownership)

| Agent | Owns | Deliverable / acceptance |
|---|---|---|
| A. `mesa-build` | `parity/patches/gpu-mesa-kbase/` (new 0004+), Mesa meson config | Full `libgallium_dri` + `libEGL`/`libGLESv2` link with EGL/GLES2 enabled; build log; `libpan` skipped or `mesa_clc` built. |
| B. `kbase-winsys-sync` | `parity/userspace/gpu/kbase-winsys/` | Event→fence, BO/VM, coherency; `make check`/selftest; no hardware claim. |
| C. `midgard-submit` | Mesa `src/gallium/drivers/panfrost/{pan_jm,pan_job,pan_cmdstream,pan_screen}.c` | JM submit through the kbase backend; headless triangle path; compile/link. |
| D. `wsi-fbdev` | Mesa `src/egl/`, new `platform_kbase_fbdev` | Surfaceless EGL, then fbdev blit; `eglinfo`/`es2_info` static/documentation. |
| E. `integration-tests` | `parity/userspace/gpu/tests/`, `check-state.sh`, mock kbase | Mock `/dev/mali0` (LD_PRELOAD/ioctl mock), unit tests, CI script; no source overlap. |

One integration owner merges Mesa branches; agents must not edit the same Mesa
files concurrently. Use `git worktree` for separate Mesa checkouts.

## 5. Commands

Winsys:

```
cd parity/userspace/gpu/kbase-winsys
make clean all
make check-abi
./tests/kbw-selftest --print-abi
./tests/kbw-selftest          # no device: expects non-zero
make aarch64
```

Mesa:

```
cd parity/userspace/gpu/mesa/src
# patches already applied on branch kbase-winsys; reapply with:
#   ../../../../patches/gpu-mesa-kbase/apply.sh .
meson setup build-kbase -Dgallium-drivers=panfrost -Dvulkan-drivers= \
  -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
  -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dpanfrost-rust=false \
  -Dbuild-tests=true
ninja -C build-kbase \
  src/panfrost/lib/kbase-kmod-selftest \
  src/gallium/drivers/panfrost/libpanfrost.a
```

For the full driver, change to `-Degl=enabled -Dgles2=enabled`, resolve
`libpan`/`mesa_clc`, and build `src/gallium/targets/dri` (or the EGL/GLES
targets).

## 6. Hardware test plan (when a phone is available)

1. Boot the Nura kernel with kbase; confirm `/dev/mali0` and no kbase errors in
   `dmesg`.
2. Run `parity/userspace/gpu/kbase/kbase-selftest` (handshake/alloc/soft job).
3. Run `kbw-selftest` with no args (handshake, SAME_VA, event).
4. `eglinfo -B`, `es2_info` with surfaceless; then the fbdev WSI test.
5. `glmark2-es2 --off-screen` (if packaged); capture dmesg on any hang.
6. Suspend/resume, multi-context, forced job fault (if the kernel supports it).

## 7. Do not

- Do not switch to Panfrost/DRM: that is mainline-only and not the Nura target.
- Do not enable both the vendor fbdev display stack and a DRM display stack.
- Do not claim GL/rendering without the hardware test.
- Do not add dma-buf/sync_file dependencies to the first milestone; they are
  kernel-limited and not needed for the shortest visible path.
