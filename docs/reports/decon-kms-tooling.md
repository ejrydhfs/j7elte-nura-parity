# DECON KMS tooling — modetest/kmscube/libdrm, pattern+flip test, 300-flip soak

Status: **headless half executed and PASS on the phone; the card0 half is
ready and SKIPs today** (no `/dev/dri/card0` until the M1 DECON KMS
subdriver binds). No flash, no reboot; every device command ran under
`flock /tmp/gpsdev.lock`. Owner slice: `KmsTooling`
(`decon-kms-plan.md` §6 #6).

Deliverables (committed): `code/gpu31/kms/tools/` — a native on-device
pattern/flip checker `kmsflip`, a GBM+EGL-surfaceless runner that works
**before** card0 exists, a card0 acceptance runner, a 300-flip soak, a dmesg
counter gate, and a host-side `remote.sh` that serializes everything behind
the shared device lock.

Related: `decon-kms-plan.md` (milestones), `gpu31-mesa20.md` (the Mesa 20.3.5
build), `drm-driver.md`, `gpu-drm.md` (current kernel = render node only).

---

## 1. What is installed today (device inventory)

Kernel: `Linux nura-j7elte 3.10.108 #37 SMP PREEMPT … aarch64`.

| Item | State on the phone | Evidence |
|---|---|---|
| DRM nodes | **`/dev/dri/renderD128` only; no `card0`** | `ls /dev/dri` → `by-path renderD128` |
| `/sys/class/drm` | `renderD128`, `version` → `drm 1.1.0 20060810` | `cat /sys/class/drm/version` |
| fbdev | `/dev/fb0`, modes `U:720x1280p-60`, `U:720x1280p-51` | `cat /sys/class/graphics/fb0/modes` |
| libdrm | **2.4.134-r0** + `libdrm-dev-2.4.134-r0` (headers+pkgconfig) | `apk list -I` |
| libdrm tools | **`libdrm-tests-2.4.134-r0`** → `/usr/sbin/modetest` *(installed by this slice; was absent)* | same |
| kmscube | **`kmscube-0_git20220902-r1`** → `/usr/sbin/kmscube` *(installed by this slice; was absent)* | same |
| system Mesa | `mesa-26.3.0-r0` (+ egl/gbm/gles/dri-gallium/dev), `/usr/lib/libEGL.so.1.0.0`, `/usr/lib/libgbm.so.1.0.0` | `apk list -I` |
| Mesa 20.3.5 diagnostic build | `/data/mesabuild/mesa20-b` — panfrost only, `-Dplatforms=x11 -Dglx=dri -Degl=enabled -Dgbm=enabled -Dgles1/2=enabled -Dllvm=disabled`; outputs `src/egl/libEGL.so`, `src/gbm/libgbm.so`, `src/mapi/es2api/libGLESv2.so`, `src/gallium/targets/dri/{libgallium_dri.so,panfrost_dri.so}` | `meson-log.txt`; `gpu31-mesa20.md` §2 |
| toolchain | `gcc (Alpine 15.2.0)`, `meson`, `ninja`, `pkg-config`, `python3` | `command -v` |
| other GL/KMS-era clients | `weston-16.0.0-r0` + `weston-backend-drm`, `glmark2-2023.01` (`glmark2-drm`, `glmark2-es2-drm`), `mesa-demos` (`eglkms`, `eglgears_*`, `eglinfo`, `es2_info`), `freeglut` | `apk list -I`, `/usr/bin` |

### What is missing / must be built

1. **`card0` + a DECON KMS CRTC.** Nothing userspace can fix; this is M1.
   Consequence today: `modetest` and `kmscube` cannot run:
   - `modetest -M exynos -c` → `failed to open device 'exynos' with busid
     '(null)': No such file or directory`
   - `kmscube -x -c 5` → `no drm device found! / could not open drm device /
     failed to initialize legacy DRM`
2. **`modetest` and `kmscube` were not installed** on the image. They do not
   have to be built — they are stock packages. `install-deps.sh` installs
   them (`apk add kmscube libdrm-tests`; the APK index on the phone resolves
   both, network is the USB-NAT link). Re-run it after any image rebuild.
3. **`weston-simple-egl` is not packaged** (`apk search` finds no package;
   `weston` ships only `weston-simple-im` and icons). If wanted, build
   `clients/simple-egl.c` from the weston 16 source; it is *not* required —
   `kmscube` exercises the same GBM/EGL/GLES2/page-flip path for M2.
4. **`kmsflip` must be built** — the deterministic pattern/flip/checksum
   checker with a machine-readable PASS/FAIL. It is built **natively on the
   phone** (the host cross sysroot has no DRM/EGL headers and the device is
   musl; same precedent as `code/gpu31/egltest`, `wsi`).

No cross-compilation is needed for anything in this slice.

---

## 2. Tooling layout (`code/gpu31/kms/tools/`)

| File | Role |
|---|---|
| `kmsflip.c` | The atomic test tool (see `kms-flip-tool.md`). KMS mode: dumb `ADDFB2` buffers plus dma-bufs imported from the panfrost render node (or card PRIME round-trip), atomic modeset + N `DRM_MODE_ATOMIC_NONBLOCK|PAGE_FLIP_EVENT` flips, per-commit/event timeouts, per-phase EVENTS/EBUSY/EINVAL counters, FNV-1a checksum verification, `--list`, `--dump`; `--probe` checks the dma-buf source on render-only images. Headless mode: GBM BO alloc+map on a render node, then EGL surfaceless + GLES2 render, `glReadPixels` checksum across N frames. |
| `Makefile` | Native build. `make` = KMS+EGL/GBM; `make NO_EGL=1` = libdrm only; `make check` = 3-frame headless smoke; `make probe` = render-node dma-buf source probe. |
| `build.sh` | On-device build wrapper (header checks, `file`/`ldd`). |
| `env.sh` | Selects the Mesa stack: `egl` (mesa20-b + `EGL_PLATFORM=surfaceless`), `kms` (mesa20-b libs + `MESA_LOADER_DRIVER_OVERRIDE=panfrost`, no platform override), `system` (stock Mesa 26). |
| `kms-dmesg.sh` | `snapshot` / `delta` of DECON underrun, vsync/update-timeout, DSIM, DRM-core and panfrost counters. Gate: underrun/timeout/DSIM deltas must be 0; DRM/panfrost deltas are diagnostics (`STRICT_DRM=1`, `STRICT_PF=1` to escalate). |
| `install-deps.sh` | Root-only: `apk add kmscube libdrm-tests`, prints versions and binaries. |
| `run-headless.sh` | **Runs today.** GBM + EGL-surfaceless checksum run on `renderD128` against mesa20-b, plus dmesg delta. |
| `run-kms.sh` | card0 acceptance: `kmsflip --list`, `modetest -M exynos` resources + `-s conn:#0` animation + `-v` vsync flip, `kmsflip` 10 flips, then `kmscube` 10 s (skippable for the M1 gate). |
| `run-soak.sh` | 300-flip soak with the same checks + memory/thermal stamps. |
| `remote.sh` | Host-side driver: `push`, `build`, `headless`, `kms`, `soak`, `logs`, `pull`; every `ssh`/`scp` under `flock /tmp/gpsdev.lock` with the askpass helper. |

### Why the Mesa-20 env is required

The kernel names the device `panfrost7580`; Mesa's loader looks for
`panfrost7580_dri.so` unless `MESA_LOADER_DRIVER_OVERRIDE=panfrost`
short-circuits it (`gpu31-mesa20.md` §3). `env.sh` also sets
`LD_LIBRARY_PATH` to the mesa20-b build and `LIBGL_DRIVERS_PATH` to its
`libgallium_dri.so`+`panfrost_dri.so`. `LIBGL_ALWAYS_SOFTWARE`, `DRI_PRIME`
and `PAN_MESA_DEBUG` are cleared so a lingering llvmpipe environment cannot
silently satisfy the test.

---

## 3. Exact commands

### Host → device (recommended, serialized)

```sh
cd /tmp/j7repo/code/gpu31/kms/tools
./remote.sh push          # scp kmsflip.c + scripts to /data/kms/tools
./remote.sh build         # native make on the phone
./remote.sh headless      # GBM+EGL test (works today, no card0)
./remote.sh kms           # card0 gate (SKIP today)
./remote.sh soak          # 300 flips (SKIP today)
./remote.sh logs          # newest on-device log
./remote.sh pull          # /data/kmslogs -> ./kmslogs
```

`remote.sh` needs the one-time root-owned directory creation (phone):

```sh
rootshell /bin/sh -c 'mkdir -p /data/kms/tools /data/kmslogs && \
                       chown -R user:user /data/kms /data/kmslogs'
```

### On the device directly

```sh
# root, once per image rebuild
rootshell /bin/sh /data/kms/tools/install-deps.sh

# build (as user)
cd /data/kms/tools && sh build.sh                 # EGL+GBM build
# cd /data/kms/tools && NO_EGL=1 sh build.sh      # KMS-only build

# today: no card0
sh /data/kms/tools/run-headless.sh
FRAMES=300 sh /data/kms/tools/run-headless.sh     # extended stability loop

# after M1 lands
sh /data/kms/tools/run-kms.sh
SKIP_KMSCUBE=1 sh /data/kms/tools/run-kms.sh      # M1 gate only
sh /data/kms/tools/run-soak.sh                    # 300 flips by default
```

Every device invocation must stay inside the shared lock, e.g.:

```sh
flock /tmp/gpsdev.lock sh /data/kms/tools/run-kms.sh
```

### Environment knobs

| Variable | Default | Meaning |
|---|---|---|
| `MESA20_B` | `/data/mesabuild/mesa20-b` | Mesa 20.3.5 build dir |
| `MESA_MODE` | `egl` (headless) / `kms` (kms) | `egl`, `kms`, `system` |
| `GBM_NODE` | `/dev/dri/renderD128` | headless node |
| `KMS_DEV` | `/dev/dri/card0` | KMS node |
| `KMS_MODULE` | `exynos` | modetest `-M`; falls back to `-D $KMS_DEV` |
| `FRAMES` | 30 headless / 10 kms / 300 soak | loop count |
| `PATTERN` | `bars` | `bars`, `checker`, `gradient`, `noise` |
| `TIMEOUT_MS` | 250 kms / 200 soak | per-flip completion timeout |
| `EXPECT_RENDERER` | `Mali` (headless) | empty accepts llvmpipe |
| `SKIP_KMSCUBE` | unset | `1` = M1 gate only |
| `STRICT_DRM`, `STRICT_PF` | 0 | make DRM/panfrost dmesg deltas fatal |
| `KMS_SSH_ASKPASS` | `/tmp/ap.sh` | host `remote.sh` askpass helper |

---

## 4. Pass/fail criteria

`kmsflip` atomic summary line (`flips/timeouts/events/ebusy/einval`);
full per-counter table in `kms-flip-tool.md`:

| Gate | PASS condition | What it catches |
|---|---|---|
| Flips retire | `flips == frames` per phase, `timeouts == 0`, `einval == 0`, `events == frames+1` | missing/never-signalled vblank, dead atomic flip path, rejected properties, lost events |
| Pattern integrity | `checksum_bad == 0`; FNV-1a-64 over the active pixels is identical before the flip and after the flip event | dma-buf/GEM corruption, cache/alias bugs, buffer reused under scanout |
| Content actually changes | successive `--pattern solid` frames use different colours; each buffer is refilled before its flip and checksummed after the event | “flip succeeded” but the same buffer/memory is scanned out |
| Display pipeline counters (`kms-dmesg.sh delta`) | `underrun_delta == 0`, `timeout_delta == 0`, `dsim_delta == 0` | DECON FIFO underrun (`underrun (level …`), `decon wait for vsync timeout`, `linecnt_is_zero timeout`, DSIM errors |
| Diagnostics | `drm_delta`, `p7580_delta` printed; fatal only with `STRICT_DRM=1` / `STRICT_PF=1` | DRM-core `*ERROR*`, panfrost job errors |

Headless (`kmsflip --headless`): PASS requires GBM BO creation, EGL/GLES2
init, a nonzero checksum that is **identical for all N frames**, and
`GL_RENDERER` containing `Mali` by default. `gbm_map` is reported separately
(render-to-GBM does not need CPU mapping, so the tool does not fail on it),
but on the current panfrost stack it is `1` and a change should be
investigated. Different Mesa/driver versions produce different absolute
checksum constants; compare the constant across builds, not to a hardcoded
number.

Soak: all of the above for `FRAMES` (default 300) flips, plus the script
records `maxrss`, wall time/FPS, `MemAvailable` and the max thermal zone
before/after. At 60 Hz the 300-flip run should take ≈5.0 s; a slower run is
not itself a failure unless flips time out or underruns appear.

dmesg gate details: counters are whole-log `grep -c` counts, so the script
compares before/after. If the kernel ring wraps mid-run the delta can go
negative — treat that as FAIL/inconclusive and re-run (or `dmesg -c` as root
immediately before a soak) rather than ignoring it.

---

## 5. Evidence (executed on the phone, 2026-11-23)

### 5.1 Headless GBM + EGL surfaceless, Mesa 20.3.5 panfrost — **PASS**

```
$ FRAMES=30 sh /data/kms/tools/run-headless.sh
env.sh: mode=egl B=/data/mesabuild/mesa20-b LD_LIBRARY_PATH=…/src/egl:…/src/mapi/es2api:…/src/gbm:…/src/gallium/targets/dri:…/src/mapi/shared-glapi:…/src/glx
Linux nura-j7elte 3.10.108 #37 SMP PREEMPT Sun Nov 22 23:16:55 EST 2026 aarch64 Linux
KMSFLIP-HEADLESS: gbm backend=drm node=/dev/dri/renderD128
KMSFLIP-HEADLESS: gbm bo=720x1280 stride=2880 csum=0x0093300371d3a725 map=ok
KMSFLIP-HEADLESS: EGL 1.4 vendor=Mesa Project
KMSFLIP-HEADLESS: GL_RENDERER="Mali T720 (Panfrost)" GL_VERSION="OpenGL ES 3.0 Mesa 20.3.5"
KMSFLIP-HEADLESS: frame 30/30 csum=0x016f30f2da294819
KMSFLIP-HEADLESS: node=/dev/dri/renderD128 size=720x1280 frames=30 stable=1 gbm=1 gbm_map=1 csum=0x016f30f2da294819 PASS
KMSDMESG: underrun_delta=0 timeout_delta=0 dsim_delta=0 drm_delta=0 p7580_delta=0 PASS
HEADLESS: kmsflip_rc=0 dmesg_rc=0 log=/data/kmslogs/headless-20261123T135639Z.log
RESULT: PASS
```

The 720×1280 pattern PPM (`/data/kmslogs/headless-20261123T135639Z.ppm`)
is produced by the same tool and is the visual artifact for M2’s “correct
colours” check.

### 5.2 card0 gates — **SKIP today (expected)**

```
$ sh /data/kms/tools/run-kms.sh
KMS: SKIP no /dev/dri/card0 (DECON KMS subdriver not bound; only render node today)
$ echo $?
3
$ sh /data/kms/tools/run-soak.sh
SOAK: SKIP no /dev/dri/card0
$ echo $?
3
```

`modetest` and `kmscube` are installed but cannot open a device yet
(§1). Their failure paths were checked so the card0 scripts will surface a
real failure, not a missing tool:

```
$ modetest -M exynos -c
failed to open device 'exynos' with busid '(null)': No such file or directory
$ kmscube -x -c 5
no drm device found!
could not open drm device
failed to initialize legacy DRM
$ kmsflip --device /dev/dri/renderD128 --list
kmsflip: drmModeGetResources: Invalid argument
```

(The last line is the expected render-node rejection — it proves the tool
reaches `drmModeGetResources` and fails cleanly instead of mis-binding to
`renderD128`.)

### 5.3 300-frame headless stability loop — **PASS**

```
$ FRAMES=300 sh /data/kms/tools/run-headless.sh
KMSFLIP-HEADLESS: frame 300/300 csum=0x016f30f2da294819
KMSFLIP-HEADLESS: node=/dev/dri/renderD128 size=720x1280 frames=300 stable=1 gbm=1 gbm_map=1 csum=0x016f30f2da294819 PASS
KMSDMESG: underrun_delta=0 timeout_delta=0 dsim_delta=0 drm_delta=0 p7580_delta=0 PASS
RESULT: PASS
```

The checksum is bit-identical to the 30-frame run above, i.e. the pattern
and the readback path are deterministic across runs. This is the
soak-scale exercise of the userspace path while card0 is unavailable; the
300-*flip* soak is `run-soak.sh`.

### 5.4 Build evidence

```
$ sh build.sh
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -DHAVE_EGL=1 -I/usr/include/libdrm -o kmsflip kmsflip.c -ldrm -lgbm -lEGL -lGLESv2
build: /data/kms/tools/kmsflip
/data/kms/tools/kmsflip: ELF 64-bit LSB pie executable, ARM aarch64, dynamically linked, interpreter /lib/ld-musl-aarch64.so.1
BUILD: PASS
```

`NO_EGL=1` also builds clean (no warnings, binary reports “built without
EGL/GBM support” for `--headless`).

---

## 6. What the M1/M2 acceptance run will look like

After the DECON KMS subdriver binds (`card0` present), `run-kms.sh`:

1. `kmsflip --list` prints `CONNECTOR id=<n> … status=connected` + modes
   (panel: 720×1280@60 preferred) and the CRTC id.
2. `modetest -M exynos` lists CRTC/encoder/connector; `modetest -s <conn>:#0`
   animates the test pattern for 8 s (killed by timeout = survived);
   `modetest -v` runs the libdrm vsynced page-flip test.
3. `kmsflip --frames 10` prints, on success:

```
KMSFLIP: dev=/dev/dri/card0 connector=<n> crtc=<m> plane=<p> mode=720x1280 (720x1280@60) atomic=1 saved_fb=<f>
KMSFLIP: phase=dumb source=dumb modeset=1 flips=10/10 commits=11 commit_ok=11 events=11 timeouts=0 ebusy=0 einval=0 other=0 checksum_bad=0 elapsed_ms=<…> PASS
KMSFLIP: phase=dmabuf source=panfrost-gbm modeset=0 flips=10/10 commits=11 commit_ok=11 events=11 timeouts=0 ebusy=0 einval=0 other=0 checksum_bad=0 elapsed_ms=<…> PASS
KMSFLIP: restore=atomic saved_fb=<f>
KMSFLIP: RESULT PASS device=/dev/dri/card0 … frames=10 phases=2 commits=22 events=22 flips=20/20 timeouts=0 ebusy=0 einval=0 other=0 restore=ok elapsed_ms=<…>
KMSDMESG: underrun_delta=0 timeout_delta=0 dsim_delta=0 drm_delta=0 p7580_delta=0 PASS
RESULT: PASS
```

4. `kmscube -D /dev/dri/card0` for 10 s (M2: panfrost-rendered buffers
   scanned out; `SKIP_KMSCUBE=1` for the M1-only gate).

Then `run-soak.sh` for the 300-flip soak. If the kernel rejects the dumb
buffer path, `kmsflip` prints an explicit
“M1 acceptance needs a dumb_create path in the DECON KMS driver” hint — that
is a driver gap, not a tooling gap.

Mode/connector discovery for debugging:

```sh
/data/kms/tools/kmsflip --device /dev/dri/card0 --list
/data/kms/tools/kmsflip --device /dev/dri/card0 --list | sed -n 's/^CONNECTOR id=\([0-9]*\).*status=connected.*/\1/p'
```

---

## 7. Notes and risks

- **Atomic path.** `kmsflip` now builds the compositor path itself:
  `DRM_CLIENT_CAP_ATOMIC`, plane `FB_ID`/`CRTC_ID`/`SRC_*`/`CRTC_*` commits
  with `PAGE_FLIP_EVENT` completions, and (when the kernel has `MODE_ID` +
  property blobs) a full atomic modeset with connector `CRTC_ID` and CRTC
  `ACTIVE`/`MODE_ID`. It does not use `drmModePageFlip()` for the test, so it
  fails where KWin would fail even if `modetest -v` (legacy) still passes.
  On the v4.0 atomic core (no `MODE_ID`) the tool reports
  `atomic_modeset=0`, establishes the mode with legacy `SetCrtc` and still
  flips atomically; `kms-flip-tool.md` documents both counter sets.
- **libdrm 2.4.134 vs 3.10 UAPI.** Uses
  `GETRESOURCES/GETCONNECTOR/GETENCODER/GETCRTC/GETPROPERTY/GETPROPBLOB/
  ATOMIC/CREATE_DUMB/MAP_DUMB/ADDFB2/RMFB/DESTROY_DUMB/PRIME_*`. Property
  blobs are used only when the CRTC `MODE_ID` property exists; the v4.0
  backport has neither, which is why the atomic modeset is capability-gated.
- **Master conflicts.** If X/Weston (or another process) holds DRM master,
  `drmSetMaster` prints a warning and flips fail with `EACCES`; run the KMS
  tests from a console session with no compositor (M1/M2), or stop the
  session first (M4).
- **Screen restore.** `kmsflip` saves the CRTC state before the first atomic
  commit and restores it (including the original fb/mode) on exit; on a
  KMS-only boot with `drm_fb_helper`, the console returns. With `--hold-ms`
  the pattern can be left visible before restoring. Restore uses atomic with
  a legacy `SetCrtc` fallback and is bounded by a 5 s watchdog.
- **dmesg ring.** Whole-log counters (`kms-dmesg.sh`) avoid needing root for
  a live `dmesg -w`; a ring wrap during a very long soak makes the delta
  negative — re-run after `dmesg -c` (root) rather than accepting it.
- **Mesa stack.** System Mesa 26.3.0 is present but is not the validated
  stack for the vendor panfrost UAPI; all tests default to mesa20-b. Use
  `MESA_MODE=system` only as a reference probe.
- **weston-simple-egl** must be built from source if required; it is not in
  the APK index. `kmscube` is the M2 reference client.
