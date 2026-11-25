# integration92 — commit re-enables a non-ON DECON (no more -EAGAIN frames)

Live KWin reported every later frame as

```
exynos7580_plane_atomic_update_cb: [PLANE:16] atomic update failed: -11
```

-11 came from the built-in shim's `decon_kms_commit()`:

```c
	if (decon->state != DECON_STATE_ON)
		return -EAGAIN;
```

The initial fbdev restore / KWin's early commit sequence can leave the DECON
off while the KMS CRTC stays active.  Every frame then failed with -EAGAIN
(and `exynos7580_plane_atomic_update_cb` only logs it), so the panel stayed
black while vblank IRQs kept counting.

## Fix (`patches/drm-kms/0142-kms-decon-commit-reenable.patch`)

Built-in shim (`drivers/video/exynos/decon_7580/decon_kms_api.c`):

- `decon_kms_commit()`: when the state is not ON, log
  `decon_kms_commit: state=N -> enable -> retry by <comm>`, call
  `decon_kms_enable()` (the same sequence `crtc_enable_link()` uses), log
  `enable rc=N state=N`, and retry the commit exactly once.  If enabling fails
  or the state is still not ON, log the register/state dump and return that
  errno (or -EAGAIN).  No loop, so the commit stays bounded.
- `decon_kms_disable()`: logs `state=N by <comm>` on every call (including the
  already-off early return).
- The vendor fbdev async path (`decon_kms_commit_async`) is untouched: its
  pre-signalled-fence-on-off behaviour is the documented fd ABI.

KMS module (`exynos_drm7580_crtc.c`):

- `exynos7580_crtc_disable_link()` logs `disabling link state (by <comm>)`
  when it actually disables;
- `exynos7580_crtc_disable()` logs `[CRTC:N] crtc_disable by <comm>`;
- `exynos7580_crtc_dpms()` logs `[CRTC:N] dpms mode=M by <comm>`.

Together the shim and module lines show the disable -> re-enable sequence and
who triggered it, next to int90's atomic diagnostics.

## Artifacts (Image needed: the shim is built-in)

| file | size | sha256 |
|---|---|---|
| `Image-int92` | 18191112 | `77ea2de0708a44ce8515cd22b482814e8609df279b6d1d94a5a4a0d58ee6a6f7` |
| `kernel-boot-int92.img` | 27330560 | `30a562b8bad3ea376a065259e826ef4b75c28ede99d17e5e21f68bb289d2de8a` |
| `exynos-drm7580-int92.ko` | — | `f0bc1b42e5ead9a0c1c21292707171dda2db93ff9bf608470e89e87dd0a75873` |
| `drm_kms_helper-int92.ko` | — | `d0abf5d903fc20b75bb35257c146e9ba403b47a325ebc761996260c749d1e0bb` |

Build 0 errors.  All int91 gates plus `int92 gate: commit re-enables a non-ON
DECON, disables are logged`: shim re-enable path + both log strings, the old
bare `return -EAGAIN` gone from `decon_kms_commit()`, module comm logs, shim
string in vmlinux, module strings in the KMS .ko.  Log sha256s: build `931433700570307169d7b4eb4aac85bcf170a3ff8f8464990f2d12b7686d0473`,
gates `18d5099cf72ab90ac3a12909541b84c6d40b6aa1c216a2369d2c493ef3239ba6`, evidence `3f3336e62013d4627a1cf8b1e1f72cbc600f07c3f66cf87cabd1c2e129daa13a`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Expected dmesg

First frame after the phantom disable:

```
decon_kms_commit: state=<N> -> enable -> retry by kwin_wayland
decon_kms_enable: enter state=0 ...
decon_kms_enable: decon_enable ret=0 state=<on>
decon_kms_commit: enable rc=0 state=<on>
```

then commit programming as usual (int90's `exynos7580_atomic_commit` stage
lines).  The disable source appears as

```
decon_kms_disable: state=<N> by <comm>
exynos7580_crtc_disable_link: disabling link state (by <comm>)
[CRTC:N] crtc_disable by <comm>
```

Acceptance: no `atomic update failed: -11` after the first frame; the panel
shows KWin's composited content (at least its clear colour); `kmscube` and
`modetest -s` unchanged.
