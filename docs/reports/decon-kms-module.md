# DECON KMS as a loadable module (int53)

int53 keeps the int52 source series and adds `patches/drm-kms/0102-kms-module.patch`,
so the DECON DRM/KMS driver can be brought up on a running device instead of
replacing the vendor display stack at boot.  The vendor DECON fbdev driver stays
built in and owns the panel from boot; a failed load costs one reboot, never a
battery pull.

## 1. Config (int53 vs int52)

| symbol | int52 | int53 | why |
|---|---|---|---|
| `CONFIG_MODULES` | not set | `=y` | the shipping config had no module loader at all; nothing can be `=m` without it |
| `CONFIG_MODULE_UNLOAD` | not set | `=y` | `rmmod` after a successful bind |
| `CONFIG_DRM_EXYNOS7580` | `=y` | `=m` | the KMS driver itself |
| `CONFIG_DRM_KMS_HELPER` | `=y` | `=m` | it is a promptless tristate selected by `DRM_EXYNOS7580`; it follows the selector down to `=m` |
| everything else | | unchanged | vendor DECON/DSIM/panel/IOVMM/DRM core all stay built in |

`CONFIG_DRM_EXYNOS7580` is now `tristate` (`0102`); the `EXYNOS7580_DISPLAY_KMS`
choice still gates it and the KMS fragment pins the choice.  `EXYNOS7580_DISPLAY_KMS`
still says `depends on DRM=y`, and `DRM` stays `=y`, so the DRM core is still in
the Image and only the helper and our driver are loadable.

Consequence: the loadable set is exactly **two** objects, and they must be
inserted in this order (our `.modinfo` says `depends: drm_kms_helper`):

1. `drm_kms_helper-int53.ko`
2. `exynos-drm7580-int53.ko`

No other `.ko` is produced by the build (`find drivers -name '*.ko'` = 2).

## 2. Artifacts (external disk, `$EXT = /run/media/asdf/…/j7k/gpu31`)

| file | size | sha256 |
|---|---|---|
| `Image-int53` | 18058856 | `cd51433661217a98ac639eb62fd93bf6dfa2e77868dc02a74b83e24a99ad5b54` |
| `kernel-boot-int53.img` | 27197440 | `54b491f9ab6dbbc6f7fe46ddfd6300a61cd08b67b83e28e4e9f537094242ec5d` |
| `exynos-drm7580-int53.ko` | 1425208 | `b05f09741cc693d39c5fe3245081dcdf2e7c1b187b72466d8b8e04517c373ae5` |
| `drm_kms_helper-int53.ko` | 1439512 | `d9007caa7b2806c7b3f2c944b4d7858f582624aa26282f9f058591e061db81b3` |

`exynos-drm7580.ko` lives at `drivers/gpu/drm/exynos7580/` in the build tree and
`drm_kms_helper.ko` at `drivers/gpu/drm/`; both are copied next to the boot image
because the device has no modules directory.  Transfer both to e.g.
`/home/user` on the phone before testing.

`modinfo exynos-drm7580.ko`:

```
license:        GPL
description:    Samsung Exynos 7580 DECON DRM/KMS
author:         gpu31-kms
alias:          of:N*T*Csamsung,exynos5-decon_driver*
depends:        drm_kms_helper
intree:         Y
vermagic:       3.10.108 SMP preempt mod_unload aarch64
parm:           fbdev:Register /dev/fb0 on the DECON KMS device (default: 1) (bool)
parm:           fbdev_bpp:Preferred fb0 bits per pixel: 32, 24, 16, 15 or 8 (default: 32) (int)
```

The `alias` line exists because `MODULE_DEVICE_TABLE(of, …)` is required for a
manual `modprobe`; it does **not** auto-load anything on this device: there is no
`/lib/modules`, no `modules.alias`/`modules.dep` generated, `CONFIG_MODULE_SIG`
is off and the device tree is stock.  `/etc/modules` is untouched.

## 3. Load procedure (device, root)

The vendor fb0 and SSH must be up first — that is the always-recoverable state.

```
systemctl stop lightdm            # or kill Xorg; required: no fb0 client
echo 14830000.decon_fb > /sys/bus/platform/drivers/decon/unbind
insmod /home/user/drm_kms_helper.ko
insmod /home/user/exynos-drm7580.ko
dmesg | tail -50                  # "DECON handed to KMS", "exynos7580: ... bound"
ls /dev/dri/                      # card0
modetest -M exynos               # if the test package is present
```

Step 2 is what makes the load possible: `decon_kms_get()` refuses to take the
DECON when the vendor driver owns it (`-EBUSY`), and after `unbind` the platform
device is free.  `0102` also fixes the vendor `decon_remove()` so this is safe:

* it now clears the platform drvdata (stock `decon_remove()` frees the
  `decon_device` and leaves `platform_get_drvdata()` dangling, which the KMS
  probe would dereference),
* clears the exported `decon_int_drvdata` if it still points at that device,
* unregisters the V4L2 subdev before the struct is freed.

Failure modes are cheap and visible:

* **`insmod: ERROR: could not insert module: Unknown symbol …`** — a symbol that
  is not exported.  `logs/int53-imports.txt` is the complete import list (130
  symbols) and the gates prove each one is in `Module.symvers`; if the device
  kernel differs from int53 the loader prints the missing name, no crash.
* **probe error in dmesg** (`DECON handle unavailable (-N)`, `drm_platform_init
  failed`) — the probe unwinds and returns an error; the device stays unbound,
  the panel is dark, reboot restores the vendor stack.
* **panic/hang after the second `insmod`** — first-time bring-up of the real
  hardware path; reboot returns to vendor fb0.

If only the vendor driver was unbound and nothing else was loaded, rebinding is
`echo 14830000.decon_fb > /sys/bus/platform/drivers/decon/bind` (the vendor
driver still is built in and its remove path is now re-bindable).

## 4. `rmmod` after a successful bind

`rmmod exynos-drm7580` is safe **when no DRM client holds card0** (no X/Plasma,
no `modetest` running; an open DRM fd pins the module and `rmmod` returns
`EBUSY`, which is the safe failure):

* `module_exit` → `platform_driver_unregister()` → `exynos7580_remove()` →
  `drm_platform_exit()` → `drm_put_dev()` → the driver's `->unload()`, which
  tears down fbdev (`drm_fbdev_cma_fini`), vblank, connector/CRTC/planes and
  then `decon_kms_put()`;
* `decon_kms_put()` (the same body the built-in probe failure path uses) stops
  the update/vsync threads, destroys the workqueue, disables the pipe, releases
  the media entity, drops pm_runtime and the IOVMM domain, and clears the
  drvdata — the DECON is then free for the vendor driver again;
* after `rmmod` the device is NOT re-bound automatically: the display stays
  dark until a manual `bind` (above) or a reboot.  Do not `rmmod` while a
  client is active.

## 5. Exports the module needs (added by 0102)

`decon_kms_*` were already `EXPORT_SYMBOL_GPL` in `0100-decon-export-shim.patch`.
The only imports modpost could not resolve against the built-in tree were the
vendor IOVMM pair used by the dmabuf slice:

* `iovmm_map()` — `drivers/iommu/exynos-iovmm-v6.c`
* `iovmm_unmap()` — same file

`0102` adds `EXPORT_SYMBOL_GPL` for both next to the definitions (and
`#include <linux/export.h>`).  Nothing else in the vendor stack is static-only
for our paths: the module's other 128 imports are DRM core
(`drm_*`, `drm_gem_cma_*`, `drm_atomic_helper_*`, built in because `DRM=y`),
`drm_fbdev_cma_*`/`drm_fb_cma_*` from `drm_kms_helper.ko`, standard kernel
exports (`kmalloc`, `mutex_*`, `of_*`, `platform_*`, `dev_*`, `printk`,
`registered_fb`) and the nine `decon_kms_*` entry points.  The complete list is
`logs/int53-imports.txt`; the helper module's 119 imports are in
`logs/int53-imports-helper.txt` (all resolved, `modpost` clean).

## 6. Gates and logs

`build-int53.sh` = int52 series + `0102`, config as above, `make … Image modules`
(-j4, pristine copy, stock `dt.img`, scratch deleted).  Result: 5346 build-log
lines, 488 warnings, **0 errors**, **0 `undefined!`**, no `WARNING: modpost`,
no warnings in `drivers/gpu/drm/exynos7580/`; 8 gate groups PASS, including:

* `.config` closure (`CONFIG_MODULES=y`, `CONFIG_MODULE_UNLOAD=y`,
  `EXYNOS7580_DISPLAY_KMS=y`, `DRM_EXYNOS7580=m`, `DRM_KMS_HELPER=m`, vendor
  display `=y`);
* exactly two `.ko` files, ordered dependency `drm_kms_helper`;
* every KMS source symbol defined once in the `.ko` and absent from vmlinux
  (`no leak into the Image`), `card0 ready (atomic)` string only in the `.ko`;
* all 130 + 119 imports present in `Module.symvers`;
* `decon_kms_get` present in `vmlinux` `__ksymtab`, all `decon_kms_*` exports in
  `Module.symvers`;
* GPL license, `aarch64` vermagic, stock-dt boot image.

Log sha256s:

* `logs/int53-kms-build.log` — `3275164b3cec29ecdc5f402bceffd80985bf88501f1fadc77981d413dc291bbb`
* `logs/int53-gates.log` — `8afd6280b46a234da28b5f83ffe02471aa0655f9558888815faa13b615849e40`
* `logs/int53-kms-evidence.log` — `ed908a4eec97776ff23bd5e4fc6feebd9a1cdb03cea9391a91f9eda5089dfd11`
* `logs/int53-kms-config.log` — `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`
