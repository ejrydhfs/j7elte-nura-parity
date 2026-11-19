# GPU31 packaging — winning stack, reproducible build, rollback, patch series

Date: 2026-11-19. Slice: `G31_Packaging` (pmaports + patches + docs).

## 1. Verdict: the winning stack is software GL

Live device state (locked SSH, 2026-11-19; kernel `#35`, uptime fresh —
**no reboot performed, none needed**):

| Check | Result |
|---|---|
| `uname` | `3.10.108 #35 SMP PREEMPT Wed Nov 18 23:36 EST 2026 aarch64` |
| `/dev/dri` | absent (`No such file or directory`) — running kernel predates M1 |
| `j7elte-gpu-soft` | `j7elte-gpu-soft-1-r0 aarch64 (MIT) [installed]` |
| `/etc/profile.d/j7elte-softgl.sh` | present, `export LIBGL_ALWAYS_SOFTWARE=1` |
| `DISPLAY=:0 glxinfo -B` | `llvmpipe (LLVM 23.1.5, 128 bits)`, Mesa 26.3.0, `Accelerated: no` |
| `/etc/X11/xorg.conf.d/` | `10-fbdev.conf` (this package) + `99-fbdev.conf` (equivalent display-work stanza) |

So: X on fbdev + Mesa llvmpipe is the shipped, working 3D path. There is no
DRM render node on the running kernel and no redistributable Mali userspace
(`gpu-userspace.md` §3–§4), so hardware GL is not packageable today. The M1–M3
kernel/driver work (`gpu-drm.md`, `gpu31-master.md`) is the route to HW GL;
when it lands, this package's `j7elte-softgl.sh` is removed (it says so in
the file) and packaging is revisited.

Stale-report correction: `gpu-drm.md` / `gpu-verify.md` passages written
while the USB link was down describe the phone as unreachable. The phone is
UP (`#35`, RNDIS/SSH working); only the `/dev/dri`-absent observation still
holds, because the running image predates the gated M1 flash.

## 2. What is packaged (all already in the repo, verified in sync)

| Artifact | Location | State |
|---|---|---|
| `j7elte-gpu-soft` APKBUILD + `10-fbdev.conf` + `j7elte-softgl.sh` | `code/pmaports/device/archived/j7elte-gpu-soft/` | installed on device from this source (built on-device 2026-11-18, `gpu-verify.md` §2) |
| pmaports patch (new-package diff) | `patches/userspace/gpu/0001-j7elte-gpu-soft-package.patch` | byte-identical to tree (verified §4) |
| `device-samsung-j7elte` depends line (`j7elte-gpu-soft`) | `code/pmaports/device/archived/device-samsung-j7elte/APKBUILD:22` | already in tree since baseline `b041151`; no new patch needed (baseline pmaports already carries it) |
| on-device status script | `code/userspace/gpu/check-3d.sh` | read-only, no reboot |
| kbase UAPI probe (source + Makefile) | `code/userspace/gpu/kbase-probe/` | freestanding aarch64, no libc |
| M1 fragment | `patches/drm-m1/m1-kconfig.fragment` + mirror `code/gpu31/series/m1-kconfig.fragment` | gated, not flashed |
| M3 foundation series | `patches/drm-backport/0001-0005` + mirror `code/gpu31/series/` | host-checked, not a driver yet |
| display KMS candidate (code only) | `patches/drm/0001` + mirror `code/gpu31/series/0001-drm-exynos7580-decon-dsim-kms.patch` | candidate, not applied anywhere |

## 3. Reproducible build + install

Scripts (this slice, under `code/gpu31/package/`):

- `regen-series.sh` — re-copies `code/gpu31/series/` from the authoritative
  `patches/*` dirs and rewrites `SHA256SUMS`. Run from repo root:
  `sh code/gpu31/package/regen-series.sh`.
- `build-install.sh` — documents the host (`abuild -r` against an aports
  tree) and on-device (`/data/gpusoft`, `REPODEST=/data/pkgs`, because `/`
  is 94% full) builds, the install (`apk add j7elte-gpu-soft`) and the
  no-reboot verify (`code/userspace/gpu/check-3d.sh`).
- `rollback.sh` — `apk del j7elte-gpu-soft` + removes the two installed
  files; X falls back to the equivalent `99-fbdev.conf` stanza.

Checksums (package inputs):

```
738cc58dc3449da8288adc6a80c90180ddad49265e75bb36b0497c33afaf92d9474b3c45dc3c97ec87733b0311577060d4aa4e3cf2e5d3d1ca8bc8caa8dbfe7a  10-fbdev.conf
141e1e5738cb34d4bc2800eb24fc5898b42391ae6885f8a9763841af5d9fa4f55c3c43fc9e27c0fc41be219f8954decad104abca16fc7bde2b269b37dcc93a45  j7elte-softgl.sh
8310301a594aae562a72f732554896f091c51743e4196aa9ae2edcb5238f8805be82bd65e418a69cfb3a854d4e2a432e1de50cbf56a936d9719439f05b32e966  APKBUILD
```

## 4. Patch-series regeneration + `git apply --check`

The `code/gpu31/series/` mirror is regenerated from `patches/*` (copy, no
edits) and matches:

```
diff -q code/gpu31/series/0001-dma-fence-minimal-subset.patch \
        patches/drm-backport/0001-dma-fence-minimal-subset.patch  -> identical
diff -q code/gpu31/series/0001-drm-exynos7580-decon-dsim-kms.patch \
        patches/drm/0001-drm-exynos7580-decon-dsim-kms.patch      -> identical
diff -q code/gpu31/series/m1-kconfig.fragment \
        patches/drm-m1/m1-kconfig.fragment                        -> identical
```

`git apply --check` from the repo root (kernel/Mesa/DT patches are
checked for well-formedness; their target trees are not in this repo —
`patches/drm*` apply to the shared kernel tree via the integrator,
`patches/gpu-mesa-*` to a Mesa checkout via `apply.sh`):

| Patch | `git apply --check` | Notes |
|---|---|---|
| `patches/userspace/gpu/0001-j7elte-gpu-soft-package.patch` | **clean** | applies to a pmaports checkout; extracted tree is byte-identical to `code/pmaports/device/archived/j7elte-gpu-soft/` (`diff -r` empty) |
| `patches/pmaports/0003-firmware-samsung-j7elte-package.patch` | **clean** | pre-existing, untouched |
| `patches/drm-backport/0001-0004` | **clean** | new files (`drivers/base/dma-fence.c`, `dma-resv`, `drm_syncobj`, `drm_sched`) |
| `patches/drm-backport/0005-build-wiring-for-shims.patch` | check needs kernel tree (`drivers/base/Makefile`) | new-file series otherwise; integrator applies in shared tree |
| `patches/drm-m1/` fragment, `patches/iommu-dma/`, `patches/drm/0001-0002`, `patches/gpu-mesa-kbase/0001-0003`, `patches/gpu-mesa-render/*.diff`, `patches/userspace/gpu-panfrost/0001` | check needs their target trees (kernel / Mesa / DT) | not regressed by this slice; owners' READMEs record per-target status |

## 5. Rollback

Userspace rollback is `code/gpu31/package/rollback.sh` (no flash, no
reboot): `apk del j7elte-gpu-soft`, delete the two installed files, re-run
`check-3d.sh`. Kernel rollback (only if the integrator ever flashes a test
image — NOT done by this slice): reflash the known-good boot image per
`code/kernel/recovery/RECOVERY.md`; stock `dt.img` is never touched.

## 6. Files changed by this slice

- `code/gpu31/package/regen-series.sh` (new)
- `code/gpu31/package/build-install.sh` (new)
- `code/gpu31/package/rollback.sh` (new)
- `docs/reports/gpu31-package.md` (this report)
