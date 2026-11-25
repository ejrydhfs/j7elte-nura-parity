# v4.2 blob properties + CRTC `MODE_ID` — backport for the M0 atomic core (int84)

Status: **patch complete, compile-checked, and integrated into Image-int84; not
booted.** Patch:
`patches/drm-kms/0135-kms-blob-props-and-crtc-mode-id.patch`. Applies on top of
`drm-atomic/0001..0005` → `drm-kms/0104..0130/0133` → `0134`. No existing patch is
modified. The previous boot evidence is int83; this report adds the kernel pieces
KWin needs and the compile/apply proof, not a hardware run. Integration state is
in §9.

Why: `kwin_wayland` 6.8's `DrmCrtc::updateProperties()` (kwin
`src/backends/drm/drm_crtc.cpp:93-97`) treats a CRTC without `MODE_ID` and `ACTIVE`
as unusable and drops the CRTC, ending in
`Modeset failed: More outputs are enabled than there are CRTCs`. The M0 atomic
backport is v4.0-shaped, so `MODE_ID` and the blob ioctls (both v4.2) were absent.

## 1. Donor mapping (Linux v4.2)

The ticket named `drivers/gpu/drm/drm_property.c`, but in v4.2 the property/blob
code still lives in `drivers/gpu/drm/drm_crtc.c`; `drm_property.c` appears only in
v4.10+. Every hunk below is a straight port of the listed v4.2 hunk (the tree has
no `include/drm/drm_property.h`, `drm_modes.h`).

| v4.2 donor | Our tree | Notes |
|---|---|---|
| `include/drm/drm_crtc.h:217` `struct drm_property_blob` (`dev`, `kref refcount`, `head_global`, `head_file`, `size_t length`) | `include/drm/drm_crtc.h` `struct drm_property_blob` | replaces the 3.10 `head`/`unsigned int length` shape |
| `include/drm/drm_crtc.h:303` `crtc_state->mode_blob` | `struct drm_crtc_state` | |
| `include/drm/drm_crtc.h:1128` `mode_config->blob_lock` | `struct drm_mode_config` | |
| `include/drm/drm_crtc.h:1323,1404-1410` prototypes | same header | |
| `include/drm/drmP.h:331` `file->blobs` | `struct drm_file` | |
| `drm_crtc.c:4218` `drm_property_create_blob()` | `drm_crtc.c` | was a *static* 3.10 helper; now public |
| `drm_crtc.c:4267` `drm_property_free_blob()` | `drm_crtc.c` | new |
| `drm_crtc.c:4288` `drm_property_unreference_blob()` | `drm_crtc.c` | new |
| `drm_crtc.c:4316` `drm_property_unreference_blob_locked()` | `drm_crtc.c` | new (static) |
| `drm_crtc.c:4331` `drm_property_destroy_user_blobs()` | `drm_crtc.c` | new |
| `drm_crtc.c:4353` `drm_property_reference_blob()` | `drm_crtc.c` | new |
| `drm_crtc.c:4365` `__drm_property_lookup_blob()` | `drm_crtc.c` | new (static, `crtc_idr`) |
| `drm_crtc.c:4393` `drm_property_lookup_blob()` | `drm_crtc.c` | new |
| `drm_crtc.c:4501` `drm_mode_getblob_ioctl()` | `drm_crtc.c` | lookup under `blob_lock` (was lock-free `drm_mode_object_find`) |
| `drm_crtc.c:4550` `drm_mode_createblob_ioctl()` | `drm_crtc.c` | new |
| `drm_crtc.c:4599` `drm_mode_destroyblob_ioctl()` | `drm_crtc.c` | new |
| `drm_crtc.c:1459-1463` `"MODE_ID"` in `drm_mode_create_standard_properties()` | `drm_crtc.c` | `ATOMIC|BLOB`, 0 values |
| `drm_crtc.c:689-691` CRTC attach of `prop_mode_id` | `drm_crtc_init_with_planes()` | guarded by `DRIVER_ATOMIC && prop_mode_id`, like the existing `ACTIVE` attach |
| `drm_crtc.c:5844` cleanup drops remaining blobs | `drm_mode_config_cleanup()` | `head_global` + `unreference`, replaces the direct free |
| `drm_crtc.c:4180-4195` getproperty blob note | `drm_mode_getproperty_ioctl()` | 3.10 walked an always-empty `enum_blob_list`; now `count_enum_blobs = 0` |
| `drm_atomic.c:304` `drm_atomic_set_mode_for_crtc()` | `drm_atomic.c` | kernel-side mode set, creates the mode blob |
| `drm_atomic.c:354` `drm_atomic_set_mode_prop_for_crtc()` | `drm_atomic.c` | blob-side mode set: length check + `drm_mode_convert_umode()` into `state->mode` |
| `drm_atomic.c:402` `drm_atomic_crtc_set_property()` | `drm_atomic.c` | `prop_active` + `prop_mode_id` |
| `drm_atomic.c:435` `drm_atomic_crtc_get_property()` | `drm_atomic.c` | `prop_active` + `prop_mode_id` |
| `drm_atomic_helper.c:1618,1636` set_config uses the mode setter | `drm_atomic_helper_set_config()` | was direct `state->mode`/`enable` writes |
| `drm_atomic_helper.c:2055-2056,2078-2079,2122-2123` state hooks | `drm_atomic_helper_crtc_{reset,duplicate_state,destroy_state}()` | take/drop the `mode_blob` reference |
| `drm_modes.c:970-990` `drm_mode_validate_basic()` | `drm_modes.c` | new, used by `convert_umode` |
| `drm_modes.c:1417` `drm_mode_convert_to_umode()` | `drm_crtc.c` | was static `drm_crtc_convert_to_umode()`; kept next to its 3.10 callers |
| `drm_modes.c:1456` `drm_mode_convert_umode()` | `drm_crtc.c` | was static `drm_crtc_convert_umode()`; now validates |
| `drm_ioctl.c:629-630` ioctl table | `drm_drv.c` `drm_ioctls[]` | 3.10 keeps the table in `drm_drv.c` |
| `uapi/drm/drm.h:789-790` `0xBD`/`0xBE` | `include/uapi/drm/drm.h` | after `MODE_ATOMIC` (`0xBC`) |
| `uapi/drm/drm_mode.h:565-579` blob structs | `include/uapi/drm/drm_mode.h` | after `drm_mode_atomic` |
| `drm_fops.c:170,411` blob list init/destroy | `drm_open_helper()` / `drm_release()` | 3.10 has no `drm_drv.c` open path |

## 2. Touched surface

`git show --stat` of the patch (11 files, +588/−76):

```
 drivers/gpu/drm/drm_atomic.c        | 124 ++++++++++-
 drivers/gpu/drm/drm_atomic_helper.c |  17 +-
 drivers/gpu/drm/drm_crtc.c          | 420 ++++++++++++++++++++++++++++++------
 drivers/gpu/drm/drm_drv.c           |   2 +
 drivers/gpu/drm/drm_fops.c          |   5 +-
 drivers/gpu/drm/drm_modes.c         |  32 +++
 include/drm/drmP.h                  |   4 +
 include/drm/drm_atomic.h            |   5 +
 include/drm/drm_crtc.h              |  33 ++-
 include/uapi/drm/drm.h              |   2 +
 include/uapi/drm/drm_mode.h         |  20 ++
```

No new files, no Makefile/Kconfig change, no driver-visible ABI break: the only
signature change is `drm_property_create_blob(dev, size_t, const void *)`, which was
`static` in the 3.10 tree (sole callers updated: EDID path, `set_config`).

## 3. Behaviour added (what KWin actually uses)

1. `drm_mode_config_init()` creates `MODE_ID`; `drm_crtc_init_with_planes()` attaches
   it to every CRTC when `DRIVER_ATOMIC` is set. `MODE_ID` + `ACTIVE` therefore show
   up in `DRM_IOCTL_MODE_OBJ_GETPROPERTIES`, which is what
   `DrmCrtc::updateProperties()` walks; the CRTC is no longer dropped.
2. `DRM_IOCTL_MODE_CREATEPROPBLOB` copies a `drmModeModeInfo` (68 bytes) from
   userspace, returns a blob id, and links the blob to the file that created it;
   `DESTROYPROPBLOB` removes it (owner check, `-EPERM` otherwise), `GETPROPBLOB`
   reads it back. Blobs die with the file (`drm_release()`).
3. An atomic commit that sets `MODE_ID` to that blob resolves the mode:
   `drm_atomic_crtc_set_property()` → `drm_property_lookup_blob()` →
   `drm_atomic_set_mode_prop_for_crtc()` checks `length == sizeof(drm_mode_modeinfo)`,
   converts with `drm_mode_convert_umode()` into `state->mode`, sets `enable`, and
   holds a blob reference in `state->mode_blob`. The helper's
   `check_modeset()` then sees `mode_changed`/`enable` change and the driver's
   `exynos7580` DSI `mode_fixup()` accepts the mode, which is the same comparison
   the legacy `SETCRTC` path passes.
4. `ACTIVE` also works: the v4.0-shaped `drm_atomic_crtc_set_property()` fell through
   to `return -EINVAL` *after* storing `state->active`, and `get_property()` did not
   implement `ACTIVE` at all. Both are fixed (v4.2 body), otherwise every KWin
   commit (`ACTIVE=1`) would fail with `EINVAL` even with `MODE_ID` present.

Lifetime model (v4.2): `create_blob()` returns one reference on a blob;
`head_global` is the device list (no reference), `head_file` the creator's list (the
blob's reference is dropped by `destroyblob`/`drm_release`, not by the list);
`lookup_blob()` returns a new reference; `mode_blob` in CRTC states is
reference-counted by the helper hooks. All list/idr manipulation is under
`mode_config.blob_lock`; lock order is modeset locks → `blob_lock` → `idr_mutex`,
which matches `getblob`/`createblob` and the atomic set path.

## 4. Compile evidence

Scratch copy of the pristine tree, full series applied (`drm-atomic/0001..0005`,
`drm-kms/0001-core*..0003/0005`, `0100`, `0101`, `0201`, dsi/fence/plane/fbdev/dmabuf,
`0200`, `0102`, `0103`, `0104..0130`, `0133`, `0134`, then `0135`), KMS config
(`CONFIG_DRM=y`, `CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_EXYNOS7580=y`,
`# CONFIG_DRM_EXYNOS is not set`, fragments `display-owner-kms` + `shim-only`):

```
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS=-Wno-error -j8 drivers/gpu/drm/
rc=0
```

Log: `/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/blobwork-build.log`
(`gcc 16.2.0`, incremental from the pristine object tree). It compiles
`drm_crtc.o`, `drm_atomic.o`, `drm_atomic_helper.o`, `drm_modes.o`, `drm_drv.o`,
`drm_fops.o`, `drm_kms_helper.o`, `exynos7580/*` and links `drm.o`,
`drm_kms_helper.o`, `exynos-drm7580.o`; zero errors, zero warnings.

The int84-shaped module config was also built (`CONFIG_MODULES=y`,
`CONFIG_DRM_KMS_HELPER=m`, `CONFIG_DRM_EXYNOS7580=m`), same command, `rc=0`,
log `blobwork-build-mod.log`. The only diagnostics are two pre-existing
`-Wmissing-attributes` warnings on the KMS module's `init_module`/`cleanup_module`
(3.10 `linux/init.h` vs gcc 16), nothing from this patch. `make ... .ko` in that
scratch tree fails modpost because the builtin `drm.o` was never relinked into a
`vmlinux` there (the same error list contains pre-existing exports such as
`drm_atomic_state_alloc`, `drm_universal_plane_init`, `sys_copyarea`), so it is a
scratch-tree artefact, not a missing export. The symbols this patch shares across
module boundaries are in `__ksymtab` of `drivers/gpu/drm/built-in.o`
(`__ksymtab_drm_atomic_set_mode_for_crtc`, `__ksymtab_drm_atomic_set_mode_prop_for_crtc`,
`__ksymtab_drm_property_{create,lookup,reference,unreference}_blob`,
`__ksymtab_drm_mode_convert_to_umode`, `__ksymtab_drm_mode_convert_umode`,
`__ksymtab_drm_mode_validate_basic`) and `drm_kms_helper.o` names exactly the three
it needs (`drm_atomic_set_mode_for_crtc`, `drm_property_reference_blob`,
`drm_property_unreference_blob`). `drm_property_destroy_user_blobs()` is
deliberately unexported, as in v4.2 (only `drm_release()` calls it).

`CONFIG_DRM_EXYNOS` (the vendor 3.10 exynos driver, a legacy `drm_crtc_init()` user)
cannot be enabled on arm64 at all: its Kconfig `depends on DRM && (PLAT_SAMSUNG ||
ARCH_MULTIPLATFORM)`. It was therefore not compiled. Risk is bounded by grep: the
only users of blob internals in the whole tree are `drm_crtc.c`, `drm_sysfs.c`
(`edid_blob_ptr->data/length`, unchanged) and the new `drm_atomic.c` code;
`drivers/gpu/drm/exynos/` touches no blob field. Panfrost (drm-m1 series, not part
of this image) has no reference to `drm_property_blob` either; it only gains the new
`drm_crtc_state.mode_blob` member and uses the helper CRTC state hooks.

## 5. Apply check

```
# baseline = series 0001..0134 applied to the pristine tree
cd /tmp/patchcheck && git apply --check /tmp/0135.patch   # rc=0
```

The patch was generated from exactly that baseline (`git format-patch` of the
scratch diff), and the compile-checked tree is baseline+patch.

## 6. Expected `modetest -c` change

Before (int82/int83): the CRTC property list has `ACTIVE` only. After 0135 the CRTC
mockup gains one property block, emitted after `ACTIVE` (kernel attach order is
`ACTIVE` then `MODE_ID`):

```
 CRTCs:
 id     fb      pos     size
 18     0       (0,0)   (0x0)
   ACTIVE:
        id:     <n>
        value:  1
        flags:  atomic
+  MODE_ID:
+       id:     <n+1>
+       value:  0
+       flags:  atomic
```

`value` is the value in the core property store (0 until something sets it through
the property path); the blob itself is read with `DRM_IOCTL_MODE_GETPROPBLOB`. The
blob-id-carrying readback of `MODE_ID` via `OBJ_GETPROPERTIES` is the v4.2
`get_properties()` helper, which this patch deliberately does not port (see risks)
— KWin only needs the CRTC to expose the property. KWin's log then loses
`No mode_id or active property` / the CRTC-drop and
`Modeset failed: More outputs are enabled than there are CRTCs`.

## 7. Risks / limits

- **Not booted.** The mode-blob path is compile-proven only. The mode resolution is
  the same `drm_mode_convert_umode()` + `drm_mode_equal()` comparison the DSI
  `mode_fixup()` already accepts from the legacy path, but this needs the int84
  boot plus a KWin run to be called done.
- **`MODE_ID` readback through `OBJ_GETPROPERTIES` is 0, not the blob id.**
  Pre-existing 4.0-era behaviour (static property store). KWin's
  `updateProperties()` only checks presence; `modetest -c` prints 0. Porting v4.2's
  `get_properties()` would return the live state value and is a candidate follow-up,
  not required for KWin init.
- **Legacy `SETCRTC` is slightly stricter:** `drm_mode_convert_umode()` is now the
  public v4.2 function and rejects modes failing `drm_mode_validate_basic()`
  (clock 0, `hsync_start < hdisplay`, `htotal < hsync_end`, same for vertical).
  Previously the static 3.10 helper accepted them. Any userspace passing a
  nonsense mode to `DRM_IOCTL_MODE_SETCRTC` now gets `-EINVAL` at the ioctl instead
  of failing later in the driver.
- **Legacy `set_config` allocates a mode blob per call** (v4.2 behaviour). The blob
  is freed with the state; there is no leak, but the fbdev bring-up path now does
  one extra small `kzalloc` per mode set.
- **`drm_property_create_blob` length is the u32 from the ioctl** (v4.2 has no cap):
  a 4 GiB request fails in `kzalloc` with `-ENOMEM`.
- **Driver-subclassed CRTC states** must handle `mode_blob` in their own
  `atomic_duplicate_state`/`destroy_state`. No in-tree driver does; only
  `exynos7580` (helper hooks) and panfrost (helper hooks) are in scope.
- **`crtc->mode`/`crtc->enabled` are still not updated by the atomic commit**
  (`crtc_set_mode()` in this 3.10 helper never wrote them). Pre-existing, untouched
  here; `drmModeGetCrtc` on a KMS session keeps reporting the legacy field.
- **No per-plane/framebuffer blobs** were added (not needed by KWin); `/dev/fb0`
  and the page-flip path are unchanged except for the state-hook reference
  handling.

## 8. Series wiring for int84

`build-int84.sh` (from int83) needs one line after `0134`:

```sh
addp "$REPO"/patches/drm-kms/0135-kms-blob-props-and-crtc-mode-id.patch
```

int83's new-file hunk-count gate and `git apply --check` gate accept the patch
(no `+1,N` new-file hunks in it). Test plan for int84: boot
`exynos-drm7580-int84.ko`, `modetest -c` shows `MODE_ID` on the CRTC, then KWin
6.8 with `KWIN_DRM_DEBUG=1` must log `DrmCrtc` initialising instead of
`More outputs are enabled than there are CRTCs`; a MODE_ID-bearing atomic commit
must reach `exynos7580_crtc_mode_set()` with the panel timing.

## 9. int84 integration (G31, commit 4c5cce7)

`0135` is in Image-int84, applied after `0134` and before the drm-backport/driver
patches:

- patch order: `0104..0130` → `0133` → `0134` → **`0135`** → drm-backport
  `0001-0004` → `0005` wiring → mode-b → driver `0001/0002` → `0136`
- build: 5369 log lines, **0 errors**; `Image-int84`
  `2fee6bef69a48af930660656bd5b1d14c5d59f25753ed364c6f562bda8328ae9`,
  `exynos-drm7580-int84.ko`
  `8317703a256dcf3f8f89d51ea0c7356e44e67ad1230157b931909af2eb93cede`,
  `drm_kms_helper-int84.ko`
  `0781cf9d849fca0f00310b8a323a459f73a89cc8a3655992ff2a7521bed1cb31`
- blob gates all pass on the built tree: `drm_property_create_blob`/
  `lookup_blob`, `drm_atomic_set_mode_for_crtc`/`set_mode_prop_for_crtc`,
  `drm_mode_convert_umode`, `MODE_ID` created/attached,
  `CREATEPROPBLOB`/`DESTROYPROPBLOB`, `destroy_user_blobs` in `drm_release`
- int83 gates unchanged (two KMS `.ko`, imports, relocations, stock dt,
  pstore/lockup)

Not yet on hardware: the expected device observations (MODE_ID in `modetest -c`,
KWin `DrmCrtc` init) are still pending flash/boot; the int84 report is
`docs/reports/decon-kms-int84.md`.

