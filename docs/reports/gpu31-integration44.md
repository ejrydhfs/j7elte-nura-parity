# gpu31-integration44 — WAIT_BO maps a 0-jiffy wait to -ETIMEDOUT (Mesa's BO-cache poll no longer reports busy BOs ready); 28/28 gates

Date: 2026-11-22. Owner: G31_Integrator44. Phone: on int43; this
ticket builds host-side only. No flash per ticket (Main/Storm58 flash
and run acceptance).

Consumes **98640d5** (gpu31-storm58): `dma_resv_wait_timeout()` returns
>0 (idle, jiffies left), 0 (budget ran out) and <0 (error). The WAIT_BO
handler mapped only <0 to an error and returned 0 for **both** 0 and >0,
so a 0-jiffy poll — exactly what Mesa's BO-cache fetch issues
(`panfrost_bo_wait(bo, 0, RW)`) — was always told "ready", even for a BO
an in-flight submit still owned. The report's proof on int42:
`hang_inject=4` + `job_timeout_ms=60000` kept a submit fence pending
60.1 s (`EV start dt=0` → `EV timeout dt=60137825 us`) while the same
GEM BO (`h=6`, 64 KiB, `gpu=0x24bd000`) was PUT/FETCHed 4 times inside
that window. Driver 0001 at build time: sha256
`1c295834debcc477e209aa0bdda5862fb251d4a026abd0e78f9a24af23e144c0`,
repo HEAD `98640d5`; the ioctl.c new-file hunk is now `+1,388`.

## 0. TL;DR

- **Build green**: fresh scratch tree on the external disk, `-j4`
  Image, RC 0, 0 `error:` lines, 5397-line log ending
  `OBJCOPY arch/arm64/boot/Image`; 499 warnings (same ambient count as
  int41–int43).
- **28/28 gate banners PASS, 0 FAIL**: the 27 int43 gates unchanged
  plus new **6ae**.
- **New 6ae**, in the applied tree, the mirror, `ioctl.o` and the Image:
  - exactly one `return -ETIMEDOUT;` in
    `panfrost7580_ioctl.c` (applied `:300`), inside
    `panfrost_ioctl_wait_bo` (`:259`), after
    `timeout = p7580_gem_wait(dev, file_priv, args->handle, timeout, 1);`
    (`:284`), guarded by `if (timeout == 0)` (`:299`), with
    `if (timeout < 0) return (int)timeout;` (`:301–302`) and
    `return 0;` for the idle (>0) case below it;
  - compiled: `bl <p7580_gem_wait>` at `0x420`, then
    `mov w1, #0xffffff92` (−110 = −ETIMEDOUT) at `0x42c` on the
    not-idle path;
  - host **T11** pins the three-way mapping (`0 → -ETIMEDOUT`,
    `1`/`250 → 0`, `-EINTR → -EINTR`); T1–T10 unchanged and green.
- **Every int43 invariant still holds**: one BO publish at SUBMIT inside
  `df->serial_lock` (`:2092`), no `publish_excl`/run_job publish,
  queue-time `get_excl` leg, fbd/jc_age capture with no `desc[10]` read,
  PAGE dump offset math, the 750 ms budget + hang_inject knobs, the
  ev-ring/recovery rules.
- **Config**: 7/7 d0+d4 fragment symbols survive `olddefconfig`
  verbatim.
- Outputs: `Image-int44` `bf834883…`, `kernel-boot-int44.img`
  `e32de3ac…` (§4).

## 1. Build

Script `code/gpu31/series/build-int44.sh` (3263 lines; sha256
`8d8a0269c32d5aeb1ac27c25584d04880024f816b4004be57a58d1f70c9af381`),
a copy of int43 with int44 names, step **6ae** and a rewritten header.

Fresh `cp -a` of the pristine tree →
`…/j7k/gpu31/scratch-int44` (birth 17:25:11, tree mtime 17:29:25);
scratch and stage deleted after the pack. `TMPDIR` and every output are
on the external disk; nothing was written to `/tmp` or `/`.

Patches, each `git apply --check` first: backport shims 0001–0005,
Mode-B render node 0001, dmabuf CMA writecombine 0001, driver 0001+0002.
Driver 0001 (`1c295834…`) is hashed before apply and re-checked at 6v;
the new-file hunk `+count` integrity check ran first and passed (this is
the check that catches the 372→388 header drift class).

Config: pmaports `config-samsung-j7elte.aarch64` + `d0-m1-minimal` +
`d4-panfrost7580`, then `yes "" | make ARCH=arm64 olddefconfig`
(7/7 closure).

Command: `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
KCFLAGS="-Wno-error" -j4 Image`.

- Build log: `…/logs/gpu31-int44-build.log` (5397 lines, 0 `error:`,
  499 `warning:`; tail `SORTEX vmlinux`/`SYSMAP System.map`/
  `OBJCOPY arch/arm64/boot/Image`). Make RC 0 and Image mtime
  (17:29:25, gated) ≥ build start are enforced.
- Gate stdout: `…/logs/gpu31-int44-gates.log` (920 lines, 28 PASS
  banners, 0 `GATE FAIL`).
- Evidence: `…/logs/gpu31-int44-evidence.log` (963 lines).
- Host tests: `…/logs/gpu31-int44-hosttest.log`
  (`submit_fix_test: all checks passed` — T1–T11 —,
  `chainsplit_test: all checks passed`),
  `…/logs/gpu31-int44-schedhost.log` (2× `409 checks, 0 failures`,
  `gates: all PASS`, kcheck PASS),
  `…/logs/gpu31-int44-jobtest.log` (`job_test: all checks passed`).
- Pack: stock `dt.img` + reference `ramdisk-new.gz` (never rebuilt) +
  the built Image → 27348992 B = kernel 18209976 + ramdisk 8822392 +
  dt 313344 (+3280 header).

Source provenance (evidence:421): `repo HEAD=98640d5 driver-0001
sha256=1c295834…`. The report commit does not touch driver 0001; any
later edit to it invalidates §4.

Freshness: the pristine tree has no `drivers/gpu/drm/drm_sched.c` and no
`drivers/gpu/drm/panfrost/` directory. `Image-int44` has the same
18209976-byte size as int43; 5,349,012 bytes differ in the common
prefix.

## 2. What changed in the image

int43 fixed who publishes BO exclusivity; int44 fixes what a waiter is
told when it asks without blocking. Together they close both halves of
the storm: the right fence is on the BO (int43), and a poll that finds
it unsignalled says so (int44).

| Applied site | Change |
|---|---|
| `ioctl.c:284` | `timeout = p7580_gem_wait(dev, file_priv, args->handle, timeout, 1);` unchanged — the raw `dma_resv_wait_timeout()` result. |
| `ioctl.c:299–302` | `if (timeout == 0) return -ETIMEDOUT;` then `if (timeout < 0) return (int)timeout;`; the `return 0;` below stays for >0 (idle). v5.2 `panfrost_wait_bo` contract. |
| `submit_fix_test.c` T11 | `model_wait_bo_ioctl(long)`: 0 → `-ETIMEDOUT` (busy), 1/250 → 0 (idle), `-EINTR` → `-EINTR`; `run_t11()` in main. |

No other handler changes: SUBMIT, GET_PARAM, CREATE/MMAP, WAIT and the
debug ioctls are byte-identical to int43, and the driver patch's
submit.c/job.c/gem.c/mmio sections are untouched by this commit.

## 3. Gate evidence (computed inside the build)

### 3.1 Prior gates: 27/27 PASS, unchanged

All int43 gates pass without retarget: 6j (single serial BO publish,
run_job free of `add_excl`), 6z (one publish site + mirror), 6w (3
`p7580_drv_gpu_read` sites), 6ab (`off=%03x` PAGE, `job->fbd`, no
`desc[10]`), 6ad (publish ordering, capture, ledger), 6aa (always
power-on recovery), 6ac (750 ms budget + hang inject), and the older
layout/ABI/IRQ/fence/MMU gates. Unchanged constants: ops=24/slot=232/
ran=236, cfg `0x83700`, 56/192/192/64/32, `P7580_BO_AGE_SLOTS 64`,
`P7580_JOB_TIMEOUT_MS 750u`.

### 3.2 New 6ae: WAIT_BO result mapping (PASS)

Applied-tree anchors and the gated block:

```
ioctl.c:259   int panfrost_ioctl_wait_bo(struct drm_device *dev, void *data,
ioctl.c:284       timeout = p7580_gem_wait(dev, file_priv, args->handle, timeout, 1);
ioctl.c:299       if (timeout == 0)
ioctl.c:300           return -ETIMEDOUT;
ioctl.c:301       if (timeout < 0)
ioctl.c:302           return (int)timeout;
ioctl.c:303       return 0;
```

The gate proves exactly one `return -ETIMEDOUT;` in the whole file, that
it is inside `panfrost_ioctl_wait_bo`, and the line order
`gem_wait(284) < ==0(299) < return(300) < <0(301)`; it also pins the
negative propagation, the idle `return 0;`, and that no second
ETIMEDOUT leaks from this commit (the patch's other `-ETIMEDOUT`
returns belong to the MMU/GEM wait helpers and are unchanged).

Compiled proof (`ioctl.o`, -O2):

```
panfrost_ioctl_wait_bo:
    3b4: cbnz w3, 444
    420: bl   <p7580_gem_wait>
    42c: mov  w1, #0xffffff92      // #-110 = -ETIMEDOUT
```

Mirror: the driver 0001 `if (timeout == 0)` line is followed by exactly
one `return -ETIMEDOUT;` (the patch contains three such returns across
different files, so the gate scopes the pair, not a global count), and
`return (int)timeout;` is still present.

Host T11 output is part of the green `submit_fix_test` run; together
with T9 (recovery always powers on) and T10 (publish order) the host
model now covers both storm halves.

### 3.3 Config closure (d0+d4)

7/7 fragment lines survive `olddefconfig` verbatim
(`CONFIG_MODULES`, `DRM`, `DRM_KMS_HELPER`, `DRM_EXYNOS`,
`DRM_EXYNOS_DMABUF`, `DRM_EXYNOS_G2D`, `CONFIG_DRM_PANFROST7580=y`).

## 4. Outputs (sha256)

- `Image-int44` (18209976 B):
  `bf834883694a588b2b9af240896b1354eb5ba42674f03ab17ab7e70fdbe8c6a9`
- `kernel-boot-int44.img` (27348992 B):
  `e32de3ac213b4f7d843a299ecc2728b6f0fb07e924c9ce5bc2dc9b45e617155b`
- build log:
  `64918f24cfbd94d84d6092ee3159a553ddc9672f0eeb39af0b10120969e8dc6d`
- gate stdout log:
  `6cd926e66f3df94383a6ece851f2d46b17725b0a013c14f71e665c351333f737`
- evidence log:
  `1b6c0fae97aa242180c2a855e22c7b76bd97ab5ee4ea1b2a3eb844ff491122b5`
- host-test log:
  `91007da2edb706a24eb9523859d7fea5cffbc64519bd157b15d1f7ce9f1d1567`
- sched-host log:
  `3c38146b7d22d3e54d2c6299ff1b631d884d1652369f7c6138e31d067da69d38`
- jobs-host log:
  `ada85c46c81086468fc7cf33cff9ce53e221b9dbc59754e2433ec639ae10d68b`
- script:
  `8d8a0269c32d5aeb1ac27c25584d04880024f816b4004be57a58d1f70c9af381`

All paths under
`/run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/`.
No flash per ticket.

## 5. Silicon acceptance (Main/Storm58, after flashing int44)

1. **10×300 acceptance** (report §6 loop): `dmesg -c`, `ev_arm=1`, ten
   runs of 300 frames at 256² no-readback, plain, under `flock`. Pass =
   every run `ERR=0`, `TIMEOUT=0`, `EVRING=0` and wall 1.9–2.1 s.
2. **Deterministic WAIT_BO proof** (`PAN_FBD_DEBUG=1` +
   `hang_inject=4`): with the submit fence held pending, the slab
   holding `jc` must **not** be FETCHed again. On int42 the same BO was
   PUT/FETCHed 4 times in 60 s; on int44 the first FETCH after the mask
   must fail with `ETIMEDOUT` and userspace must retry rather than be
   handed the BO. Expect exactly one `recover-done … cycle=1` when the
   timeout fires, and no `jc_age` 1–2 capture on a live fence.
3. **Regression sanity**: a plain 30-frame 256² readback and a 10-frame
   512² read must still pass; a `-ETIMEDOUT` surfaced to a client that
   asks with 0 timeout while everything is idle would mean the mapping
   is wrong (it must only fire when `dma_resv_wait_timeout` returned 0).

## 6. Script notes

- The patch contains three `return -ETIMEDOUT;` lines in total (MMU/GEM
  wait helpers in earlier slices), so the mirror gate scopes the
  `if (timeout == 0)` → `return -ETIMEDOUT;` pair instead of counting
  globally; the applied-file count is the single-site gate.
- 6ae was dry-run against the patched tree and its real `ioctl.o`
  before the final build: the compiled form is
  `mov w1, #0xffffff92` (not `w0`), and the load/store offsets in
  `panfrost_ioctl_wait_bo` place the return after the `gem_wait` call,
  which the gate now asserts by disassembly line order as well.
- Retained: hunk `+count` integrity (ioctl.c 372 → 388 verified),
  RC/mtime freshness, defectb loop proof, config closure, driver-0001
  drift re-check, and every int43 gate.
