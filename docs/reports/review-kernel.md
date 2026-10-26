# Kernel patches and final build — verification (re-check after D1–D3 fixes)

Scope: the four parity kernel patches (`bluetooth/0001`, `camera/0001`,
`drm/0001`, `drm/0002`), the DRM config fragment, the `linux-samsung-j7elte`
package, `parity/verify.sh`, `parity/build-all.log` and `parity/final-out/`.
Static/build verification only — no hardware, no boot.

Snapshot: 2026-10-24 22:37 EST. Peers are concurrently editing `parity/pmaports`
and `parity/patches`; counts below are for that snapshot.

Verdict: **D1, D2 and D3 are fixed and verified.** The four patches, the package
patch set, the fragment and the shipped `final-out` artifacts are consistent.
One tooling defect remains: `verify.sh` reports a false-negative `FAIL` on a
patch that depends on an earlier patch of the series (D4). No false routing.
The DRM stack is now correctly opt-in; the prerequisite is verified in §3.3.

| # | Check | Result |
|---|---|---|
| 1 | Four kernel patches apply to clean `b5e363a` (+ pmaports package patches) | **PASS** |
| 1b | `linux-samsung-j7elte` package: all 9 `source=` patches apply via abuild semantics | **PASS** (BT/camera/sun included) |
| 1c | pmaports series applies (sequentially, dependency order) | **PASS** |
| 2 | `Image` + 3 DTBs, DRM objects, DT compatible, camera fallback, BT gpc4-2 | **PASS** (unchanged) |
| 2b | No new build errors | **PASS** (0 errors; 5 new warnings, pre-existing code) |
| 3 | `build.sh` default config reproduces `final-out/.config` | **PASS** (D1 fixed) |
| 3b | DRM opt-in caveat (fragment without DRM patches) | documented/verified, see §3.3 |
| 4 | `verify.sh` | **PARTIAL**: 21 OK + 1 SKIP + 1 FAIL, exit 1 — the FAIL is a false negative (D4) |

Paths are relative to
`S=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session`.

## 1. Patch application

### 1.1 Kernel side

Clean-tree test: `git archive b5e363a` for every touched path into a scratch
tree, `git init` + commit, then apply in order. Results:

* the seven original package patches (gcc8, gcc10, linux3.7/sun, fix-vmm,
  create-platform.h, fix-argb, samsung-usb-ethaddr) all apply. `gcc8-…` needs
  `patch -p1` (offset 6, abuild behaviour); `git apply` alone refuses it because
  its hunk header says line 162 while the baseline has the line at 168. It only
  touches 32-bit `arch/arm/include/asm/uaccess.h`, so it cannot affect the arm64
  build.
* the four reviewed patches apply in sequence with `git apply`:
  `bluetooth/0001`, `camera/0001`, `drm/0001`, `drm/0002` — all OK.
* the new `userspace/gpu-panfrost/0001-arm64-dts-exynos7580-add-mali-t720-compatible.patch`
  also applies on top (`arch/arm64/boot/dts/exynos7580.dtsi`, disjoint from the
  reviewed files).

After applying the four reviewed patches + the package patches, every
**arm64-relevant** file is byte-identical to `parity/final-kernel-check`
(gpio 00/04/06, rev 00/04/06, `arch/arm64/mm/proc.S`, camera source,
`drivers/gpu/drm/{Kconfig,Makefile,drm_gem_cma_helper.c}`, the whole
`drivers/gpu/drm/exynos7580/` directory, `init/_vmm.S`, dtc lexer,
`decon-int_drv.c`, `u_ether.c`). `final-kernel-check` only lacks the gcc8 hunk
(irrelevant on arm64), so the recorded build inputs are reproducible.

Method used for the abuild check (same order as `source=` in the APKBUILD):

```
0001-j7elte-bt-pinctrl.patch                                  git apply OK
0001-fimc-is2-fallback-to-request_firmware-…patch             git apply OK
gcc8-fix-put-user.patch                                       patch -p1 OK (offset 6)
gcc10-extern_YYLOC_global_declaration.patch                   git apply OK
linux3.7to4.5-ARM64-remove-Sun-Solaris-style-flag-on-section.patch  git apply OK
fix-vmm-blob-include.patch                                    git apply OK
create-a-file-platform.h-which-is-a-symlink.patch             git apply OK
fix-argb.patch                                                git apply OK
samsung-usb-ethaddr.patch                                     git apply OK
```

The BT and camera patch copies inside the package are byte-identical to
`parity/patches/bluetooth/0001-…` and `parity/patches/camera/0001-…`
(sha512 `a5d09aa5…` / `767b7afd…`), and the APKBUILD `sha512sums` match the
actual files (also `4351f862…` config, `75545ccd…` sun patch).

### 1.2 pmaports series

`parity/patches/pmaports/0005-linux-samsung-j7elte-config-and-parity-patches.patch`
replaces the old config-sha-only patch and applies to baseline `ba682bd`; it
adds the BT + camera patch files, updates `source=`/`sha512sums`, and replaces
the broken sun-patch file with a real 288-byte patch (`arch/arm64/mm/proc.S`,
removes `.section ".text.init", #alloc, #execinstr`; sha512 `75545ccd…`, which
matches the APKBUILD entry and the `proc.S` hunk in `final-kernel-check`).
The old `0005-linux-samsung-j7elte-config-sha512.patch` is gone.

The 15 pmaports-tree patches that existed at the time of the original review all
still apply. The series has since grown (audio/0003, review-a/0001; audio/0004
was folded into `userspace/0001-device-deps.patch`, which now adds
`j7elte-audio-jack`). Current snapshot: 23 patch files = 5 kernel + 17 pmaports
+ 1 source-tree skip.

Sequential test on a clean `ba682bd` archive (`git init` + commit, apply in
dependency order) — **all 17 pmaports patches apply**:

```
pmaports/0003  pmaports/0005  userspace/0001  bluetooth/0002  wifi/0001
audio/0002  audio/0003  battery/0001  camera/0001  connectivity/0001
connectivity/0002  fde/0001  gps/0001  gpu/0001  ril/0001  sensors/0001
review-a/0001
```

The result reproduces the working tree exactly for
`device/archived/linux-samsung-j7elte/` (including the two new patch files and
all hashes) and for `device/archived/device-samsung-j7elte/APKBUILD` (`diff -r`
clean). `review-a/0001` must come **after** `sensors/0001` (it edits
`j7elte-sensors/dbus-wire.c`/`APKBUILD`, which `sensors/0001` creates). Note
that the working tree also contains an untracked `j7elte-ril/` package from the
concurrent RIL work that has no exported patch yet; it is outside this series.

`git apply` warns about 45 whitespace errors inside the nested patch text of
`0005` (trailing whitespace / space-before-tab in the embedded patch copies).
Cosmetic only; the patch applies cleanly.

## 2. Final build (`parity/final-out`, `parity/build-all.log`) — unchanged, re-checked

* `arch/arm64/boot/Image`, 17,545,608 B, arm64, `Linux version 3.10.108 … #1 SMP
  PREEMPT Sat Oct 24 21:49:39 EST 2026`.
* exactly three DTBs: rev00 102,379 B, rev04 102,740 B, rev06 102,837 B
  (log has exactly three `DTC …j7elte_rev0{0,4,6}.dtb` lines).
* `drivers/gpu/drm/exynos7580/{exynos7580_drm,exynos7580_decon,exynos7580_dsim,
  exynos7580-drm,built-in}.o` built and linked (`nm` has
  `exynos7580_drm_driver`, `exynos7580_decon_*`, `exynos7580_dsim_*`); vendor
  `drivers/video/exynos/decon_7580/` is not built.
* all three DTBs and the Image contain `samsung,exynos7580-decon-drm` and
  `samsung,exynos7580-dsim`; the driver matches the first (`exynos7580_drm.c:1309`)
  and looks the second up with `of_find_compatible_node` (`:194`).
* camera fallback string present in Image; cross-objdump of
  `fimc_is_sec_fw_sel_eeprom` (`0xffffffc0007bf3a4`) shows
  `bl <filp_open>` `+0xf8`, `bl <request_firmware>` `+0x128`,
  `bl <release_firmware>` `+0x2f0`, normal `bl <vfs_read>` `+0x3cc`.
  `FIMC_IS_HEADER_VER_SIZE=11`; the `fw_size > 11` guard makes `fw_size-11`
  non-negative and the 11-byte `strncpy` fits the 12-byte buffers.
* BT: all three DTBs have a `bt-en` group on `gpc4-2` (func 1, pud 0, con-pdn 3,
  pud-pdn 0) and the `bluetooth` node's `pinctrl-0` third phandle references it
  (rev00 `<0x93 0x94 0x95>`, rev04 `<0x94 0x95 0x96>`, rev06 `<0x95 0x96 0x97>`),
  matching the ROM DT. rev04/06 also carry the unreferenced shared
  `ese-cs-func` gpc4-2 group from `exynos7580-pinctrl.dtsi`; no active conflict.
* `build-all.log`: 0 errors, ends with `SYSMAP System.map` / `OBJCOPY
  arch/arm64/boot/Image`. 415 warnings vs 412 baseline = 5 new / 2 dropped:
  2 pre-existing `drm_gem_cma_helper.c:274` format warnings (untouched line),
  2 `hdmi.c` strncpy warnings (newly compiled via `CONFIG_HDMI=y`), 1 kconfig
  host-tool warning; the 2 dropped are vendor DECON/DSIM code no longer built.
  No warning from the new driver.

## 3. Config

### 3.1 D1 fixed — default build reproduces the final image config

`parity/build.sh` now defaults
`CFG=$S/parity/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`
(sha512 `4351f862…`), and the fragment header says to apply on top of that
config. Reproduced with exactly those inputs on the patched tree
(`olddefconfig`):

```
cmp scratch/.config parity/final-out/.config   → identical
sha512 070ceb9ca4c4834f27cb265506307f5579f805d278a153c8895122158d5a91c65f8c436ced0ba9c1bee607cb7c2ce5e023a1461e62c90e8b99502d1e766cf357
CONFIG_BT_HCIUART=y  CONFIG_BCMDHD_FW_PATH="/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin"
CONFIG_DRM_EXYNOS7580=y  CONFIG_CMA_SIZE_MBYTES=32
```

`parity/final-config` is the pre-olddefconfig input; running `olddefconfig` on
it also yields the same file, so the whole config chain is consistent.

Expected fragment values are all present: `DRM`, `DRM_KMS_HELPER`,
`DRM_KMS_CMA_HELPER`, `DRM_EXYNOS7580`, `FRAMEBUFFER_CONSOLE` = `y`,
`CMA_SIZE_MBYTES=32`, `FW_LOADER(_USER_HELPER)=y`, and vendor
`EXYNOS_DECON_EXYNOS7580` / `EXYNOS_MIPI_DSI` off, `EXYNOS_DECON_LCD` /
`PANEL_EA8061_DYNAMIC` absent, `DRM_EXYNOS` off. olddefconfig logs
“override: reassigning to symbol EXYNOS_MIPI_DSI / EXYNOS_DECON_LCD /
PANEL_EA8061_DYNAMIC”; expected (baseline `y`, fragment appends `is not set`).

### 3.2 D2 fixed — package layout

`linux-samsung-j7elte/APKBUILD` now lists `0001-j7elte-bt-pinctrl.patch` and
`0001-fimc-is2-fallback-…patch` in `source=` with matching `sha512sums`; both
files are in the package directory; `default_prepare` therefore applies them
(verified with abuild-style `patch -p1` in §1.1). The fixed 288-byte sun patch
is also applied by `default_prepare`. Default package config is the unchanged
pmaports config: `# CONFIG_DRM is not set`, `CONFIG_EXYNOS_DECON_EXYNOS7580=y`,
`CONFIG_EXYNOS_MIPI_DSI=y`, `CONFIG_PANEL_EA8061_DYNAMIC=y` — the default
`linux-samsung-j7elte` apk keeps the vendor fbdev display stack, and the BT +
camera patches are display-neutral.

The two DRM patches are deliberately **not** in the default package; DRM is
opt-in via `parity/patches/drm/*` + `config-drm-exynos7580.fragment`.

### 3.3 DRM opt-in caveat (verified)

The fragment must only be used on a kernel tree that already contains
`drm/0001` and `drm/0002`. I applied the fragment to a tree **without** the DRM
patches and ran `olddefconfig`:

```
CONFIG_DRM=y                       (DRM core exists in the baseline)
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_CMA_SIZE_MBYTES=32
# CONFIG_EXYNOS_MIPI_DSI is not set
# CONFIG_EXYNOS_DECON_EXYNOS7580 is not set
CONFIG_DRM_EXYNOS7580              → symbol unknown, silently dropped
```

i.e. the fragment alone disables the vendor display stack while the replacement
`DRM_EXYNOS7580` is not in the tree yet → **no display driver at all**. With the
two DRM patches applied first, `CONFIG_DRM_EXYNOS7580=y` is retained and the
vendor stack stays off (this is exactly `final-out/.config`). Also,
`parity/build.sh` does not apply patches to `SRC`; the caller must use a tree
with `drm/0001` + `drm/0002` applied (e.g. `final-kernel-check`, or the pmaports
package tree with the two patches added manually).

Suggested (non-applied) documentation diff for the fragment header:

```diff
-# Apply on top of the pmaports config-samsung-j7elte.aarch64, then run olddefconfig.
+# Apply on top of the pmaports config-samsung-j7elte.aarch64, then run
+# olddefconfig.  The kernel tree MUST already contain parity/patches/drm/0001
+# and 0002: without them this fragment disables the vendor fbdev stack while
+# DRM_EXYNOS7580 does not exist, leaving the device with no display driver.
```

## 4. `verify.sh` — one false negative (D4)

Snapshot output (23 patches): **21 OK + 1 SKIP + 1 FAIL, exit 1**.

* routing is still correct for every patch: 5 kernel patches → kernel tree,
  17 `device/archived/…` patches → pmaports, 1 source-tree patch skipped
  (`gps/0002`, target `parity/userspace/gps/Makefile`). No false routing.
* the single FAIL is a **false negative**:
  `userspace/review-a/0001-j7elte-sensors-ignore-unknown-dbus-header-fields.patch`
  reports “`device/archived/j7elte-sensors/dbus-wire.c: does not exist in
  index`”. That file is created by `userspace/sensors/0001`, which sorts after
  `review-a` and is checked independently against the baseline index. Applied
  sequentially (sensors then review-a) the patch applies cleanly (§1.2).
* the same independent-check limitation would hide any future dependency; the
  earlier `audio/0004` failure was of the same kind (since folded into
  `userspace/0001`).

Together with the four reviewed kernel patches, the 15 pmaports-tree patches
that existed at the original review still yield the previously reported
19 OK + 1 SKIP; the current 23-patch tree cannot, because `verify.sh` assumes
patches are order-independent.

Proposed fix (not applied): check cumulatively in dependency order. Minimal
version that keeps the shared worktree/index untouched — use a temporary index
and iterate until no new patch applies:

```sh
# per routed tree:
tmpidx=$(mktemp); export GIT_INDEX_FILE=$tmpidx
git -C "$tree" read-tree HEAD
# remaining = patches routed to this tree, in preference order
while [ -n "$remaining" ] && [ $progress -eq 1 ]; do
    progress=0
    for p in $remaining; do
        if git -C "$tree" apply --cached "$p" 2>/dev/null; then
            echo "OK   ..."; remaining=$(echo "$remaining" | grep -vxF "$p"); progress=1
        fi
    done
done
# anything left is a real failure
unset GIT_INDEX_FILE
```

(`git apply --cached` writes blobs into the shared object store; a stricter
variant is to copy `git archive HEAD` into a temp dir, `git init` there and
apply in the temp repo — that is what I used for §1). Also note that patch
*dependencies* are real: a sorted `find` is not an ordering, so the retry loop
(or an explicit order file) is required, not just cumulative staging.

## 5. Defect status

| ID | Defect | Status |
|---|---|---|
| D1 | `build.sh` default config was the stale `config-nura-j7elte` | **fixed**, verified: default now reproduces `final-out/.config` byte-for-byte |
| D2 | kernel package did not ship the parity kernel patches | **fixed**: BT + camera + real sun patch in `source=`/`sha512sums`, applied by `default_prepare`; DRM kept opt-in with the caveat in §3.3 |
| D3 | sun patch file was a 94-byte path string, breaking `default_prepare` | **fixed**: real 288-byte patch, sha512 `75545ccd…` matches APKBUILD and `final-kernel-check` |
| D4 | `verify.sh` independent-index check reports a false FAIL for dependent patches (`review-a/0001`; previously `audio/0004`) | **open** — patch proposed in §4; current output 21 OK + 1 SKIP + 1 FAIL, exit 1 |
| — | `final-out` artifacts / four patches | **PASS**, unchanged, no new build errors |

## 6. Residual risk / not verified

* No hardware, no boot, no runtime: DRM probe/panel, camera `request_firmware`
  and BT power sequencing are only proven linked/compiled.
* No full `abuild`/`pmbootstrap` run (no srcdir/network); evidence is
  abuild-equivalent patch application, recorded `build-all.log`, and exact
  config/artifact reproduction.
* `drm/0001` is a 3,112-line new driver; only build/link/DT-binding checks were
  made, not register programming.
* pmaports is a moving target (RIL package untracked, peer edits during the
  check); counts in §4 are for the stated snapshot.

## 7. Reproduce

```sh
S=/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session
sh $S/parity/verify.sh                 # snapshot: 21 OK + 1 SKIP + 1 FAIL (review-a)
# config (needs the patched tree):
mkdir -p /var/tmp/cfg && cp $S/parity/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 /var/tmp/cfg/.config
cat $S/parity/patches/drm/config-drm-exynos7580.fragment >> /var/tmp/cfg/.config
make -s -C $S/parity/final-kernel-check O=/var/tmp/cfg ARCH=arm64 olddefconfig
cmp /var/tmp/cfg/.config $S/parity/final-out/.config       # identical
# sequential pmaports series (baseline ba682bd, apply sensors before review-a):
git -C $S/parity/pmaports archive ba682bd | tar -x -C /var/tmp/pm && cd /var/tmp/pm
git init -q && git add -A && git -c user.name=r -c user.email=r@r commit -qm base
# then: git apply $S/parity/patches/... in the order listed in §1.2
# artifacts:
strings -a $S/parity/final-out/arch/arm64/boot/Image | grep -E 'exynos7580-decon-drm|trying request_firmware'
$S/toolchains/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-objdump \
  -d $S/parity/final-out/vmlinux --start-address=0xffffffc0007bf3a4 --stop-address=0xffffffc0007bffa4 | grep request_firmware
```
