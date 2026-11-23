# int64: loadable DRM modules without out-of-range relocation types

## The failure

`insmod drm_kms_helper.ko` failed with

```
module drm_kms_helper: overflow in relocation type 261 val ffffffbffc000000
```

`261` is `R_AARCH64_PREL32`.  In both modules every one of them lives in
`.rela.eh_frame`:

| module | `R_AARCH64_PREL32` | section |
|---|---|---|
| `drm_kms_helper.ko` | 116 | `.rela.eh_frame` (all) |
| `exynos-drm7580.ko` | 56 | `.rela.eh_frame` (all) |

GCC emits unwind tables by default, so `.eh_frame` carries PC-relative 32-bit
self-references.  Built-in code never sees this (the linker resolves
`.eh_frame` at link time); this vendor tree never built modules
(`CONFIG_MODULES` was unset), so `arch/arm64/kernel/module.c` was never
exercised on them.  That loader applies the relocation as
`val = st_value + addend` and then `val - (section_addr + offset)`, with
`section_addr` at `MODULES_VADDR` — the result is far outside the signed
32-bit range, hence the overflow.  3.10 has no `CONFIG_ARM64_MODULE_PLTS` to
switch on.

## The fix (`0106-kms-module-no-ehframe.patch`)

Compile the loadable DRM objects without unwind tables:
`-fno-asynchronous-unwind-tables -fno-unwind-tables`

* `drivers/gpu/drm/exynos7580/Makefile` — `ccflags-y` (this directory is only
  ever a module in this series),
* `drivers/gpu/drm/Makefile` — per-object `CFLAGS_*` for the
  `drm_kms_helper` compound objects, guarded by
  `ifeq ($(CONFIG_DRM_KMS_HELPER),m)` so a built-in helper keeps the stock
  code generation.

Verified at object level before the full build: `drm_atomic_helper.o`
recompiled with the flags has **no `.eh_frame` and no `R_AARCH64_PREL32`**.

`-mcmodel=large` was not needed: the failing relocation is the `.eh_frame`
PREL32, not an `ADRP` range problem.  The remaining relocation types in the
final modules are all handled by this kernel's loader:

```
exynos-drm7580.ko:  257 258 275 277 278 283 284 285 286
drm_kms_helper.ko:  257 258 275 277 278 283 285 286
```

(`257` ABS64, `258` ABS32, `275` ADR_PREL_PG_HI21, `277` ADD_ABS_LO12_NC,
`278` LDST8, `283` CALL26, `284` LDST16, `285` LDST32, `286` LDST64 — all in
the `module.c` switch.)  A numeric gate decodes the type from `r_info`
(readelf truncates long names in this binutils) and rejects anything outside
the supported list, so a future out-of-range relocation is caught at build
time.  `readelf -r` shows `PREL32=0` and `readelf -S` shows no `.eh_frame` in
either `.ko`.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int64` | 18058984 | `5d0b5dc41e0b2d50e402746457aa15cd2856658f12c0fb5800eb96cdd3a07f69` |
| `kernel-boot-int64.img` | 27197440 | `824e0aab594ae6af0f75f804478514dd0a6d89293036884c7fcee02a44e18fad` |
| `exynos-drm7580-int64.ko` | 1430120 | `140eb5cd35b6cf5eb7762cf9b8fc6e353440760b899b95aa6dbf53714ce5d3d1` |
| `drm_kms_helper-int64.ko` | 1443368 | `6e5315f065415876ca9d8c3d2f2a19e404385fadc347af9056a4237d2625a47c` |

The Image is code-identical to int63's (same size 18058984; the flags only
affect module objects).  Load order unchanged: helper first, then
`exynos-drm7580.ko`.  Build 5346 lines, 0 errors, 0 `undefined!`, modpost
clean; 130 KMS + 119 helper imports all in `Module.symvers`.

## Gates (all PASS)

Everything int63 gates, plus the relocation gate above.  Vendor refusal
(0103, incl. the objdump check on `decon_probe`), drvdata order (0105), no
KMS symbols/initcalls in `vmlinux`, module symbols and probe strings in the
`.ko`, `LOG_BUF_SHIFT=18` + single-line `ignore_loglevel` cmdline, pstore,
minimal vendor remove, stock-dt boot.

## If `insmod` still fails

The next failure can only be a different loader complaint; `dmesg` will say
which.  If it is another unsupported relocation, the readelf gate will already
show it (it currently does not).  If it is `Unknown symbol`, the import gate
covers all 130/119 names against `Module.symvers`, so the device kernel would
have to differ from int64's Image.

Built-in fallback (not built, per instruction): disable
`CONFIG_EXYNOS_SNAPSHOT_PANIC_REBOOT` so a panic hangs instead of rebooting,
keep `PSTORE_CONSOLE`, and investigate why `ess_pstore_init`'s ramoops is
empty on this boot image (it registers the bootloader `log_pstore` region only
if `exynos_ss_get_item_paddr("log_pstore")`/size are present; if the current
bootloader/boot.img does not publish that item, pstore stays empty no matter
what the kernel does).

Log sha256s: build `bca6966288876c7cba437f7db741f5dfcbfd22e621523004c5b01c1521539f66`, gates `8233ff3939d9088a7dfae2c2ef54a09316ddb9ffc5efe1598f7b01471cb64977`, evidence `6ceb5578e6b3f864fc9fbc0c55ad28b4244aed9aeda0e492cd34101d4bb8a69a`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.
