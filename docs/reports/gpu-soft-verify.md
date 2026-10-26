# Verification — `j7elte-gpu-soft` (Mesa llvmpipe via Xorg fbdev)

**Verdict: package PASS.** The APKBUILD is valid, the patch applies cleanly to
pmaports `ba682bd`, the checksums match, `package()` installs exactly the two
intended files with correct modes, and the Xorg snippet/profile are both valid
and compatible with this kernel (DECON fbdev, no DRM). No edit was made to the
package because no defect was found in it.

One **integration defect outside the package** was found: `j7elte-gpu-soft` is
not in `device-samsung-j7elte`'s depends, so a default install never gets it.
That file is shared device config owned elsewhere, so the fix is proposed below
as a diff, not applied (see §8 F1).

All verification is static plus host-side execution (Xorg config parse,
`sh -n`, checksum/package simulation). No aarch64 rootfs run, no j7elte
hardware, no `/dev/fb0` on the verification host; runtime GL was not executed.

---

## 1. Package metadata (`device/archived/j7elte-gpu-soft/APKBUILD`)

| Field | Value | Check |
|---|---|---|
| `pkgname` | `j7elte-gpu-soft` | valid abuild charset; unique in pmaports |
| `pkgver` / `pkgrel` | `1` / `0` | `apk version --check`-valid values (integer ver, rel 0) |
| `pkgdesc` | `Software OpenGL (Mesa llvmpipe) on fbdev for the Galaxy J7 (j7elte)` | 76 bytes, one line (<128 limit) |
| `url` | `https://postmarketos.org` | present |
| `arch` | `aarch64` | matches device (`deviceinfo_arch="aarch64"`) |
| `license` | `MIT` | config + shell snippets authored in this repo; no upstream code |
| `options` | `!check !archcheck` | no check() defined; arch is fixed aarch64 |
| `depends` | `mesa-dri-gallium xf86-video-fbdev xorg-server` | all resolvable for aarch64 edge; see §4 |
| `source` | `10-fbdev.conf`, `j7elte-softgl.sh` | exactly the two files shipped by the patch |
| install paths | `/etc/X11/xorg.conf.d/10-fbdev.conf`, `/etc/profile.d/j7elte-softgl.sh` | see §3 |
| `build()` | absent | acceptable: abuild defines `build() { :; }` (`abuild.in:745` in upstream abuild) |
| `package()` | present | required and present (`abuild.in:205`) |

`builddir="$srcdir"` is harmless (unused without a build function).

## 2. Sources, checksums, patch application

Actual checksums of the files in the package directory:

```
738cc58dc3449da8288adc6a80c90180ddad49265e75bb36b0497c33afaf92d9474b3c45dc3c97ec87733b0311577060d4aa4e3cf2e5d3d1ca8bc8caa8dbfe7a  10-fbdev.conf
141e1e5738cb34d4bc2800eb24fc5898b42391ae6885f8a9763841af5d9fa4f55c3c43fc9e27c0fc41be219f8954decad104abca16fc7bde2b269b37dcc93a45  j7elte-softgl.sh
```

Both match the APKBUILD's `sha512sums` exactly (`sha512sum` output compared
byte-for-byte).

Patch test against a clean worktree of the pinned baseline:

```
pmaports HEAD = ba682bdf2272a1e23c5e33d836c49e2a903ca076 ("baseline: pmaports j7elte packages")
git worktree add --detach /tmp/pm-clean ba682bd
git -C /tmp/pm-clean apply --check parity/patches/userspace/gpu/0001-j7elte-gpu-soft-package.patch   -> OK
git -C /tmp/pm-clean apply         <same patch>                                                     -> OK
  device/archived/j7elte-gpu-soft/10-fbdev.conf    | 15 +++
  device/archived/j7elte-gpu-soft/APKBUILD         | 33 +++
  device/archived/j7elte-gpu-soft/j7elte-softgl.sh |  5 +++
  3 files changed, 53 insertions(+)
sha512sum -c (embedded sums against the applied files) -> OK, OK
diff -ru /tmp/pm-clean/device/archived/j7elte-gpu-soft <working tree> -> empty
```

So: patch applies to `ba682bd`, the applied tree is byte-identical to the
package in the working tree, and the embedded checksums are self-consistent.
The patch is a pure new-file diff with paths relative to the pmaports root —
no `--directory` flag needed.

Note: the package directory is currently untracked in the pmaports git index
(`git status` shows `?? device/archived/j7elte-gpu-soft/`), which is expected
for a not-yet-committed patch. abuild tolerates this: `git_last_commit` returns
empty and `SOURCE_DATE_EPOCH` falls back to the APKBUILD mtime
(`abuild.in:114-121`); it is not a build failure.

## 3. `package()` simulation

`package()` was executed with `srcdir`/`pkgdir` pointing at an empty fake root
(no abuild/apk available on the host):

```
srcdir=<pkgdir of package> pkgdir=/tmp/pkgtest/pkgdir sh -c '. ./APKBUILD; package'
```

Result:

```
d /tmp/pkgtest/pkgdir/etc/X11/xorg.conf.d
f 644 261  /tmp/pkgtest/pkgdir/etc/X11/xorg.conf.d/10-fbdev.conf
d /tmp/pkgtest/pkgdir/etc/profile.d
f 644 322  /tmp/pkgtest/pkgdir/etc/profile.d/j7elte-softgl.sh
```

Exactly two files, both mode `0644`, byte-identical to the sources. No extra
files, no accidental paths. `sh -n` on the APKBUILD and on the profile snippet:
OK.

No path collision with dependencies: xorg-server installs only
`/usr/share/X11/xorg.conf.d/10-quirks.conf`; `xf86-video-fbdev`,
`mesa-dri-gallium` and `mesa-gl` ship no `/etc/X11` or `/etc/profile.d` files.
The `/etc` location is also what makes this snippet outrank the `/usr/share`
quirks file, as intended.

## 4. Dependency resolution (Alpine edge, aarch64 — pmOS uses the same packages)

Evidence: `APKINDEX` for `edge/main` and `edge/community`
(`dl-cdn.alpinelinux.org/alpine/edge/{main,community}/aarch64/APKINDEX.tar.gz`,
2026-10-24) plus streamed `.apk` tar listings.

| Package | Version / repo | Provides | Depends (relevant) |
|---|---|---|---|
| `mesa-dri-gallium` | 26.2.5-r0, main | `usr/lib/dri/swrast_dri.so`, `kms_swrast_dri.so`, `panfrost_dri.so` | `mesa=26.2.5-r0`, `so:libgbm.so.1` |
| `xf86-video-fbdev` | 0.5.0-r6, community | `usr/lib/xorg/modules/drivers/fbdev_drv.so` | `so:libc.musl-aarch64.so.1` |
| `xorg-server` | 21.1.25-r0, community | `usr/lib/xorg/modules/extensions/libglx.so`, `.../libfbdevhw.so`, setuid `usr/libexec/Xorg.wrap` | `mesa-egl`, `so:libGL.so.1`, `so:libgbm.so.1`, `font-*`, `xkbcomp`, … |
| providers | — | `so:libGL.so.1` → only `mesa-gl`; `so:libEGL.so.1` → `mesa-egl`; `so:libgbm.so.1` → `mesa-gbm` | — |

Conclusion: the three depends are sufficient and complete. `libGL`/`libEGL` are
pulled transitively because `xorg-server` depends on `so:libGL.so.1` and
`mesa-egl`; `libgbm` via `so:libgbm.so.1`. `mesa-dri-gallium` supplies the
`swrast` driver itself. Adding `mesa-gl` explicitly would only harden against a
future provider switch and is not required today.

## 5. Xorg configuration

`10-fbdev.conf` content is the minimal fbdev stanza (Device/Monitor/Screen, no
Display subsection, no ServerLayout).

* **Real parse test.** Host `xorg-server 21.1.25-1` — the *same* upstream
  release as Alpine edge `xorg-server-21.1.25-r0` (the server self-reports
  `X.Org X Server 1.21.1.25`) — was started with the file as `-config`:

  ```
  (++) Using config file: ".../10-fbdev.conf"
  (**) |-->Screen "Screen0" (0)
  (**) |   |-->Monitor "Panel"
  (**) |   |-->Device "Fbdev"
  (II) LoadModule: "fbdev"
  (WW) Warning, couldn't open module fbdev
  (EE) Failed to load module "fbdev" (module does not exist, 0)
  ```

  The only failure is the host not having `fbdev_drv.so` (the aarch64 module
  cannot load on x86_64). Section parsing and the Screen→Monitor/Device wiring
  are correct.

* **Driver/option names.** `xf86-video-fbdev` has `DriverName "fbdev"` and
  registers the string option `fbdev` (`fbdev.c:120,127`), which is passed to
  `fbdevHWInit()` and used as the device path (`fbdevhw.c:382-393`). So
  `Driver "fbdev"` + `Option "fbdev" "/dev/fb0"` are both valid.

* **No `DefaultDepth` needed.** `FBDevPreInit` asks the kernel for the current
  framebuffer depth (`fbdevHWGetDepth`, red+green+blue lengths) and passes it
  into `xf86SetDepthBpp` (`fbdev.c:441,474`); `xf86SetDepthBpp` only falls back
  to `GLOBAL_DEFAULT_DEPTH 24` when neither command line, config nor probe
  supplies one (`xf86Helper.c:411-440`). DECON reports `default_bpp = 32`
  (`decon_core.c:2861`), i.e. depth 24 / bpp 32.

* **No Display subsection needed.** With no `Display` entries,
  `pScrn->display->modes == NULL`, so `fbdevHWSetVideoModes()` returns early
  (`fbdevhw.c:518-520`) and `fbdevHWUseBuildinMode()` (`fbdev.c:578-579`) uses
  the mode probed from `/dev/fb0` (DECON 720×1280 from the DT
  `resolution = <720 1280>`). This is the intended behaviour on a fixed phone
  panel: X adopts the panel's current mode.

* **Mode-set compatibility [inference from source].** On first ScreenInit the
  driver issues one `FBIOPUT_VSCREENINFO` built by `xfree2fbdev_fblayout` /
  `xfree2fbdev_timing` (bpp 32, 8/8/8 lengths, probed timings). DECON's
  `decon_check_var` accepts 32 bpp, normalises to 8:8:8 and keeps virtual
  dimensions ≥ requested; `fbdev_modes_equal` compares bpp, colour lengths,
  resolution, margins, sync and vmode, all of which the DECON normalisation
  preserves (`decon-int_drv.c:388+`, `fbdevhw.c:149-220`). Expected to pass;
  not hardware-tested.

* **Access to `/dev/fb0`.** Alpine's `/usr/bin/Xorg` is a wrapper that execs
  `Xorg.wrap`, which is setuid root (mode `4755`), so the server can open the
  framebuffer without a `video` group membership.

* **GLX without DRM.** Alpine's `libglx.so` contains the `DRI_SWRast` provider
  and the strings `swrast`, `LIBGL_DRIVERS_PATH`, `/usr/lib/dri`. The server
  loads `/usr/lib/dri/swrast_dri.so`, which `mesa-dri-gallium` provides. No
  DRM/GBM is needed for this path (`glx/glxdriswrast.c` uses the swrast loader
  extension, not a DRM fd).

## 6. Kernel/display-path compatibility and conflicts

Baseline `ba682bd` config (`device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`;
the working-tree modifications touch only Bluetooth/Wi-Fi strings, not display):

| Symbol | Line | Meaning |
|---|---|---|
| `# CONFIG_DRM is not set` | 2262 | no DRM/KMS → fbdev is the only usable X path; no vendor DRM to conflict with |
| `CONFIG_FB=y`, `CONFIG_FB_CFB_*` | 2312+ | fbdev core present |
| `CONFIG_EXYNOS_DECON_FB=y` | 2358 | DECON framebuffer (the `/dev/fb0` provider) |
| `CONFIG_EXYNOS_DECON_LCD=y`, `CONFIG_EXYNOS_MIPI_DSI=y` | 2362, 2351 | panel/DSIM path |
| `# CONFIG_FB_S3C is not set` | 2340 | no competing Samsung fb driver |
| `CONFIG_FB_SIMPLE=y` | 2349 | enabled, but no `simple-framebuffer` node exists in `arch/arm64/boot/dts/` (0 grep hits) → cannot claim fb0 |
| `CONFIG_VT=y` | 1568 | VT ioctls Xorg needs |
| `# CONFIG_FRAMEBUFFER_CONSOLE is not set` | 2412 | Xorg does not need fbcon; only kernel-console visibility is affected (separate display-userspace gap) |

`decon_core.c` calls `register_framebuffer()` once, for the default window
(`decon_core.c:4590-4591`), so the device is `/dev/fb0` with var
`bits_per_pixel = 32` (`decon_core.c:2861,2896`) and the full fb ops set Xorg
uses: `check_var`, `set_par`, `blank`, `setcolreg`, `pan_display`, `mmap`,
`fillrect`, `copyarea`, `imageblit` (`decon_core.c:2501+`,
`decon-int_drv.c`). The framebuffer `fbinfo->var` is populated via
`fb_videomode_to_var()` from the panel mode, so `FBIOGET_VSCREENINFO` returns
720×1280.

**Conflicts: none found.**

* No other pmaports package writes `/etc/X11/xorg.conf.d/**` or a profile.d GL
  override (repo-wide grep). `xorg-server`'s only config file lives in
  `/usr/share/X11/xorg.conf.d/`.
* `j7elte-fde` also uses `/dev/fb0`, but only from its initramfs hook
  (`50-j7elte-fde-fb.sh`) to unblank the panel/backlight before `unl0kr`; the
  hook exits before the rootfs switch and installs no Xorg config. No runtime
  or file conflict.
* `LIBGL_ALWAYS_SOFTWARE=1` cannot mask a vendor/DRM driver: this kernel has no
  DRM node and no usable hardware GL userspace (`gpu-userspace.md` §1–§4). It
  only biases Mesa clients toward swrast; Xorg's GLX always uses its swrast
  provider on this DDX anyway.

## 7. `LIBGL_ALWAYS_SOFTWARE` profile snippet

`/etc/profile.d/j7elte-softgl.sh` is mode 0644, `sh -n` clean, and exports the
single variable. Alpine's `alpine-baselayout` ships
`/etc/profile.d/README` stating that `*.sh` files there are evaluated by
**login shells**. So the intended path (console login → `startx` → GL clients)
inherits it. Non-login shells or a display manager would not — but this package
installs no display manager, so this is a documented caveat, not a defect. The
variable is advisory for GLX (the server's provider choice is already swrast)
and matters for EGL/GL clients that would otherwise probe for hardware.

## 8. Findings

### F1 — Integration defect (outside this package, not applied): missing device dependency

`device/archived/device-samsung-j7elte/APKBUILD` depends on nine helper
packages but not `j7elte-gpu-soft` (lines 12-26):

```
	linux-samsung-j7elte
	firmware-samsung-j7elte
	j7elte-audio-ucm
	j7elte-bt
	j7elte-cam
	j7elte-cbd
	j7elte-fde
	j7elte-gpsd
	j7elte-power          <-- no j7elte-gpu-soft here
	j7elte-sensors
	j7elte-wifi
```

`parity/patches/userspace/0001-device-deps.patch` adds those nine and was
created at 21:48; the gpu-soft package directory was created at 21:57, i.e. the
dependency list was written before the package existed. Effect: a normal
`pmbootstrap install` for j7elte installs neither the Xorg fbdev config nor the
software-GL environment, so the only working 3D path documented in
`gpu-userspace.md` §5/§6 is not present on the device.

`device-samsung-j7elte` is shared device config owned by another agent, so this
was **not** edited. Proposed diff, to be folded into
`parity/patches/userspace/0001-device-deps.patch` (alphabetical position):

```diff
--- a/device/archived/device-samsung-j7elte/APKBUILD
+++ b/device/archived/device-samsung-j7elte/APKBUILD
@@
 	j7elte-fde
 	j7elte-gpsd
+	j7elte-gpu-soft
 	j7elte-power
 	j7elte-sensors
 	j7elte-wifi
```

### F2 — Caveat, no change: no input driver

`xorg-server` does not depend on an input driver and this package does not add
one, so X starts with no keyboard/touch device until `xf86-input-libinput` (or
a UI package) is installed. `glxinfo`/`glxgears` under `xinit` still run, and
input is outside a GL-enablement package's stated scope; noted for the display
userspace gap rather than changed here.

### F3 — Checked and not defects

Missing `DefaultDepth`, missing `Display` subsection, missing `ServerLayout`,
`builddir` without `build()`, untracked APKBUILD, absence of an explicit
`mesa-gl` dependency — all verified benign in §1, §3, §4, §5.

## 9. Residual risk / not verified

* No `abuild`/`apk` on the verification host: `package()` was simulated into a
  fake `pkgdir`; no `.apk` was built, so packaging metadata (PKGINFO, file
  ownership) was not produced by abuild itself.
* No hardware and no aarch64 userland: Xorg was not started against a real
  DECON framebuffer. The fbdev↔DECON mode/ioctl compatibility in §5 is
  source-derived (`[INFERENCE]`); the residual risk is the single
  `FBIOPUT_VSCREENINFO` at ScreenInit and `FBIOBLANK`/`FBIOPAN_DISPLAY`
  behaviour on the DECON driver.
* The Xorg parse test used the same upstream xorg-server release (21.1.25) but
  the host module set; it proves config syntax/wiring only.

## 10. Suggested checks once an aarch64 build/device is available

```
pmbootstrap build j7elte-gpu-soft            # abuild + sha512sums, real .apk
pmbootstrap install --add j7elte-gpu-soft    # or rely on F1 if applied
# on device (console login):
apk info -L j7elte-gpu-soft                  # exactly the two files
startx &                                     # Xorg log: FBDEV(0), 720x1280x24
DISPLAY=:0 glxinfo -B                        # renderer: llvmpipe (LLVM ...)
```
