# int82: legacy `crtc->fb` sync (async page flip -EBUSY) and PRIME tracing

int81 made modetest -s, dd and enumeration clean; kmscube renders and then

```
failed to queue page flip: Resource busy
```

with IRQ 234 climbing 125→322.

## 1. The -EBUSY is the legacy `crtc->fb` field

`drm_mode_page_flip_ioctl()` (3.10, `drm_crtc.c:3497`) does

```c
mutex_lock(&crtc->mutex);
if (crtc->fb == NULL) {
	ret = -EBUSY;   /* legacy field, "fb unbound" */
	goto out;
}
```

The atomic commit path never updates `crtc->fb` (only `plane->fb`, via the
helper's page_flip TODO).  So every page flip was rejected with -EBUSY before
ever reaching our `atomic_commit(async=true)` — our commit path itself returns
no -EBUSY.

Fix (`0130`, module): after every successful commit and in the page-flip
wrapper, sync the legacy fields from the atomic state
(`crtc->fb = primary->state->fb`, `plane->fb = state->fb`).  The commit entry
logs `async`, `crtc->fb` and `primary->state->fb`.

Trace for confirmation (`0129`, built-in): the EBUSY branch now logs
`crtc %u legacy fb NULL -> -EBUSY (primary->state->fb=%p)`.

## 2. PRIME / Mesa decision path

`drm_gem_prime_fd_to_handle()` now logs device name, prime fd, dma_buf
pointer and caller, and on import the object/handle/return
(`0129`, built-in).  A kmscube run with
`EGL_LOG_LEVEL=debug MESA_DEBUG=1 LIBGL_DEBUG=verbose` can then be matched
against these lines:

* no `fd_to_handle` line at all → Mesa never imports into card0 (kmsro not
  selected; llvmpipe path);
* `fd_to_handle: dev=exynos …` followed by `exynos7580_gem_prime_import: ok`
  → import works and the failure is later (ADDFB2/format/scanout).

Also worth trying on the device: `GBM_BACKEND=dri`, unset
`MESA_LOADER_DRIVER_OVERRIDE` vs `panfrost`, `PAN_MESA_DEBUG=debug`, and
checking that `exynos_dri.so` exists and that Mesa's kmsro pairs it with
panfrost (`renderer:` line decides).

## Artifacts (image rebuild — 0129 is built-in)

| file | size | sha256 |
|---|---|---|
| `Image-int82` | 18075368 | `ac2f2bfea0fc0007c7d5c9c69d7225c622eb5385bc2afa9a3d46a33af0284ff9` |
| `kernel-boot-int82.img` | 27213824 | `162bb2be527f7ca6b9dc8b130f1afc4f7c0f2e6be7be2b22d893a4e9b2cc69a4` |
| `exynos-drm7580-int82.ko` | 1434352 | `c313d8dc31febc0603a85a8adbf4f3e69465a0b43cf1f90961867b06ae6ea174` |
| `drm_kms_helper-int82.ko` | 1443520 | `93a395dd160139929aa972eb866c7caded5ad09360393bf4ed313a57b5d1ade8` |

## Gates (PASS)

All int81 gates plus the EBUSY branch trace, the PRIME fd_to_handle trace,
`crtc->fb = fb` in the page-flip wrapper, the post-commit `c->fb =
p->state->fb` sync, and the corresponding strings in vmlinux/module.

## Expected

`kmscube -D /dev/dri/card0 -c 30` completes 30 flips with no "Resource busy"
(renderer may still say llvmpipe — that proves the KMS flip path).  The second
run with the Mesa debug env shows either `renderer: panfrost` plus the
`fd_to_handle`/`prime_import` lines, or the exact point where Mesa avoids our
device.  `modetest -s`, `dd /dev/fb0`, IRQ growth and the watchdog harness
stay as before.

Log sha256s: build `f18180156065542175e87d4c6c2e5020f605ca1b8806a2b1b4a2b001f3812a9a`, gates `56b4d412a7b43a6b9c99caac33769f415d0328455207e0736b1f7ed678f342df`, evidence `ea1b944ad87b361b10a308c5632c21132b716ceefeb788db5677960a89b00d76`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
