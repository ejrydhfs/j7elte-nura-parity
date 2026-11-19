# gpu31-shim — r7p0 stride fix + H2 render-node-over-kbase design

Date: 2026-11-19. Owner: G31_KbaseShim3. Ticket: review §11 (r7p0 compat
stride) + optional DRM render-node-over-kbase design; winsys targeting.

Deliverables (this commit):

| Path | What it is |
|---|---|
| `patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h` (edited) | §11 F1 stride fix: stride 64 + per-atom expansion |
| `code/gpu31/shim/0001-drm-kbase-shim-render-node-over-kbase.patch` | H2 shim: optional render node backed by kbase UK 10.6 |
| `code/gpu31/shim/stride_test.c` + `Makefile` | host proof: translated batches pass the kernel's jd gate |
| `code/gpu31/shim/stub/` | host-test stubs (kernel headers stand-ins, test-only) |
| `code/userspace/gpu/kbase-winsys/{include,src,tests}` (3-line) | H2 path note + `KBW_DEFAULT_DEVICE` (no behavior change) |
| this report `docs/reports/gpu31-shim.md` | design, verification, review disposition |

No kernel-tree edits (read-only tree honored). Only the Integrator
builds/applies; only Main gates flash. Live-device state per Main:
phone UP on kernel #37 (M1 DRM core built, fbdev X 720x1280,
`/dev/mali0` present, **no /dev/dri** — no driver bound).

Relation to sibling slices (no overlap, verified by grep + broadcasts):
G31_Gem (panfrost_drm.h / GEM — acknowledged, no conflict),
G31_Devfreq (landed d8438056, no conflict), G31_FaultTests (no conflict).
G31_DriverAssemble2 owns the H1 panfrost path
(`code/gpu31/driver/*`, v5.2 panfrost UAPI on renderD128); this slice
owns ONLY the H2 kbase-UK fallback and the r7p0 stride fix. The two
must never both bind the render minor (either/or at Integrator time).

## 1. §11 F1 stride fix (REQUIRED, done)

Review finding (`gpu31-review.md` §11 F1): `kbase_r7p0_job_submit_translate`
preserved the 48/56-byte source stride, which the r15p0 kernel ALWAYS
rejects — `mali_kbase_jd.c:1343-1352`:

```c
if (submit_data->stride != sizeof(base_jd_atom_v2)) {
        dev_err(kbdev->dev, "Stride passed to job_submit doesn't match kernel");
        return -EINVAL;
}
```

`sizeof(base_jd_atom_v2)` is 64 unconditionally in this tree
(`BASE_LEGACY_UK6_SUPPORT` is compiled in per `mali_base_kernel.h:33`,
but the r15p0 code has NO `BASE_LEGACY_UK10_2`-era 48/56 stride arm —
only the UK6 32-byte-compat branch, which does not apply to r7p0 atoms).
The old comment ("Keep the source stride: kbase_jd_submit() walks the
user array") was wrong and is gone.

Fix (in `patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h`):

1. `kbase_r7p0_job_submit_translate()` now sets
   `dst->stride = sizeof(struct base_jd_atom_v2)` (64) and documents
   that the caller MUST expand every source atom and point `dst->addr`
   at the 64-byte array. `dst->addr` is deliberately left zero in the
   helper: copying `src->addr` would invite submitting the r7p0 array
   at stride 64, which the kernel walks atom-by-atom and would read
   out of bounds. The dispatch-site glue shape is specified in §2.
2. `kbase_jd_atom_v2_from_r7p0()` now returns `bool` and validates
   before writing: stride ∈ {48, 56} (else false), and refuses the
   untranslatable r7p0 SOFT_DVFS soft job (type 5, `SOFT_JOB|0x5` —
   verified `t72x/r7p0/mali_base_kernel.h:460`; in r15p0 type 5 is
   `SOFT_EVENT_WAIT`, `:575` — a silent misinterpretation if passed
   through). Types 1..4 match in both revisions (verified same files)
   and pass. All reads use raw-offset `memcpy` bounded by the validated
   stride (never a struct store past 48 bytes for the no-systrace form —
   the host `-Warray-bounds` build caught the naive version).
3. New `kbase_r7p0_atom_batch_translate()`: walks the source `@stride`
   bytes at a time, expands to 64-byte atoms, fails the WHOLE batch on
   the first bad atom (never a half-translated submit), `nr_atoms == 0`
   is a no-op success, `nr_atoms > 256` (`BASE_JD_ATOM_COUNT`,
   `mali_base_kernel.h:67`) refused.

Wire facts re-verified against the tree for this fix (not trusted from
the old report): r7p0 submit 32 B (`mali_kbase_uku.h:105-116`) vs r15p0
40 B (`:127-141`); r7p0 atom 48 B, 56 B with `CONFIG_MALI_SYSTRACE_SUPPORT`
(which the Nura `.config:2319` has `=y`, and the ROM blob was built with —
default `y` per `t72x/r7p0/Kconfig:51-58`); `core_req` u16@34 → u32@44
(r15p0 keeps the u16 slot as `compat_core_req`, and
`BASE_LEGACY_UK10_2_SUPPORT 1` at `mali_base_kernel.h:46` makes
`kbase_jd_submit()` copy `compat_core_req & 0x7fff` over `core_req` for
10.0 contexts — which is why the shim sets BOTH); vendor block shift
+37..+44 → +41..+48 (r7p0 `:470-478` runs `HWCNT_UTIL_SETUP`…`TMU_SKIP`
consecutively from +37 since `HWCNT_READER_SETUP=+36`; r15p0 `:549-557`
starts the same run at +41 because +38/+39/+40 became `SOFT_EVENT_UPDATE`
/ `MEM_JIT_INIT` / `TLSTREAM_ACQUIRE` — the review's "UNVERIFIED" note
on the shift is now closed: t72x source read, ids confirmed); the
`NO_MALI` guard that would drop the vendor block is off
(`.config:2308` `# CONFIG_MALI_NO_MALI is not set`).

Out of scope (unchanged, documented): vendor-id remap is NOT wired
into any dispatch (no kernel build in this slice); `EXT_BUFFER_LOCK`
(+17, removed in UK 10.4) stays refused; r7p0 `SOFT_DVFS` stays refused.

## 2. Host proof: stride_test (acceptance: proven)

`code/gpu31/shim/stride_test.c` (`make check`: native + aarch64/qemu —
**PASS 9/9 groups both**):

- sizes 32/56/40/64; header emits stride 64 and passes the emulated
  `sizeof(*job) != args_size → bad_size` + `stride != 64 → -EINVAL`
  gates; the OLD 48/56/32 strides are shown rejected by the same gate.
- 56-byte atoms expand field-for-field (jc/udata/extres/nr_extres/
  compat+core_req/pre_dep/atom/prio/device/gles-own); 48-byte atoms
  inherit the submit-level gles handle; undefined r15p0 fields
  (frame_number/surfacep) stay zero.
- batch 2x56 → 2x64 preserves order; DVFS batch/atom refused;
  bad stride / 257 atoms / soft types 1-4 covered.

The test compiles the REAL compat header (staged copy) against stub
kernel headers carrying the kernel-measured offsets; static asserts in
the header (32/48/56/64 sizes, core_req@34, pre_dep@36) run in the same
build. `make check` output is the evidence; no device needed.

## 3. H2 design: render node backed by kbase UK 10.6 (optional, non-invasive)

Goal (from the ticket + `gpu-drm.md` M2 + `mesa-render/ADAPT.md` H2):
Mesa uses the EXISTING kbase winsys/backend (UK 10.6, stride-64,
SAME_VA, fences) while opening a DRM render-node name
(`/dev/dri/renderD128`) instead of `/dev/mali0` — no panfrost UAPI
needed, no winsys rewrite (one-hunk Mesa adapt already shipped as
`patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff`).

Why a driver, not a flag on exynos: kbase keeps per-open GPU context
state in the file (`filp->private_data = kctx`: `kbase_open:1224`,
`kbase_release:1291`, `kbase_ioctl:1346`, `kbase_mmap:2128`); a DRM
open installs a `drm_file` instead and routes ioctls through the DRM
dispatcher (fixed-size struct copies, unknown numbers rejected before
driver code runs). The kbase ioctl needs the opposite: one ioctl number
carrying a variable-size buffer whose first u32 is the function id
(`kbase_dispatch`, `kbase_core_linux.c:283+`). Grafting that onto the
exynos driver would replace its open/ioctl/mmap dispatch for every
client including the display path. A separate driver keeps the display
path byte-identical and removes with one Kconfig switch.

Patch `code/gpu31/shim/0001-drm-kbase-shim-render-node-over-kbase.patch`
(5 files, +402; new files only + 2 one-line wiring hunks):

- `drivers/gpu/drm/kbase-shim/kbase_shim.c` (+407): platform driver
  creating an in-code device (`platform_device_register_simple`, matches
  NO DT compatible, steals no DECON/DSIM, no clocks/power/IRQ); one
  `DRM_MINOR_RENDER` minor (existing 128..383 carve-out,
  `drm_stub.c:110-128`); `drm_fill_in_dev` for file-list + GEM init
  (no `load()`, no MODESET → no control/legacy minors, no CRTC).
- Open creates BOTH states (DRM `drm_file` via `drm_open`, then kbase
  context via `kbase_create_context` exactly as `kbase_open` does,
  stored in `priv->driver_priv`); every kbase op (ioctl/read/poll/mmap)
  swaps `filp->private_data` to the kctx for the call and restores the
  `drm_file` after (kbase reads ONLY the kctx from private_data — no
  DRM state touched, no new locking, per-fd serialization as on
  `/dev/mali0`). Release destroys the kctx then runs stock
  `drm_release()`. The first design (calling `kbase_open()` inside the
  DRM open hook) was caught in self-review — it clobbers private_data
  and orphans the drm_file — and replaced with this swap model.
- ioctl: table holds ONLY `DRM_IOCTL_VERSION` (node identification for
  probe/libdrm); the fops wrapper serves VERSION via `drm_ioctl()`,
  refuses KMS numbers (`< DRM_COMMAND_BASE`), forwards everything else
  to `kbase_ioctl()` (in-buffer UK dispatch, `_IOC_SIZE`-sized copies).
- mmap: forward to `kbase_mmap()` (SAME_VA cookies `0x40000+n*0x1000`
  vs GEM fake offsets `>= 0x100000` — disjoint; driver creates no GEM
  objects so no `drm_mmap_locked` lookup can hit).
- Winsys side (`kbase-winsys.h/.c`, selftest): 3-line change — H2 path
  note on `kbw_device_open()` + `KBW_DEFAULT_DEVICE` macro used by the
  two default-path spots. No behavior change: handshake still decides
  (real DRM answers the UK query with ENOTTY → refused).

Verification (host-side; Integrator builds, M1Verify asserts on device):
`patch -p1 --dry-run` clean; `gcc -fsyntax-only` clean on the patched C
against the slice stubs (host stand-ins for the verified 3.10 shapes —
context line numbers recorded in the patch message); on-device asserts:
`renderD128` present, `render-node-probe` → H2, winsys selftest full
path vs the node, `SETCRTC` from the node → `-EINVAL`.

## 4. Review disposition + what remains

- §11 F1 (REQUIRED stride-64 + per-atom expansion): FIXED + proven
  (`stride_test` PASS native/qemu). The old "keep the source stride"
  comment is gone.
- §11 vendor-shift note (UNVERIFIED): CLOSED for the header's purposes
  (t72x source read; +37..+44 → +41..+48 confirmed; `NO_MALI` off in
  Nura config). Remap still not wired into dispatch (needs the
  Integrator kernel build; r7p0 blob path, not the Mesa path).
- §15 adapt diff (PASS, H2 Mesa hunk): unchanged; this slice's shim is
  its kernel-side companion.
- Mesa `n_slots` (§12 F1): G31_Mesa's scope, untouched here.
- Integrator/M1Verify gates (not this slice): apply order backport
  0001-0005 → KMS Mode-B → shim (or H1 driver — either/or, never both)
  → M1/D4 fragments; build `CONFIG_DRM_KBASE_SHIM` off by default;
  device asserts in §3.

## 5. Files changed (this commit)

- `patches/gpu-kbase-compat/mali_kbase_uku_r7p0_compat.h` — stride fix
  (+127/-23: bool returns, DVFS refusal, batch translator, 64-stride
  header, addr-zero contract, bounded raw copies).
- `code/gpu31/shim/` — H2 patch, stride test + Makefile, stubs.
- `code/userspace/gpu/kbase-winsys/` — header note + macro (3 lines),
  no logic change (`make check`: ABI + no-device path PASS on host).
- this report.
