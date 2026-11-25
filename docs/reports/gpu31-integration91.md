# integration91 — stale needs_full_modeset no longer forces every commit

int90's log named the failure:

```
drm_atomic_check_only: FAIL crtc=19 requires full modeset
    (mode_changed=1 active_changed=0) but allow_modeset=0
```

`exynos7580_crtc_atomic_check()` copied `crtc_link.needs_full_modeset` into
`state->mode_changed` on every commit.  The flag is set by `disable_link()`
and at crtc_init, but was cleared only in `crtc->mode_set()` — which the
atomic commit tail reaches only for commits already classified as full
modesets.  Once set, every plane-only commit (KWin's flips, no
DRM_MODE_ATOMIC_ALLOW_MODESET) was turned into a full modeset and rejected by
`drm_atomic_check_only()` with -EINVAL.

## Fix (module-only: `patches/drm-kms/0141-kms-crtc-link-modeset-gating.patch`)

- `exynos7580_crtc_enable_link()` clears `needs_full_modeset` whenever it
  returns with the link on: on the already-on early return (stale flag) and
  after the successful `decon_kms_power_get()`/`decon_kms_enable()` sequence.
  The atomic_commit() enable path never calls `mode_set()`, so this is the
  only place that makes the flag truthful; the old clear in `mode_set()` is
  kept.
- `exynos7580_crtc_atomic_check()` now derives its decision from the actual
  transition:
  - **activating** (`state->active && state->enable` and the current state is
    inactive): the commit already calls `enable_link()` via
    `exynos7580_crtc_enabling()`, so no `mode_changed` forcing — the first
    enable after probe/resume keeps its full link/power sequence;
  - **mode_changed / active_changed**: a real full modeset, unchanged;
  - **link off while staying active** (a disable or pm cycle happened):
    DECON registers are gone, so the commit must re-init the pipe.  It forces
    `mode_changed` only when the state carries ALLOW_MODESET; a plane-only
    commit with the link actually off is rejected with a named
    `link_off plane-only commit without ALLOW_MODESET` log instead of
    programming a dead pipe;
  - **link on**: a stale flag can never force anything.
- One decision log per check:
  `[CRTC:%d] atomic_check: needs_full_modeset=%d link_on=%d activating=%d
  mode_changed=%d active_changed=%d allow_modeset=%d modeset=%d[ (forced:
  link_off)]`.

int90's diagnostic prints are kept, so the next run shows whether the link
was genuinely off when KWin commits.

## Artifacts

Module-only change, but the Image is rebuilt by the series (kernel builds
embed a build timestamp, so its sha changes even though no built-in source
did):

| file | size | sha256 |
|---|---|---|
| `exynos-drm7580-int91.ko` | — | `1bd6751088f9cb76406b682f79e219e7ea483587ef15777039dfc7959fb0865f` |
| `drm_kms_helper-int91.ko` | — | `f32405171922e7b3b509090762d632ba83e54d91ea7e6e5679839a587c2ab161` |
| `Image-int91` | 18191112 | `9d3bf21cc2c08985de96c543c146bd7099cffe8f12ada3f3c3577ffa12e6f292` |
| `kernel-boot-int91.img` | 27330560 | `343a6ccdff885afaf124b62e15c06fc83eb2839f71097ee44d79845edbe98402` |

Build 0 errors.  All int90 gates plus `int91 gate: stale link flag cannot
force modesets`: decision log present, both clear sites present, link_off
rejection present, the old unconditional
`needs_full_modeset && !state->mode_changed` forcing absent
(source and KMS .ko strings checked).  Log sha256s: build `45401356c29897e62fe2e18829f50113309fdfc9b25daff707b033ffca7d273a`, gates
`0e8495ac27ec9027bdf4d2f872b08ac1888b716cc9212d3c137a791b7177985e`, evidence `eb95a2f83b722c71900453d28b6c3b6119c446634449e94793ca8c0d695f432c`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Device acceptance

Load the two int91 .ko (Image not required for this fix) and rerun kwin-try:

- `drm_atomic_check_only` has no `FAIL ... requires full modeset` for KWin's
  plane-only commits; instead the decision log shows
  `needs_full_modeset=0 link_on=1 ... allow_modeset=0 modeset=0`;
- kwin-try runs 20 s without `Atomic commit failed`;
- the first commit after load/resume still logs
  `enable_link: link on, needs_full_modeset cleared` and the activation path;
- `modetest -s` and `kmscube` still work (their operation is unchanged: they
  send full modesets with ALLOW_MODESET);
- after a `disable` (modetest off / blank), an ALLOW_MODESET=1 commit
  re-enables the link and logs `(forced: link_off)` at most once, then clears
  it again.
