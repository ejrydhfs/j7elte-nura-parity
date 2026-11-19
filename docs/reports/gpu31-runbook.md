# GPU31 first-hardware-run runbook — M1 → M2 → M3 with gates and aborts

Date: 2026-11-19. Device: SM-J700M (j7elte), Nura Alpine aarch64.
Running baseline at writing: kernel `3.10.108 #35 SMP PREEMPT` (gcc 16.2.0),
`/dev/mali0` present (`Mali-T72x MP2 r1p0 0x0720`, DDK r15p0 / UK 10.6),
`/dev/dri` absent (expected pre-M1). Target: panfrost **v5.2** (UAPI
byte-identical, `gpu31-target.md`), render-only, stock `dt.img` always.

This is the single operator sequence. It is self-consistent with:
`gpu31-master.md` (milestones + invariants), `gpu31-m1-boot.md` (pre-flash
gate + baseline + `m1-verify.sh`), `gpu31-integration.md` (candidate image),
`gpu31-recovery.md` (boot-count fallback + TWRP), `gpu31-probe.md`
(render-node classifier), `gpu31-client.md` (six-ioctl selftest),
`gpu31-tests.md` (glmark harness), `gpu31-wsi.md` (X11 path),
`gpu31-target.md` (v5.2 contract), `gpu31-kms.md` (Mode B render node),
`gpu31-review.md` (known defects to watch).

## 0. Access and inviolable rules

All device commands go under a short host lock hold (<120 s). Never hold
the lock across a build, never reboot without Main's go:

```sh
SSH="SSH_ASKPASS=/tmp/ap.sh SSH_ASKPASS_REQUIRE=force ssh -o StrictHostKeyChecking=no user@172.16.42.1"
flock /tmp/gpsdev.lock -c "$SSH '<cmd>'"
```

- Root via `rootshell` (sudo is broken: `/etc must be owned by root`).
  `rootshell` is `/usr/local/bin/rootshell`, verified present.
- Root fs 94% full: stage binaries/logs under `/data` (`/data/probe`,
  `/data/glmark-harness`), never `/tmp` (tmpfs full) or `/`.
- **Flash gate:** ONLY `G31_M1Verify` flashes, ONLY after Main's explicit
  go. `G31_Integrator` alone builds. No `dd` to `mmcblk0p10`, no `reboot`,
  no sysrq-b from any other slice. Cold boot is scarce (GPS/FM share the
  phone).
- Shared kernel tree is never edited; slices ship patch files under
  `code/gpu31/<slice>/`. Stock `dt.img` is never rebuilt (S-Boot hangs on
  rebuilt DTBs).

## 1. Pre-flight (host-side, no device touch)

1. Integrator candidate (live reference `gpu31-integration.md` §1):
   `Image` 18110712 B, sha256
   `8a7d0452d2de4ce…44bef328`; packed `/tmp/kernel-boot.img` 27250688 B,
   sha256 `cf25ed70…51cb68be726` (rebuilt 19 Nov 01:26; the older
   `53acd61a…` sha in `gpu-drm.md` is stale).
2. `sh code/kernel/recovery/check-readiness.sh /tmp/kernel-boot.img`
   must print **READY** (`ANDROID!` magic, total < 33 554 432 = BOOT p10
   32 MiB, ramdisk `ac0b6f8b…c8491a18` identical to known-good,
   dt `352bda9b…69d8f937` identical to stock, ARM64 magic `0x644d5241`).
3. Payload string delta: candidate kernel contains `exynos-drm`,
   `drm_get_platform_dev`, `mali_kbase`; known-good
   (`fcec651e…f038`, 26 374 144 B) lacks `exynos-drm`. If READY fails or
   the dt/ramdisk hashes differ: **ABORT, do not flash** (see §7).
4. Recovery staged before any flash (`gpu31-recovery.md` §§3–4):
   `initramfs-0003-bootcount-fallback.patch` in the ramdisk,
   `/tmp/bindboot.img` present and matching `known-good.sha256`,
   `RECOVERY.md` §3 (TWRP + `adb push`) re-read with a human at the
   device confirmed.

## 2. Flash (M1Verify only, after Main's go)

Exact sequence (`gpu31-m1-boot.md` §4, unchanged from `RECOVERY.md`):

```sh
scp -o StrictHostKeyChecking=no /tmp/kernel-boot.img user@172.16.42.1:/data/
flock /tmp/gpsdev.lock -c '<ssh> "rootshell dd if=/data/kernel-boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync"'
# then, same hold: rootshell "sync; reboot"   (never sysrq-b)
```

Wait ~5 min for SSH return. On first **good** boot, snapshot the fallback:

```sh
sh code/kernel/recovery/cache-snapshot.sh M1   # dd p10 -> CACHE known-good-boot.img, verify ANDROID!, reset counter
# install guard:
scp boot-guard.sh user@172.16.42.1:/usr/local/bin/   # via rootshell install
# /etc/init.d/boot-guard + rc-update add boot-guard default
```

The guard watches up to 300 s for `xdpyinfo` = 720x1280 AND the milestone
device (M1: `card0`+`fb0`; M2: `renderD128`+fb0; M3: render node +
non-llvmpipe `glxinfo`). Three consecutive guard-silent boots self-restore
p10 from CACHE (`FAIL_MAX=3`). If the fallback ever reports "no valid cache
copy", re-run the snapshot; if an image cannot reach initramfs, use TWRP
per `RECOVERY.md` §3.

## 3. M1 asserts — expected dmesg + nodes (post-boot)

Run `code/kernel/m1/m1-verify.sh` on the phone (7 groups, exit 0 =
`M1-VERIFY-PASS (7/7)`). Expected deltas vs the §0 baseline:

| Probe | #35 baseline (pre-M1) | M1 flashed (expected) |
|---|---|---|
| `dmesg \| grep -ci drm` | `0` | ≥ 1 (`exynos … on minor 0`-class init line) |
| `dmesg \| grep -iE 'exynos-drm\|exynos_drm'` | empty | ≥ 1 line (print these lines into the report) |
| `dmesg \| grep -i mali` | latency lines only (`state restore/save latency exceeded`) | same lines, no new kbase errors |
| `/dev/dri` | `No such file or directory` | `card0` + `controlD64` char nodes; `renderD128` INFO-only (absent expected on M1, tolerated if present) |
| `/dev/fb0` + `virtual_size` | char `29, 0`, `720,1280` | identical (vendor DECON fbdev keeps the panel) |
| X | `Xorg :0 … 99-fbdev.conf`, `xdpyinfo` = `720x128 pixels` | identical |
| net | `default via 172.16.42.2 dev rndis0` | identical (proves RNDIS path back) |
| GPS | `/dev/ttySAC0` char `204, 64`, `GPS_PWR_EN/value` readable (open-as-user EACCES is parity; open-as-root is the assert) | identical |
| kbase | `/dev/mali0` char `10, 59` | identical |

Note: `/proc/cmdline` reads `console=ttySAC1` (S-Boot rewrites bootargs;
differs from the mkboot header `console=tty0 loglevel=7` — not a failure).
Early-boot banner lines may already have rotated out of the ring at check
time; assert on `grep -i drm` + node existence, not on the `Linux version`
banner.

## 4. M2 — render node + H1/H2 decision

Prerequisite: M1 PASS + Mode-B patch applied by Integrator
(`code/gpu31/kms/0001-drm-3.10-render-node-mode-b.patch`: render alloc
before `->load()`, render auth, `DRM_MASTER` denied on render —
`gpu31-kms.md` §2). No KMS takeover, no DT edit, no vendor-fbdev switch
(review §9: M1 node-only and display-swap are alternatives, not complements).

1. Copy the static probe + runner to `/data/probe`, one flock-held run:

```sh
scp -o StrictHostKeyChecking=no code/gpu31/probe/render-node-probe user@172.16.42.1:/data/probe/
flock /tmp/gpsdev.lock -c '<ssh> "rootshell sh /data/probe/run-device-probe.sh"'  # logs under /data/probe
```

2. Classify (`gpu31-probe.md` §1): exit `0` = H1 (real panfrost DRM, H1+H2
   hybrid counts as H1), `3` = H2-only (kbase-shim path), `4` = NEITHER,
   `2` = NO_DEVICE, `1` = usage error. `--json` carries `h1/h2/t720`
   (`t720=true` iff `GPU_PROD_ID == 0x0720`), the 9-value `params` map,
   and the kbase handshake record.
3. Expected transitions: M1 creates `/dev/dri` (SKIP → PASS); a panfrost
   node flips `/dev/dri/renderD128` to H1 exit 0 `t720:true`; an H2 shim
   flips it to H2 exit 3 instead; `/dev/mali0` stays H2 throughout.
   Missing `/dev/dri` at this stage = **ABORT M2, re-check M1 image**
   (do not proceed to client/Mesa).

## 5. Client selftest on the real node (six v5.2 ioctls)

Contract (`gpu31-target.md` §1, `gpu31-client.md`): ioctls `0x00 SUBMIT`,
`0x01 WAIT_BO`, `0x02 CREATE_BO`, `0x03 MMAP_BO`, `0x04 GET_PARAM`
(`GPU_PROD_ID`-only), `0x05 GET_BO_OFFSET`; `CREATE_BO.flags`
reserved-zero (reject nonzero). `SUBMIT` validation:
`jc==0‖(jc&7)`, bad `requirements`, `in_sync>16`, `bo_handles>64` →
`EINVAL`. Unknown handle → `ENOENT`.

```sh
scp -o StrictHostKeyChecking=no build-aarch64/pf7580-selftest user@172.16.42.1:/data/
flock /tmp/gpsdev.lock -c '<ssh> "PF7580_DEVICE=/dev/dri/renderD128 /data/pf7580-selftest --device /dev/dri/renderD128"'
```

Expect exit 0: `GPU_PROD_ID 0x0720 (T720)`, BO create/map R/W,
`wait_bo(0)` — the safe path never SUBMITs (a test `jc` would fault
hardware). Device-absent path is exit 2 with ABI dump. Any `EIO`/assert
failure here gates Mesa: **do not run glmark until the selftest is green**.

## 6. M3 — Mesa renderer check + glmark harness

1. Renderer string (harness G1, `gpu31-tests.md` §2): grep
   `glxinfo -B` → `es2_info` → `eglinfo -B` for
   `renderer string|GL_RENDERER|profile renderer`.
   `*llvmpipe*|*swrast*|*Softpipe*` → `software`;
   `*Mali*|*mali*|*Panfrost*|*ARM*|*Exynos*|*kbase*|*lima*` → `hardware`.
   M3 PASS requires `hardware` under `--expect hardware`; llvmpipe on an
   M3 run is a FAIL (today's `--expect software` runs are green by intent).
2. Triangle demo first (cheap, `gpu31-wsi.md` §§2–3): X11/EGL/GLES2
   `tri-x11 --frames 60 --hold 1` on `DISPLAY=:0` must print `RENDER_OK`
   (today: llvmpipe 66–295 FPS at 320x240; post-M3 the renderer string
   flips to Mali/Panfrost).
3. Full harness (one locked hold, X must be up; no reboot/flash):

```sh
flock /tmp/gpsdev.lock -c '<ssh> "mkdir -p /data/glmark-harness"'
scp -o StrictHostKeyChecking=no code/gpu31/tests/run-harness.sh code/gpu31/tests/scenes.tbl user@172.16.42.1:/data/glmark-harness/
scp -o StrictHostKeyChecking=no code/gpu31/tests/tools/* user@172.16.42.1:/data/glmark-harness/
flock /tmp/gpsdev.lock -c '<ssh> "DISPLAY=:0 sh /data/glmark-harness/run-harness.sh --expect hardware --out /data/glmark-harness/h-$(date +%Y%m%d-%H%M%S)"'
```

Smoke first with `--skip-glmark --skip-gpu` (<30 s). Full run gates G1–G5:
renderer class, `glmark2-es2 -s 320x240 -b :duration=2.0` floors
(`max(baseline×0.5, 5)`, heavy four must exceed 1 FPS, WARN-only),
`xwd -root` surface 720×1280 distinct≫100, GPU sanity
(`utilization>0` or `time_in_state` growth on hardware; `util=0` + flat
TIS is the software signature), `verdict.json` (`schema:
glmark-harness/1`) + `verdict.txt` + `RESULT`. Exit 0 iff every
non-skipped gate PASSes. Known-good software reference: glxgears
173–221 FPS, 29-scene glmark2-es2 table (`gpu-verify.md` §§3.1–3.4).

## 7. Abort / rollback triggers (exact commands → actions)

FAIL-anything below stops the run at that step. Do not advance milestones
past a FAIL.

| # | Trigger | Check | Action |
|---|---|---|---|
| A1 | Bad image | `check-readiness.sh` ≠ READY, size > 32 MiB, magic ≠ `ANDROID!`, dt/ramdisk hash drift | **Do not flash.** Hand back to Integrator for rebuild |
| A2 | No SSH return | no RNDIS/default route ~5 min post-`reboot` | Wait once more (link flaps recover); then TWRP restore `bindboot.img` per `RECOVERY.md` §3 — needs the human at the device |
| A3 | DRM missing post-M1 | `m1-verify.sh` → `M1-VERIFY-FAIL`, `/dev/dri` absent, `grep -ci drm` still 0 | Stay on serial diagnosis; if 3 consecutive boots stay guard-silent, CACHE fallback self-restores p10 automatically |
| A4 | Display regression | `/dev/fb0` gone, `virtual_size` ≠ `720,1280`, `pgrep Xorg` empty, `xdpyinfo` fails | **Rollback immediately** (fbdev X is the invariant): CACHE restore or TWRP `bindboot.img` |
| A5 | kbase/GPS regression | `/dev/mali0` gone, new `mali …` errors in dmesg, `/dev/ttySAC0` char node gone | Rollback; file dmesg excerpt against `gpu31-m1-boot.md` §2 table |
| A6 | Kernel fault | `dmesg` oops / `JS: Job Hard-Stopped` / GPU soft-reset / stuck `utilization` after idle | Stop load, capture `dmesg`, rollback; review §§1–4 shim defects (fence/resv/syncobj/sched REQUIRED items) before retry |
| A7 | Render node missing post-M2 | probe exit 2/4, `renderD128` absent | Do not run client/Mesa; re-check Mode-B build |
| A8 | Selftest red | `pf7580-selftest` exit 1 (assert/`EIO`) | Do not run glmark; fix GEM/driver per `gpu31-client.md` §1 rules |
| A9 | Software GL on M3 run | renderer `software` under `--expect hardware`, TIS flat, `util=0` | FAIL the run (record `verdict.json`); keep `j7elte-gpu-soft` (`LIBGL_ALWAYS_SOFTWARE=1`) as the shipped path until HW GL lands |
| A10 | Counter overflow | CACHE restores but boots keep failing open | Re-snapshot only from a verified-good boot; never snapshot a FAIL boot |

Post-run: copy `/data/probe/device-probe-*.log` and the harness output dir
(raw logs + `verdict.json/txt` + `RESULT`) back to the repo evidence path,
and record dmesg excerpts + `/dev/dri` listing + X check in the milestone
report before closing the run.
