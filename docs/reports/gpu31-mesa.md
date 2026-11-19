# GPU31 Mesa — pan_kmod to v5.2 ioctls, slot fix, selectable kbase backend, aarch64 recipe

Date: 2026-11-19. Slice: G31_Mesa3. Target: `patches/gpu-mesa-kbase/`
(read-only) + `code/userspace/gpu/mesa-render/` (read-only) +
`code/gpu31/mesa/` (NEW) + `docs/reports/gpu31-mesa.md` (this file).
No shared-tree edits; deliverables are patches under `code/gpu31/mesa/`;
only the Integrator builds/flashes (M1-gated).

Related: `docs/reports/gpu31-target.md` (v5.2 H1 lock),
`docs/reports/gpu31-drmcore.md` (ioctl rows), `docs/reports/gpu-verify.md`
(live T720 capture), `code/userspace/gpu/mesa-render/ADAPT.md` (H1/H2),
`patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff` (H2 hunk),
`code/gpu31/gem/panfrost_drm.h` (byte-identical v5.2 mirror).

## 0. Decision (unchanged): H1 is the path

The render node speaks the v5.2 panfrost UAPI
(`0x00 SUBMIT / 0x01 WAIT_BO / 0x02 CREATE_BO / 0x03 MMAP_BO /
0x04 GET_PARAM / 0x05 GET_BO_OFFSET`; `GET_PARAM` = `GPU_PROD_ID` only;
`SUBMIT` ends at `requirements`; `CREATE_BO.flags` reserved-zero).
Then the kbase backend does NOT apply at runtime — stock
`panfrost_dri.so` owns the fd. The kbase backend stays in-tree and
selectable (0006), and the H2 shim adapt patch stays valid, but nothing
in this slice changes the H1 runtime.

## 1. v5.2 ioctl table (drmcore, verified)

From `refs/tags/v5.2` `panfrost_drv.c:340-351` (fetched 2026-11-19):

| ioctl | # | flags | v5.2 semantics |
|---|---|---|---|
| SUBMIT | 0x00 | RENDER_ALLOW \| AUTH | `!jc` -> -EINVAL; `requirements && != FS` -> -EINVAL; syncobj in/out |
| WAIT_BO | 0x01 | RENDER_ALLOW | resv wait, `pad` must be 0 |
| CREATE_BO | 0x02 | RENDER_ALLOW | rejects `!size`, `flags`, `pad`; returns handle + GPU offset |
| MMAP_BO | 0x03 | RENDER_ALLOW | `flags` must be 0; returns mmap offset |
| GET_PARAM | 0x04 | RENDER_ALLOW | `pad` must be 0; answers GPU_PROD_ID only, else -EINVAL |
| GET_BO_OFFSET | 0x05 | RENDER_ALLOW | per-fd GPU offset |

Post-v5.2 surface (MADVISE 0x08, SET_LABEL 0x09, JM_CTX_CREATE 0x0a,
JM_CTX_DESTROY 0x0b, SYNC_BO 0x0c, QUERY_BO_INFO 0x0d, PERFCNT 0x06/0x07,
40+ params, wider SUBMIT) does NOT exist on this kernel. `GET_BO_OFFSET`
and `WAIT_BO` already match; everything else is handled Mesa-side by 0005.

## 2. Slot proof (n_slots off-by-one, 0004)

`kbase_kmod_submit_job` (0001:1235) sets `n_slots = 1` (+1 iff in-fence)
but always emits job + trigger (2 atoms), or wait + job + trigger (3).
Every atom consumes one `slots[idx++]` entry: the unconditional
`job_slot`/`trig_slot` assignment overruns the reservation in BOTH cases
(no-fence: 1 alloc vs 2 used — `slots[1]` OOB read, cleanup leaks one busy
bit; in-fence: 2 vs 3). Fix: `n_slots = 2 /* job + trigger */` (+1 with
in-fence). Proof model in `code/gpu31/mesa/slot-proof/slot-proof.c`:
alloc == use in both cases (2==2, 3==3); old count under-allocates
(1<2, 2<3). The sibling `mk_flags` write-only note is NOT part of this
patch (field + store exist only in an uncommitted side tree, never in the
repo's 0001).

## 3. v5.2 compat (0005)

`src/panfrost/lib/kmod/panfrost_kmod_v52_compat.h` (new, static-inline,
no new exports) + two call-site swaps in `panfrost_kmod.c`:

- GET_PARAM: only GPU_PROD_ID hits the kernel; everything else answered
  from the live T720 capture (gpu-verify.md: GPU_ID 0x07201000, shader
  0x3/MP2, tiler 1, l2 1, mem 1, mmu 0x2821/VA33, l2 0x07110206, tiler
  0x809, thread 0x0a040400, js 0x7, coherency NONE, MEDIUM-only prios,
  timestamp 0). Post-v5.2 params fail with -EINVAL like the kernel, so
  each caller's `required=false` default path runs.
- SUBMIT: strips the 8-byte post-v5.2 tail (jm_ctx_handle/pad never sent;
  v5.2 struct is 40 bytes), enforces the v5.2 requirement gate (FS or
  nothing; CYCLE_COUNT tolerated with a warning, dropped on the floor).
- Selftest `panfrost_kmod_v52_selftest.c` wired to `meson test v52-compat`
  (GPU_ID decode, 40-vs-48 wire gate, requirement gate, no 0x08+ ioctl
  references). Full evidence + blocked items: `code/gpu31/mesa/build.log`.

Deliberately NOT shimmed: JM_CTX_CREATE/DESTROY (T720 uses the default
context, handle 0 — 0002 never issues the ioctl), CREATE_BO flags (kernel
rejects nonzero), perfcnt/timestamp/label/madvise/sync paths (no-op or
absent-is-absent semantics documented in the patch message).

## 4. kbase backend stays selectable (0006, no hunks)

`pan_kmod.c` table order is panfrost, panthor, kbase (kbase LAST): the DRM
backend adopts render nodes first; kbase only sees refused fds (its
dev_create rejects drmGetVersion fds, modulo the H2 adapt patch).
`PAN_KMOD_RESTRICT_TO_BACKENDS` (`"all"` default): `"panfrost"` forces H1,
`"kbase"` forces /dev/mali0, `"all"` tries in order. 0005 touches only
panfrost_kmod.c call sites, so selection is untouched.

## 5. aarch64 build recipe (panfrost enabled)

`code/gpu31/mesa/aarch64-panfrost-build.sh`: fetch pinned Mesa 02f13375,
`git apply --check` 0001-0005, `git am` 0001-0003, `git apply` 0004-0005,
meson cross (`--cross-file` aarch64, `panfrost,softpipe`, all GL/VK
disabled except the Gallium drivers, LLVM off, rust off, tests on),
ninja both selftests, qemu-aarch64 runs v52-compat. llvmpipe is excluded
from the cross build (needs LLVM); softpipe is the LLVM-free GL fallback.
Known host blocker: current Mesa forces CLC when panfrost is enabled ->
needs LLVMSPIRVLib (spirv-llvm-translator, not installed, no root); the
recipe documents it (see build.log §3).

## 6. eglinfo/glxinfo + hardware expectations

Host (build machine, Mesa 26.2.6): `eglinfo -B` EGL 1.5; `glxinfo -B`
radeonsi; `LIBGL_ALWAYS_SOFTWARE=1 glxinfo -B` ->
`llvmpipe (LLVM 23.1.4, 256 bits)`, GL 4.6 compat. llvmpipe works; it is
the pre-hardware GL baseline (device runs owned by G31_MesaTests).

Hardware expectations (T720 on the v5.2 node): T720 is Midgard arch 4
(`pan_arch`: prod 0x720 -> 4); Midgard uses the JM submit path (no CSF);
default JM context (handle 0, no JM_CTX_CREATE needed); 3 job slots;
GPU_ID 0x07201000. First hardware proof: `render-node-probe` classifies H1
on `/dev/dri/renderD128`, then the glmark harness
(`code/gpu31/mesatests/run-mesatests.sh`) must show renderer != llvmpipe
and GPU utilisation > 0 (currently 0 with llvmpipe — gpu-verify.md §3).

## 7. Files delivered

| File | Role |
|---|---|
| `code/gpu31/mesa/patches/0004-kbase-slot-count-fix.patch` | n_slots 2 (job+trigger); `git apply --check` PASS |
| `code/gpu31/mesa/patches/0005-panfrost-v52-compat.patch` | v52 GET_PARAM/SUBMIT shim + selftest + meson; `git am` PASS in scratch |
| `code/gpu31/mesa/patches/0006-kbase-backend-selectable.patch` | selection contract lock (no hunks) |
| `code/gpu31/mesa/aarch64-panfrost-build.sh` | cross recipe (panfrost enabled) |
| `code/gpu31/mesa/slot-proof/slot-proof.c` | slot proof model |
| `code/gpu31/mesa/build.log` | configure/selftest/GL evidence + blockers |
| `docs/reports/gpu31-mesa.md` | this file |

No collision: G31_MesaWsiHw owns `code/gpu31/mesa/wsi-knobs.sh` only
(ack'd over IRC); no other `mesa/` names from this slice overlap it.

## 8. Verification status (honest)

- PASS: v5.2 header byte-identity vs GEM mirror; 0004/0005 check-apply +
  scratch `git am`; eglinfo/glxinfo llvmpipe on host.
- BLOCKED (host, not patch): meson configure needs LLVMSPIRVLib (no root
  to install); selftest compiles need a healthy toolchain (host gcc
  rejects <stdio.h>). Both re-runnable verbatim from build.log §3-4.
- NOT DONE (gated): device render-node-probe classify, hardware GL run,
  kernel Image build/flash — Integrator / M1-verify own those.
