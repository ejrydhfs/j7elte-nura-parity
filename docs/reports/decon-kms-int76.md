# int76: atomic-state initial modeset with an explicit fb0 commit (module-only)

int75 got as far as fb0 with a real scanout IOVA and DECON IRQ 234 counting,
then our own postcheck failed:

```
[drm:exynos7580_initial_modeset] *ERROR* primary plane has no fb0 after the initial modeset
```

Same mistake as int74's fbdev postcondition: `exynos7580_initial_modeset()`
tested the legacy `crtc->state->enable` / `crtc->primary->fb` fields, which the
atomic commit path does not maintain.

## Changes (`0120-kms-initial-modeset-atomic-state.patch`, module-only)

* Log the full atomic view before the check: crtc id, `enabled`, `state`,
  `state->active`, `state->enable`, `plane_list` empty, the primary plane,
  `plane->state`, `plane->state->fb` and the legacy `plane->fb`.
* Postcondition: `crtc->state && crtc->state->active && crtc->enabled &&
  plane->state && plane->state->fb`.
* If that is not satisfied, guarantee the first frame explicitly instead of
  failing: take the first fb in `mode_config.fb_list`, allocate an atomic
  state, `drm_atomic_get_plane_state()` + `drm_atomic_set_fb_for_plane()`,
  and `drm_atomic_commit()` it **with the modeset locks released** (the commit
  path takes its own), logging the fb and the return value.
* Re-check under the locks afterwards and only then return `-EIO`, with the
  full state in the message (`active`, `enabled`, `pstate->fb`, wanted fb).

## Artifacts (module-only)

| file | size | sha256 |
|---|---|---|
| `exynos-drm7580-int76.ko` | 1434352 | `17eade6e6a27d2aced81ef4faef890be456dae44c15fb53a85f29b0a03fa31ae` |
| `drm_kms_helper-int76.ko` | 1443520 | `341cc276a452680eee0bb5d078ec4f7bcf5cce76c74a9d090582c0327b4f5df5` |

`Image-int76` (`d8283fed…`, 18075368 bytes) is code-identical to int75 (only
the module source changed), so **keep the int75 image** and load this pair, or
flash int76 for completeness.

## Gates (PASS)

All int75 gates plus: the atomic-state log, `drm_atomic_set_fb_for_plane` and
`drm_atomic_commit(state)` present, the `pstate->fb` postcondition, the legacy
"primary plane has no fb0" message gone, and the four new log strings in the
module.

## Expected dmesg

```
exynos7580: load: initial modeset
[drm] initial modeset state: crtc id=… enabled=1 state=… active=1 … pstate->fb=… legacy->fb=…
```
(If the helper left the plane attached, no explicit commit is needed; otherwise
`initial modeset: explicit commit of fb …` and `… ret=0` follow, then the
re-check passes.)

Success: `kms_rc=0`, card0/fb0 persist, no Oops, DECON IRQs grow during a
flip, a write to `/dev/fb0` changes the panel.

Log sha256s: build `c981d6b96bcceabacf78eb2d5a18ba100beda032f233f4e4ebe1f3e6caa9c8a8`, gates `ce756120a29a9f87a25f61f80fc9a9b97f9fd426ec77d6d8a6dcf5e6fc13e6f1`, evidence `59e2c3068021b5c2bd7a2ac032e329305fc351ff8ddfffa5cb2141374a190726`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
