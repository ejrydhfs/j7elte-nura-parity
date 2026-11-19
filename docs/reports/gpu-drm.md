# GPU DRM milestones M1–M3 — j7elte (Exynos 7580 / Mali-T720 / Nura 3.10.108)

Status: **M1 build+boot in progress; M2/M3 are engineered designs with
hardware-gated evidence.** This report records config/patch diffs, the exact
flush procedure, and hard evidence per milestone. Anything not yet run on
hardware is marked `[PENDING-HW]`. A sibling RIL session currently owns a
`pmu-cp.c` change (`cp_ctrl_normalize`, in the shared tree at M1 build time);
GPS driver/config deltas vs pmaports are **zero** (verified by grep-diff,
§0) — GPS stays at stock parity.

Related: `docs/reports/gpu-verify.md` (§3 baselines), `drm-driver.md`
(display-driver design), `gpu-mesa-kbase.md` (kbase winsys + Mesa backend),
`gpu-panfrost.md` (backport audit), `gpu-kbase.md`, `gpu-userspace.md`.

Slice inputs (all committed): `patches/drm-m1/` (M1 fragment+README),
`code/kernel/m1/` (closure check + verify), `code/kernel/recovery/`
(RECOVERY.md + readiness), `docs/scratch/panfrost-backport-inventory.md`
(v5.2 target), `patches/drm-backport/` (fence/resv/syncobj/sched shims),
`patches/iommu-dma/`, `patches/drm-kms-coexist/DESIGN.md`,
`code/userspace/gpu/panfrost-validate/`, `mesa-render/`, `glmark-harness/`,
`dvfs-power/`.

## §0. Invariants (hold for every milestone)

- DT payload rule: **never touch `dt.img`** (S-Boot hangs on rebuilt DTBs;
  `docs/HARDWARE-BRINGUP-2026-11-16.md` §16). All milestone builds reuse
  `/tmp/bimg/dt.img` + `ramdisk-new.gz` + cmdline via `/tmp/mkboot.py`.
- BOOT = `/dev/block/mmcblk0p10` (32 MiB). Flash: `dd … conv=fsync` + `sync`
  **before** any reboot; sysrq-b does not sync (blobs were lost once).
  Full sequence + TWRP recovery in `code/kernel/recovery/RECOVERY.md`.
- Known-good fallback: `/tmp/bindboot.img` (26 374 144 B,
  sha256 `fcec651e…f038`; `code/kernel/recovery/known-good.sha256`).
- GPS parity: `diff` of `CONFIG_GPS*/CONFIG_MALI*` between the pmaports
  config and the build tree `.config` is **empty** (M1 build time).

## M1 — CONFIG_MODULES + CONFIG_DRM fragment; /dev/dri exists, fbdev X intact

Goal: `CONFIG_MODULES=y` + a minimal `CONFIG_DRM=y` fragment so
`/dev/dri/card0` (+`controlD64`) exist while the vendor DECON fbdev keeps
the panel and fbdev X keeps working at 720×1280. **No display-driver swap.**

### M1 config/patch diff

New file `patches/drm-m1/m1-kconfig.fragment` (6 symbols, merged on top of
`code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64`,
then `olddefconfig`):

```text
CONFIG_MODULES=y
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_EXYNOS=y
CONFIG_DRM_EXYNOS_G2D=y
CONFIG_DRM_EXYNOS_DMABUF=y
```

Deliberately omitted: `DRM_KMS_CMA_HELPER` (nothing in this exynos
snapshot selects/references it — `grep DRM_*CMA_HELPER
drivers/gpu/drm/exynos/` is empty; exynos carries its own
`dma_alloc_attrs` GEM backend in `exynos_drm_gem.c`/`exynos_drm_buf.c`;
the only selectors are tilcdc/shmobile/imx, all with unsatisfied arm64
deps — so CMA-vs-TTM is moot on M1), all display subdrivers
(`FIMD/HDMI/IPP/FIMC/GSC/ROTATOR/VIDI`), `FRAMEBUFFER_CONSOLE`, any
vendor-fbdev off-switch, any CMA-size bump (stays 0).

Mechanism (`drivers/gpu/drm/drm_platform.c:42-110`): `drm_get_platform_dev`
runs `drm_fill_in_dev` + `drm_get_minor(PRIMARY)` **before** `load()`; with
only G2D bound (`manager==NULL` → `fine_cnt++` in
`drivers/gpu/drm/exynos/exynos_drm_core.c:131-135`, G2D probe never fires —
no `samsung,exynos5250-g2d` node in stock DT), `load()` returns 0 and the
already-created primary node survives. `exynos_drm_fbdev_init` early-returns
0 on `num_connector==0`. Expected: `card0` + `controlD64`, zero
connectors/CRTCs, `/dev/fb0` + X untouched. No DT change (0001/0002
display-driver patches are M2 scope).

### M1 flush procedure

```sh
# in the shared kernel tree (serialized — one Image build at a time):
cp code/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64 .config
# merge the 6 fragment symbols (script: code/kernel/m1/m1-kconfig-check.sh),
# then: yes "" | make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- oldconfig
TMPDIR=<gcctmp> make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KCFLAGS="-Wno-error" -j12 Image  # ~7 min
python3 /tmp/mkboot.py arch/arm64/boot/Image /tmp/bimg/ramdisk-new.gz \
  /tmp/bimg/dt.img /tmp/kernel-boot.img "$(cat /tmp/bimg/cmdline)"
sh code/kernel/recovery/check-readiness.sh   # size < 32 MiB, ANDROID! magic
# flash p10 + sync + reboot; verify: sh code/kernel/m1/m1-verify.sh
```

### M1 hard evidence

| Check | Result |
|---|---|
| Kconfig closure (host, `m1-kconfig-check.sh`, 2026-11-18) | **PASS.** Resolved: `CONFIG_MODULES=y` (:217), `CONFIG_DRM=y` (:2273), `CONFIG_DRM_KMS_HELPER=y` (:2274), `CONFIG_DRM_EXYNOS=y` (:2283), `CONFIG_DRM_EXYNOS_DMABUF=y` (:2284), `# CONFIG_DRM_EXYNOS_FIMD is not set` (:2285), `CONFIG_DRM_EXYNOS_G2D=y` (:2288), `CONFIG_FB=y` + CFB trio, `CONFIG_EXYNOS_DECON_FB=y`, `EXYNOS7580=y`, `MIPI_DSI=y`, `DECON_LCD=y`, `PANEL_EA8061_DYNAMIC=y`, `CONFIG_CMA_SIZE_MBYTES=0`, `# CONFIG_FRAMEBUFFER_CONSOLE is not set`. |
| M1 Image build | **IN PROGRESS at report time.** `drm_stub.o` + `exynos_drm_{drv,core,gem,buf,dmabuf,g2d,…}.o` all freshly compiled (timestamps 23:54), `.config` shows all six symbols. Serialized under the integrator; `code/kernel/build.sh` also merges the sibling `pmu-cp.c` patch (relay in the report footer). |
| M1 boot / `/dev/dri` / X | **[PENDING-HW — device unreachable at report time.]** Phone dropped off USB-net (172.16.42.1 ping 100% loss, no `enx*` RNDIS iface; Samsung `18d1:d001` CDC/RNDIS gadget present at `1-2.3` but no host driver bound and `cdc_ether/bind` → `No such device`). Pre-build SSH baseline attempts timed out through `flock /tmp/gpsdev.lock`. Post-boot asserts are scripted and committed (`code/kernel/m1/m1-verify.sh`; harness `code/userspace/gpu/glmark-harness/run-m1.sh`): `ls /dev/dri` (card0+controlD64), `dmesg \| grep -i drm` (`Initialized exynos … on minor 0`), `xdpyinfo` 720×1280. |

## M2 — Mali bound to a DRM render node (kbase-shim fallback designed)

Two branches from `code/userspace/gpu/mesa-render/ADAPT.md`:

- **H1 (preferred): render node speaks panfrost UAPI** (`DRM_IOCTL_PANFROST_*`).
  Then the kbase backend does not apply; stock `panfrost_dri.so` owns the fd.
  Requires the M3 backport (§M3) landed + `CONFIG_MALI_MIDGARD=n` cutover
  (kbase owns `mali@0x11400000` regs, g3d clocks, pd-g3d — cannot coexist).
- **H2 (fallback): render node tunnels kbase UK 10.6.** Only one functional
  hunk changes in the delivered Mesa backend
  (`patches/gpu-mesa-render/kbase-kmod-render-node-adapt.diff`, dry-run
  clean): `kbase_kmod_dev_create` adopts the render fd instead of
  rejecting non-kbase fds via `drmGetVersion`. BO/SAME_VA, submit,
  fences, `vm_bind` legality all unchanged (backend speaks UK, not the
  node name). Probe `code/userspace/gpu/mesa-render/render-node-probe.c`
  classifies the node on-device (`make check` native + qemu pass,
  missing-node exit-2).

Coexistence design (`patches/drm-kms-coexist/DESIGN.md`): Mode A (M1,
vendor fbdev stays, no DT change) → Mode B (render minor: 3.10 already
defines `DRM_MINOR_RENDER=3` + `renderD%d` naming, but **nothing allocates
a render minor** — the M2 patch must call
`drm_get_minor(...,DRM_MINOR_RENDER)`; permission gate needs only
`DRM_AUTH|DRM_UNLOCKED` since 3.10 has no `DRM_RENDER_ALLOW`) → Mode C
(full KMS later, vendor DECON unbound).

M2 evidence: ADAPT.md mapping table + adapt diff (verified `patch -p1
--dry-run` clean); render-node-probe builds native + cross and passes
`make check`/`check-qemu`. **[PENDING-HW: on-device node classification
(H1/H2/NEITHER) + `panfrost_drm.h` ioctl presence.]**

## M3 — Mesa GL on hardware (backport path engineered, stock-Mesa verdict pending)

Target: **panfrost v5.2** (3070 LOC, 15 files, 6 ioctls
SUBMIT/WAIT_BO/CREATE_BO/MMAP_BO/GET_PARAM/GET_BO_OFFSET, no JM_CTX_CREATE —
`docs/scratch/panfrost-backport-inventory.md`). Dependency table with
file:line evidence: ABSENT `drm_sched`, `dma-fence`, `dma-resv`,
`drm_syncobj`, `drm_gem_shmem_helper`, `io-pgtable`, `drm_dev_alloc`
split, `readl_poll_timeout`, atomic helpers; PRESENT `drm_gem_cma_helper`
(272 lines), `drm_prime` export/import, old `drm_gem_mmap`, `devfreq`+old
OPP, vendor `pm_domains.c:413/431` notifier (replaces genpd/device-link),
mali DT node, kbase clocks/regulator.

Delivered foundation (`patches/drm-backport/0001-0005`): minimal
dma-fence (ordered-seqno) → dma-resv (plain-mutex shim, no `ww_mutex` in
3.10) → drm_syncobj (binary) → drm_sched (panfrost-job-only FIFO shim) +
build wiring (new files only, `git apply --check` PASS individually + as
series, each of the 8 new files passes `-fsyntax-only` AND real `-c`
object compile vs actual 3.10.108 headers with zero warnings).
NOT done: full Image link, panfrost GEM/job-slot driver itself,
`drm_dev_alloc`/GEM adaptation, boot, GPU-read tests.

Supporting work: `patches/iommu-dma/README.md` (CMA + dma_attrs works on
this arm64 tree with no adaptation; GPU needs no sysmmu — own MMU via
`AS_TRANSTAB`; keep CMA 32 MB at M2; `COHERENCY_NONE` + memattr divergence
`0x8D884D4F48` vs `0x888D88` stays open to M2); `code/userspace/gpu/dvfs-power/POWER.md`
(clocks/regulator/DVFS tables + OPP seed) and monitor;
`code/userspace/gpu/panfrost-validate/` (60-check register/LPAE/transtab
checker, native + qemu PASS, + device CHECKLIST);
`code/userspace/gpu/glmark-harness/` (M1/M3 run scripts + THRESHOLDS.md
from `gpu-verify.md` baselines: llvmpipe LLVM 23.1.5, glxgears 173–221 FPS
@320×240, glmark2-es2 29-scene table).

M3 evidence: backport series check+compile as above; Mesa side pending
(no Mesa source on host — fetch+build commands in ADAPT.md §5).
**[PENDING-HW: `eglinfo -B` / `glxinfo -B` renderer ≠ llvmpipe, glmark2-es2
FPS table, 2D X surface.]**

## Open blockers / smallest next experiments

1. **Device link down** (the immediate blocker): restore USB-net, then run
   the M1 flash + `m1-verify.sh`. Smallest step: rebind/re-enumerate the
   `1-2.3` RNDIS gadget (host `cdc_ether/bind` failed `No such device`;
   likely needs re-plug or phone-side USB state check), then `ping
   172.16.42.1` → SSH baseline → flash.
2. **M1 boot asserts** (once linked): `/dev/dri/card0`+`controlD64`,
   `dmesg` exynos init, X 720×1280 (`run-m1.sh`).
3. **M2 node classification**: `render-node-probe /dev/dri/renderD128`
   → H1/H2/NEITHER decides stock-panfrost vs kbase-backend path.
4. **M3 bring-up order**: land M1 → render minor (Mode B) → backport
   series → panfrost GEM/job-slot driver (not yet written — the honest gap;
   estimate 20–30 person-days per `gpu-panfrost.md` M2) → Mesa build →
   glmark harness.

Note on `code/kernel/build.sh` (integrator flow): it applies **every**
`*.patch` in the pmaports dir, so the current M1 Image also contains the
sibling RIL-session `0001-cp-power-raw-ctrl.patch` (TZ READ_CTRL tolerance,
`pmu-cp.c`). GPS driver/config are untouched (zero diff, §0); that patch
is modem power-domain, out of GPU scope — recorded here so the M1 image
contents are exact.
