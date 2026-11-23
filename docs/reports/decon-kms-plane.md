# DECON KMS planes (`exynos_drm7580_plane.c`)

Slice: **DeconPlane** (plan `docs/reports/decon-kms-plan.md` §6 item 11).
Source: `code/gpu31/kms/exynos_drm7580_plane.c`;
patch: `patches/drm-kms/0001-plane-primary-overlays-and-formats.patch`.
Kernel citations are `path:line` in
`android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
(read-only), abbreviated below as `decon_core.c`, `decon.h`, `regs-decon.h`
under `drivers/video/exynos/decon_7580/`, plus
`arch/arm64/boot/dts/exynos7580.dtsi`.

Status: **implemented and compiled**; not run on the phone (no KMS node yet —
this slice is one part of M1). The only shared interfaces are
`code/gpu31/kms/exynos_drm7580.h` and `code/gpu31/kms/decon_api.h`; the header
was not edited.

## 1. Plane model

The board DT exposes three DECON windows (`max_win = <3>`,
`exynos7580.dtsi:1145`; `default_win = <0>`, `:1148`), while the DECON block
itself has seven (`MAX_DECON_WIN`, `decon.h:45`).  The slice registers
`EXYNOS7580_MAX_PLANES` = 3 planes, so a missing window is reported to
userspace as `-ENOSPC` (§5) instead of silently changing the plane list.

| plane | DRM type | DECON window | IDMA channel | `possible_crtcs` | formats |
|---|---|---|---|---|---|
| 0 | `PRIMARY` | 0 | `DECON_KMS_IDMA_G0` | `BIT(0)` | ARGB8888, XRGB8888, RGB565 |
| 1 | `OVERLAY` | 1 | `DECON_KMS_IDMA_G1` | `BIT(0)` | ARGB8888, XRGB8888, RGB565 |
| 2 | `OVERLAY` | 2 | `DECON_KMS_IDMA_VG0` | `BIT(0)` | the above + NV12, NV21 |

**Window 0 keeps G0.**  The bootloader and the vendor driver program window 0
from `IDMA_G0` (`decon_core.c:4697`) and `decon_api.h` documents that window 0
must keep the channel it was left on.

**Channels must be distinct.**  `decon_reg_chmap_validate()`
(`decon_core.c:1886-1900`) silently disables a second window that reuses an
IDMA channel, so the fixed assignment above picks three different ones.
`decon_reg_config_win_channel()` (`decon_reg_7580.c:322-346`) encodes
`IDMA_G0`/`IDMA_G1` as `WINCHMAP` values 1/2 and the VG family as 0; the VG0
choice is the only YUV-capable channel of the three.

**The VG window is the YUV window.**  `WINCON_BPPMODE_NV21`/`NV12`
(`regs-decon.h:90-91`) exist, but the G0/G1 channels are RGB channels; the
slice therefore accepts NV12/NV21 only on plane 2 and returns `-EINVAL`
elsewhere.  If the vendor instance exposes fewer than three windows the plane
is still registered and `atomic_check()` returns `-ENOSPC` (§5).

`zpos` follows from the plane index (primary at the bottom, window number ==
plane number); the DECON has no per-window priority register beyond the window
index.  M0's `struct drm_plane_state` (`include/drm/drm_crtc.h`) has no `zpos`
or `alpha` member and the backport creates no matching properties, so neither
is exposed; this matches the KWin requirement matrix (row 14 is optional).

## 2. Format table (DRM fourcc → DECON)

The vendor driver has no fourcc: the shim owns
`fourcc → enum decon_pixel_format` (`decon_api.h` /
`docs/reports/decon-kms-api.md` §5).  This slice advertises and validates the
subset KWin needs, plus the two YUV420 formats the hardware supports.

| DRM fourcc | vendor `enum decon_pixel_format` | DECON bits (31..0) | `bpp` | `WINCON` mode | alpha | evidence |
|---|---|---|---|---|---|---|
| `ARGB8888` | `DECON_PIXEL_FORMAT_BGRA_8888` | A R G B | 32 | `WINCON_BPPMODE_ARGB8888` | 8 @24 | `decon_core.c:280-283,333-336,465-468,516-548`; `regs-decon.h:76,79` |
| `XRGB8888` | `DECON_PIXEL_FORMAT_BGRX_8888` | X R G B | 32 | `WINCON_BPPMODE_XRGB8888` | none | same tables, `regs-decon.h:80,83` |
| `RGB565` | `DECON_PIXEL_FORMAT_RGB_565` | R G B | 16 | `WINCON_BPPMODE_RGB565` | none | `decon_core.c:276-278,279-281,353-355`; `regs-decon.h:84` |
| `NV12` (plane 2 only) | `DECON_PIXEL_FORMAT_NV12` | Y + interleaved UV | 12 | `WINCON_BPPMODE_NV12` | none | `decon_core.c:257-258,1369-1409,175-208`; `regs-decon.h:91` |
| `NV21` (plane 2 only) | `DECON_PIXEL_FORMAT_NV21` | Y + interleaved VU | 12 | `WINCON_BPPMODE_NV21` | none | same, `regs-decon.h:90` |

How the mapping is derived: the vendor's per-component helpers give the bit
position of each channel (`decon_red_offset()` `:268-296`,
`decon_green_offset()` `:339-367`, `decon_blue_offset()` `:398-420`,
`decon_transp_length()`/`decon_transp_offset()` `:425-487`), and
`decon_rgborder()` (`:516-548`) turns the format into the `WINCON` BPP mode.
For example `DECON_PIXEL_FORMAT_BGRA_8888` has red at bit 16, green at 8, blue
at 0 and 8-bit alpha at 24 — i.e. `A[31:24] R[23:16] G[15:8] B[7:0]`, which is
`DRM_FORMAT_ARGB8888`.  The naming of the vendor enum is memory-order, not
DRM-order; the two must not be equated by name.

`decon_get_memory_plane_cnt()` (`decon_core.c:1369-1409`) returns -1 for
anything outside this set, and `wincon()` (`:175-208`) has no `WINCON` encoding
for other RGB depths, so nothing else is advertised.  The register layer adds
no further filtering: `decon_reg_set_regs_data()` writes `regs->wincon`
unchanged into `WINCON(win_idx)` (`decon_reg_7580.c:529-566`), so the format is
entirely decided by the `WINCON_BPPMODE_*` values in `regs-decon.h:76-91`
(`NV21` = `0x18 << 2`, `NV12` = `0x19 << 2`, `RGB565` = `0x8 << 2`).

**YUV420 plumbing status (integration note).**  `NV12`/`NV21` are
semi-planar in the vendor engine: `decon_get_memory_plane_cnt()` returns 1 and
`decon_calc_plane_offset()` derives the chroma address as
`addr[0] + stride * height` (`decon_core.c:1411-1431`).  `decon_api.h` allows
`addr[1]` to be left zero for that case and the shim derives it the same way.
KmsDmabuf's `exynos7580_fb_create()` currently rejects two-plane fbs, so an
NV12/NV21 fb cannot be created yet and `exynos7580_fb_scanout(fb, 1, …)`
returns `-EINVAL`; the plane code already handles both outcomes (it uses the
returned plane-1 address when available and leaves `addr[1] == 0` otherwise),
so enabling YUV later is a KmsDmabuf-only change.

## 3. Limits enforced by `atomic_check()`

Every row is also enforced by the vendor window path, so a state accepted here
cannot be rejected later by `decon_kms_commit()` for these reasons.

| Limit | Value | Our check | Vendor evidence |
|---|---|---|---|
| window count | 3 on j7elte (hw 7) | `idx >= caps->max_win` ⇒ `-ENOSPC` | `exynos7580.dtsi:1145`; `decon.h:45` |
| scaling | none | `src.w == dst.w && src.h == dst.h` | `does_layer_need_scale()`, `decon_core.c:610-613,2257-2263` |
| dst position/size | `x,y >= 0`, `w,h != 0` | same | `decon_core.c:1544-1552` |
| min direct-colour width | `dst.w * bpp / 8 >= 128` bytes | same | `decon_core.c:1579-1586` |
| x alignment (RGB565) | x and x+w on a 2-pixel boundary (`32/bpp`) | same | `decon_validate_x_alignment()`, `:551-568,1596-1603` |
| source stride | `src.f_w >= dst.w`, `src.f_w >= src.x + src.w` | same | `decon_core.c:1588-1594` |
| source rows | `src.f_h >= src.y + src.h` | same | full-buffer coordinates |
| 16.16 source | integral pixels only | low 16 bits of `src_*` must be 0 | no scaling/cropping support (`:610-613`) |
| window 0 blending | must be `NONE` | forced; overlay `PREMULT` for ARGB, else `NONE` | `decon_core.c:1538-1542,1479-1514` |
| per-plane alpha | 255 (no plane alpha) | hardwired | `decon_set_alpha_blending()`, `:1479-1514` |
| channels | G0/G1/VG0, no sharing | fixed assignment | `decon_core.c:1886-1900`; `decon_reg_7580.c:322-346` |
| YUV window | plane 2 only | `fmt->yuv && idx != 2` ⇒ `-EINVAL` | `regs-decon.h:90-91` |
| block area | unused | zero-filled | `CONFIG_DECON_BLOCKING_MODE` off (`.config:2389`); otherwise computed by the vendor code (`decon_core.c:640-729`) |
| protected content | not exposed | `false` | no DRM property in M0 |

`atomic_check()` does **not** resolve DMA addresses: M0's property write path
runs it for `TEST_ONLY` commits, which must have no side effects.

## 4. Atomic state → `struct decon_kms_win`

`exynos7580_plane_state_to_win()` is the only translator; it is shared by
`atomic_check()` (validation only) and the commit gatherer (with addresses).

| DTO field | Source | Notes |
|---|---|---|
| `state` | fb && crtc present ⇒ `DECON_KMS_WIN_BUFFER`, else `DECON_KMS_WIN_DISABLED` | a window with fb/crtc NULL is programmed disabled |
| `fourcc` | `state->fb->pixel_format` | must be in §2 |
| `idma_type` | plane index | fixed table (§1) |
| `blending` | window 0 ⇒ `NONE`; ARGB overlay ⇒ `PREMULT`; else `NONE` | Wayland/KWin buffers are premultiplied |
| `plane_alpha` | 255 | no atomic alpha field in M0 |
| `src.x/y/w/h` | `src_x/y/w/h >> 16` | integral only |
| `src.f_w` | `fb->pitches[0] / bytes_per_pixel` | the DECON works in pixels; NV12 bpp 12 makes bytes == pixels (`decon_core.c:1670-1678`) |
| `src.f_h` | `fb->height` | full buffer stride/size |
| `dst.x/y/w/h` | `crtc_x/y/w/h` | no scaling, so `dst.w/h == src.w/h` |
| `dst.f_w/f_h` | mirror of `src.f_w/f_h` | only used by the vendor fb bookkeeping |
| `addr[0]` | `exynos7580_fb_scanout(fb, 0, &addr)` | falls back to no address ⇒ window disabled, commit continues |
| `addr[1]` | `exynos7580_fb_scanout(fb, 1, …)` if it succeeds, else 0 | 0 tells the shim to derive the NV12/NV21 chroma plane |
| `addr[2]` | — | always 0 |
| `block_area` | zero | blocking mode compiled out |
| `protection` | `false` | no property in M0 |

`exynos7580_fb_scanout()` (contract entry point added in `exynos_drm7580.h`,
implemented by KmsDmabuf) is the **only** address source.  It covers imported
ADDFB2 framebuffers (DECON IOVA from `iovmm_map`) and the CMA fbdev
framebuffer, so the plane never touches GEM/ION/CMA internals and no
`drm_fb_cma_get_gem_obj()->paddr` is used (a DMA-direct address, not an IOVMM
one).

## 5. Window shortage: `-ENOSPC`

If the vendor instance exposes fewer windows than the DT advertises
(`decon_kms_caps()->max_win` < plane index), `atomic_check()` returns
`-ENOSPC`.  KWin issues `TEST_ONLY` commits and drops a plane whose test fails
(`decon-kms-req.md` §2.2 row 9, §4.2 item 3), so the compositor degrades to the
primary plane instead of failing the whole modeset.  `-ENOSPC` is the same
error the generic plane checker returns for a source rectangle outside the
framebuffer (`drm_atomic.c`), so userspace already treats it as "this plane
cannot represent the buffer".

`planes_init()` still registers all three planes when `max_win >= 1`; only a
`max_win == 0` instance fails initialisation.

## 6. Commit protocol

There is no register access in this slice.  One atomic commit results in
exactly one `decon_kms_commit()`:

1. M0's `drm_atomic_helper_swap_state()` installs the new states in
   `plane->state`; `drm_atomic_helper_commit_planes()` then walks the planes of
   the changeset in ascending index and calls `atomic_update()` (or
   `atomic_disable()`) for each (`drm_atomic_helper.c:1128-1190`).
2. The **last** plane of the changeset gathers **all** committed plane states
   (`ex->planes[i].state`, including unchanged planes) into `wins[]` and calls
   `decon_kms_commit(ex->decon, wins, 3)`.  Earlier planes only validate.
3. The gatherer is skipped while the CRTC is inactive
   (`plane->crtc->state->active == 0`) or the plane's new state has no CRTC:
   the CRTC disable path stops the DECON and owns the final blanking.
4. The transitional plane ioctls (`drm_plane_helper_update/disable`, used by
   legacy `MODE_SETPLANE`) pass a state without an atomic state; the plane then
   commits immediately, which is correct because exactly one plane changed.

`decon_kms_commit()` is the vendor shim entry point (`decon_api.h`): it takes
`output_lock`, programs the windows, waits for vsync and the shadow update.
Its `-EAGAIN` (DECON not on) is logged; the atomic helper's plane callbacks
return `void`, so it cannot be propagated to userspace after the point of no
return.  Errors from `exynos7580_fb_scanout()` disable that one window and the
frame is still latched.

## 7. Build evidence

Build tree: a private copy of the read-only kernel
(`.../j7k/kms/DeconPlane`), M0 atomic series 0001-0004 applied cleanly, the
shared header copied to `drivers/gpu/drm/exynos/exynos_drm7580.h`, `decon_api.h`
to `include/linux/exynos_decon_kms.h`, and the object added to
`exynosdrm-y` **in that copy only** (not part of the patch):

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 \
     drivers/gpu/drm/
```

- exit status 0, **0 errors**;
- **10 warnings**, all pre-existing in untouched vendor files
  (`exynos_drm_gem.c` int/pointer casts, `exynos_drm_fb.h` header guard,
  `exynos_drm_dmabuf.c` `%x`/`dma_addr_t`, `exynos_drm_g2d.c` casts);
  **0 warnings in `exynos_drm7580_plane.c`**;
- `exynos_drm7580_plane.o` (189 416 bytes, 522 source lines) is linked into
  `drivers/gpu/drm/exynos/built-in.o` and exports
  `exynos7580_planes_init`, `exynos7580_plane_atomic_update`,
  `exynos7580_plane_atomic_disable`; the expected unresolved symbols are
  `decon_kms_caps`, `decon_kms_commit` (vendor shim) and
  `exynos7580_fb_scanout` (KmsDmabuf).

Patch check: `git apply --check` clean on the pristine tree and on the
pristine tree + M0 (`km0-pristine`).  Rebuilt against contract `8048d08`
(`struct exynos7580_device` carries `struct drm_device *drm`).

## 8. Integration notes

- `ex->drm` (contract `8048d08`) is the `struct drm_device *` owned by
  `drm_platform_init()`; `exynos7580_planes_init()` uses it for
  `drm_universal_plane_init()`.  `to_exynos7580(plane->dev)` returns
  `drm->dev_private` and is the only reverse mapping used here.
- The object must be added to the build by the integration seat
  (`exynosdrm-y += exynos_drm7580_plane.o` or the `drivers/gpu/drm/exynos7580/`
  equivalent); the Makefile edit used for the evidence is scratch-only.
- `exynos7580_planes_init()` must run **before** `exynos7580_crtc_init()`
  (agreed with DeconCrtc: the CRTC needs `planes[0]` as its primary plane).
- KmsFbdev's fb0 is a CMA framebuffer, not a KmsDmabuf one; both are handled
  through `exynos7580_fb_scanout()`, so no plane-side type test is needed.
- zpos/alpha stay unexposed until the atomic core grows the state fields and
  properties; the stacking order is already correct (plane index == window).
