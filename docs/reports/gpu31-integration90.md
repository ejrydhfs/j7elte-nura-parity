# integration90 — atomic commit/check instrumentation for KWin's EINVAL

int90 = int89 + `patches/drm-kms/0140-kms-atomic-commit-plane-diag.patch`
(diagnostic only; Image and KMS module).

## What the existing log proves

KWin's failing commit only printed the int82 pre-swap line

```
exynos7580_atomic_commit: async=0 crtc->fb=0 primary->state->fb=0 by kwin_wayland
```

which runs *after* `prepare_planes` and `swap_state`, so either that line is
from an earlier (fbdev) commit and KWin's EINVAL is returned from the core
checks, or the failing commit is a different changeset.  The ATOMIC ioctl
routes through `drm_atomic_check_only()` before the driver's commit, and every
rejection there (plane core rules, CRTC active/enable, "requires full modeset
but allow_modeset=0", the driver's `drm_atomic_helper_check`) was logged only
at DRM_DEBUG_KMS, i.e. invisible.

## Instrumentation added

Core (`drivers/gpu/drm/drm_atomic.c`, temporary pr_info):

- `drm_atomic_plane_check`: full incoming plane state (plane id, crtc, fb,
  format, src 16.16, dst) and every rejection with values: both-set rule,
  `possible_crtcs`, format table index/count, coordinate overflow, source
  outside fb;
- `drm_atomic_crtc_check`: crtc id, active, enable, mode_changed,
  active_changed, and the active-without-enable failure;
- `drm_atomic_check_only`: the driver `atomic_check` rc together with
  `allow_modeset`, and the "requires full modeset" rejection with the CRTC's
  mode_changed/active_changed bits.

Driver (KMS module):

- `exynos7580_atomic_check`: dumps the whole changeset before and after
  `drm_atomic_helper_check` (one line per plane: id, crtc, fb, format, src,
  dst; per CRTC: active, enable, changed bits, mode_blob);
- `exynos7580_atomic_commit`: dumps the changeset on entry, logs
  `enable_link rc`, `prepare_planes rc` (with a second dump on failure),
  `swap_state done`, `commit_tail done`;
- `exynos7580_plane_atomic_check`: state line plus idx/max_win,
  `state_to_win rc` and decoded dst; every `exynos7580_check_rect()` rule logs
  its failing values (zero size, negative position, scaling, zero stride,
  outside fb, width < 128 B, x alignment); the fractional 16.16 `src_x/y/w/h`
  rule logs the raw values;
- `exynos7580_fb_scanout`: fb/plane/format/size/pitch on entry, bad
  fb/plane and the final rc/address.

No behaviour change: same return codes, same register writes.

## Reading the next KWin run

`dmesg | grep -E 'drm_atomic_(plane|crtc)_check|drm_atomic_check_only|exynos7580_(atomic|plane_atomic_check|check_rect|fb_scanout)'`

The first failing block identifies the fix:

1. `drm_atomic_check_only: FAIL ... requires full modeset ... allow_modeset=0`
   — KWin's commit is flagged mode_changed/active_changed but did not pass
   `DRM_MODE_ATOMIC_ALLOW_MODESET`.  Either the commit genuinely changes the
   mode/active state (KWin bug/ordering) or a property handler of ours sets
   the changed bit on every commit (check `drm_atomic_set_mode_prop_for_crtc`
   and the ACTIVE setter in 0135): the printed changed bits say which.
2. `drm_atomic_crtc_check: FAIL crtc=... active=1 enable=0` — the ACTIVE
   property was set without a MODE_ID/enable in the same state (our MODE_ID
   setter only sets enable when the blob changes).  Fix belongs to the CRTC
   property path (accept ACTIVE on an enabled current state, or derive enable
   from the current state).
3. `drm_atomic_plane_check: FAIL ... fmt=0x... not in N formats` — KWin uses
   a fourcc our plane list lacks (the log prints it; XR24/AR24 are present).
4. `exynos7580_check_rect: FAIL ...` — the named rule rejects the full-screen
   720x1280 plane; relax only the named rule, with the logged values.
5. `exynos7580_plane_atomic_check: FAIL fractional src=...` — KWin sends a
   non-integral 16.16 src; if the .16 part is zero-equivalent after rounding we
   can accept/truncate it, otherwise it is a scaling request we cannot serve.
6. `prepare_planes rc=-...` — a helper `prepare_fb` failure (ours implement
   none) or a framebuffer reference problem; the entry dump shows the state.
7. No diagnostic lines at all for the failing commit — the EINVAL is returned
   before `drm_atomic_check_only` (e.g. ioctl argument validation or a lock
   failure); then the failing ioctl number is the next thing to log.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int90` | 18191112 | `37cc02ef4b7595a73e299248dfe75331de5919967896e0f233333f265a8a20b6` |
| `kernel-boot-int90.img` | 27330560 | `21543e0ac75353ffa1ae44f728cb13c824f89889213111c656a41dcd5ad5aaaf` |
| `exynos-drm7580-int90.ko` | — | `3bff6c02d57b9f1701e13456c74ddb97312375ec4680a1a8914dcc3a0d003a2b` |
| `drm_kms_helper-int90.ko` | — | `d29652025a8ab3df205bf826c37d037dc4109d99d07e3941272ed45a20b144c1` |

Build 0 errors.  All int89 gates PASS plus `int90 gate: atomic
commit/check diagnostics` (each source site present, core strings in vmlinux,
driver strings in the KMS .ko).  Log sha256s: build `b0b55b77e3532a50ebe0e32c872bbee980c9ae24f791249d6926c72f2805d4c0`, gates `41840d1c00c9dab657afa7720b0bdda5893f3632765e9eea73a5a7405baed2ea`,
evidence `84e536b419690bfcc34c843e27f473bc9204ed17dc9537862da556f271c3f482`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
