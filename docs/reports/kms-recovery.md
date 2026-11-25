# One-command recovery to the known-good J7 kernel (int49)

Date: 2026-11-25. Host tool: `code/gpu31/kms/tools/recover/recover.sh`.
Known-good image: `kernel-boot-int49.img`, 27 348 992 B,
sha256 `62377e6573ed5045e54c8afc4b229b72961e11bc8fe774106155314248b15e73`
(kernel `3.10.108 #37 SMP PREEMPT Sun Nov 22 23:16:55 EST 2026`).

Scope: restore the BOOT partition (`mmcblk0p10`) and prove the desktop is
back. The script never touches RECOVERY (`mmcblk0p11`), USERDATA, CACHE,
DT or any other partition, and never runs `sysrq-b`.

## 1. State machine

The script classifies the phone first, then acts:

| State | Evidence | Action |
|---|---|---|
| `TWRP` | an adb device whose `ro.twrp.version` is non-empty | push image, sha256-verify on phone, `dd` p10, read back + sha256, reboot, verify |
| `HEADLESS_KMS` | ssh answers, `uname -r`/`uname -v` are not int49 (KMS test kernel) | scp image, sha256-verify on phone, `dd` p10, read back + sha256, reboot, verify |
| `DEGRADED` | `uname -v` **is** int49 but `/dev/fb0`, Xorg, fbwin or 720x1280 is missing | diagnose, print the `xsession start` remedy, exit 1; only `--force` reflashes the same kernel |
| `KNOWN_GOOD` | int49 + `/dev/fb0` + `virtual_size=720,1280` + Xorg + fbwin + `xdpyinfo=720x1280` | verify only, exit 0, no writes (idempotent) |
| `WEDGED` | no ping and no TWRP adb device | print the battery-pull + TWRP key sequence, exit 3 |
| `WEDGED_SSH` | ping answers but ssh does not | same instructions with that diagnostic, exit 3 |
| `LOCK_BUSY` | `flock` conflict while probing (another workstream holds the device) | exit 5; retry, do not treat as an outage |

`--dry-run`/`--status` performs detection (and image verification when a
recovery is possible) only: no scp/adb push, no dd, no reboot. It is the safe
way to inspect the bench.

### adb selection is deliberately strict

This host also has an emulator (`emulator-5554`) and, at test time, an
unrelated Android phone (`ASNUVB6126005556`, product `HNMTN-Q1`) attached.
The script ignores emulator serials outright and accepts a device as TWRP
**only** when `getprop ro.twrp.version` is non-empty. It does not accept
`ro.boot.mode=recovery` or `/sbin/recovery` alone, because stock Android
recovery also reports those; that prevents ever `dd`-ing a random phone's
BOOT slot. Ignored targets are printed as evidence.

## 2. Usage

```sh
cd /tmp/j7repo
code/gpu31/kms/tools/recover/recover.sh --dry-run   # read-only check + plan
code/gpu31/kms/tools/recover/recover.sh             # asks before flashing
code/gpu31/kms/tools/recover/recover.sh --yes       # non-interactive
code/gpu31/kms/tools/recover/recover.sh --force     # reflash even if healthy
```

Exit codes: `0` known-good/recovered, `1` not healthy / verification failed
(`DEGRADED` without `--force`, or post-flash verification), `2` usage,
`3` wedged (human needed), `4` image missing/invalid, `5` aborted
(confirmation, lock contention, partition missing).

Environment overrides: `DEV` (`user@172.16.42.1`), `HOST`
(`172.16.42.1`), `LOCK` (`/tmp/gpsdev.lock`), `LOCK_WAIT` (240 s),
`INT49_IMG`, `INT49_SHA256`, `KMS_SSH_ASKPASS` (`/tmp/ap.sh`), `SSH_OPTS`,
`BOOT_RUN` (`/dev/mmcblk0p10`), `BOOT_TWRP` (`/dev/block/mmcblk0p10`).

Image discovery order: `--image`, then `INT49_IMG`, then
`/tmp/kernel-boot-int49.img`,
`/run/media/*/*/j7k/gpu31/kernel-boot-int49.img`,
`$HOME/j7k/gpu31/kernel-boot-int49.img`, `/data/kernel-boot-int49.img`.
On this bench the only copy is on the mounted disk:

```sh
cp /run/media/asdf/db69437b-ed35-414a-8bd1-bade16b06830/j7k/gpu31/kernel-boot-int49.img \
   /tmp/kernel-boot-int49.img
```

Then `sha256sum /tmp/kernel-boot-int49.img` must print the pin above.

## 3. What each path actually runs

All ssh runs as `flock /tmp/gpsdev.lock` + `SSH_ASKPASS=/tmp/ap.sh
SSH_ASKPASS_REQUIRE=force setsid -w ssh …` (the repo convention). Root on the
running system is `/usr/local/bin/rootshell` (sudo is broken:
`/etc must be owned by root`).

### 3.1 Running-system (HEADLESS_KMS; DEGRADED only with `--force`)

```sh
# stage under /data (root fs is 99 % full)
mkdir -p /data/recover
scp kernel-boot-int49.img user@172.16.42.1:/data/recover/kernel-boot-int49.img
# sha256 on the phone must equal the pin, size must equal 27348992, else abort
ssh user@172.16.42.1 'sha256sum /data/recover/kernel-boot-int49.img'
# write + sync
ssh user@172.16.42.1 \
  "rootshell sh -c 'dd if=/data/recover/kernel-boot-int49.img of=/dev/mmcblk0p10 bs=512 conv=fsync; sync'"
# read back exactly 53416 x 512 B from p10 and compare sha256 again
ssh user@172.16.42.1 \
  "rootshell sh -c 'dd if=/dev/mmcblk0p10 bs=512 count=53416'" | sha256sum
# reboot only after both hashes match
ssh user@172.16.42.1 "rootshell sh -c 'sync; reboot'"
```

`/dev/mmcblk0p10` (259,2, 33 554 432 B) is the running-system node;
`/dev/block/mmcblk0p10` does **not** exist there and `dd` to it writes into
devtmpfs. The script checks `test -b` before writing.

### 3.2 TWRP (adb)

```sh
adb push kernel-boot-int49.img /data/recover-int49.img
adb shell 'sha256sum /data/recover-int49.img || busybox sha256sum … || toybox sha256sum …'
adb shell 'dd if=/data/recover-int49.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync'
adb shell 'dd if=/dev/block/mmcblk0p10 of=/data/recover-readback.bin bs=512 count=53416 conv=fsync'
adb shell 'sha256sum /data/recover-readback.bin'     # must equal the pin
adb shell reboot
```

In TWRP the node is `/dev/block/mmcblk0p10` (the script accepts
`/dev/mmcblk0p10` there too if that is what TWRP exposes). If neither node
exists the script aborts with the exact prior-art fallback; it never creates
a node by guessing:

```sh
adb shell 'ls -l /dev/block/mmcblk0p*'      # inspect first
adb shell 'mknod /dev/block/mmcblk0p10 b 259 2'
```

### 3.3 DEGRADED (int49 up, session down)

The kernel is already the known-good one, so flashing it again cannot fix a
missing Xorg/fbwin. This state is common **by design** on this bench:
`rc-service xsession stop` frees the framebuffer for KMS/Mesa work. The
script therefore prints the probe table and the remedy and exits 1; it does
not touch the device:

```sh
ssh user@172.16.42.1 "rootshell sh -c 'rc-service xsession start'"
# or reboot the phone
recover.sh --force        # only if the session cannot start at all
```

`--force` reflashes int49 through the same verified ssh path. Note that
`--yes` alone does **not** override this: a deliberate `xsession stop` must
not be turned into a flash+reboot by an automation retry.

### 3.4 WEDGED (no ping, no USB)

The script prints, and a human performs:

1. Remove the back cover, pull the battery, wait 10 s. (If the battery is
   not accessible: hold Power ~20 s.)
2. Reinsert the battery; do not press Power.
3. Hold **VolUp + Home + Power together** until the TWRP splash, then release.
4. Connect USB; `adb devices -l` must show a physical device (not
   `emulator-*`).
5. Re-run `recover.sh --yes`.

## 4. Safety invariants (enforced in code, not just documented)

1. **No dd before sha256.** The image must be a regular file, > 8 MiB,
   <= 33 554 432 B, start with `ANDROID!`, and hash to the pinned value.
   Failure exits 4 and prints no flash commands.
2. **Device-side hash before dd.** scp/adb-push is followed by a second
   sha256 on the phone; mismatch aborts before `dd`.
3. **Readback hash after dd.** The exact image length (53416 sectors) is
   read back from p10 and hashed. A mismatch exits 5 (without rebooting) and
   says the slot is written but unverified.
4. **Sync discipline.** `bs=512 conv=fsync` plus an explicit `sync` on both
   paths. `sysrq-b` is never used; reboot is `sync; reboot`.
5. **Correct partition only.** `BOOT_RUN=/dev/mmcblk0p10` on the running
   system, `BOOT_TWRP=/dev/block/mmcblk0p10` in TWRP; both `test -b`-checked;
   size cross-checked against `/sys/class/block/mmcblk0p10/size` when
   readable. RECOVERY (p11) is never referenced.
6. **Idempotent.** On a healthy int49 phone the script only verifies and
   exits 0. `--force` is required to reflash the same image. Re-running after
   a partial failure re-stages and re-verifies from scratch; no state file is
   trusted.
7. **Read-only dry run.** `--dry-run` never writes, pushes, or reboots.
8. **Contention is not an outage.** `flock -E 42` distinguishes a busy
   `/tmp/gpsdev.lock` from an unreachable phone.

## 5. Post-restore verification

A recovery is only reported as such when all of the following are observed
over ssh (retried for up to `--wait` seconds, default 300, because Xorg comes
up seconds after sshd):

| Check | Required value |
|---|---|
| `uname -r` | `3.10.108` |
| `uname -v` | `#37 SMP PREEMPT Sun Nov 22 23:16:55 EST 2026` |
| `/dev/fb0` | present |
| `/sys/class/graphics/fb0/virtual_size` | `720,1280` |
| `pgrep -x Xorg` | running (`Xorg :0 -config …/99-fbdev.conf`) |
| `pgrep -x fbwin` | running |
| `DISPLAY=:0 xdpyinfo` dimensions | `720x1280` |

`/dev/dri/card0` is printed as KMS-boot evidence but is not required for the
known-good state (int49 has no KMS; it may expose `renderD128` for panfrost).
On timeout the script prints the last observed table and exits 1.

## 6. Read-only bench test (2026-11-25)

Nothing was reflashed. Two states were observed on the same boot because a
parallel workstream stopped the desktop session mid-test.

- **KNOWN_GOOD** (first run, X up): `recover.sh --dry-run` →
  `state: KNOWN_GOOD`, all seven checks `OK` (`uname -v` = `#37 SMP PREEMPT
  Sun Nov 22 23:16:55 EST 2026`, `virtual_size=720,1280`, Xorg + fbwin +
  nura-shell up, `xdpyinfo=720x1280`), image found at the mounted-disk path,
  size/sha256 verified against the pin, exit 0, no plan emitted.
- **DEGRADED** (later runs, session stopped): same kernel (`#37 …`), but
  Xorg/fbwin/nura-shell are gone, `/dev/fb0` and `720,1280` remain, no KMS
  modules are loaded, uptime 27 min. `recover.sh --dry-run` → `state:
  DEGRADED`, four FAIL lines, the `rc-service xsession start` remedy printed,
  exit 1, no plan. `recover.sh --dry-run --force` → same probe, then image
  verification and the exact scp/dd/readback/reboot plan, exit 0.
- Independent read-only proof that p10 already holds int49:
  `ssh … "rootshell dd if=/dev/mmcblk0p10 bs=512 count=53416" | sha256sum`
  → `62377e6573ed5045e54c8afc4b229b72961e11bc8fe774106155314248b15e73`.
- `HOST=192.0.2.1 … --dry-run` → `state: WEDGED`, printed the battery-pull
  + TWRP sequence, exit 3 (no device access attempted beyond ping).
- Wrong image (`--image /tmp/boot.img`, stock Android boot) → sha256
  mismatch reported; on the healthy phone, no force meant exit 0 without
  flashing; `--force` makes it exit 4 before any device write.
- TWRP branch exercised with a stub `adb` (`ro.twrp.version=3.7.0_9-0`):
  detected `TWRP`, verified the real int49 image, printed the exact push/dd/
  readback/reboot plan, exit 0.
- Stub stock-recovery device (`ro.boot.mode=recovery`, no
  `ro.twrp.version`) → correctly **not** TWRP; the script ignored it and
  reported WEDGED. The real bench targets `emulator-5554` and
  `ASNUVB6126005556 (HNMTN-Q1)` were likewise ignored and listed as such.
- Editing/parsing paths were exercised against `sh -n`; the only remaining
  unexecuted branch is a real TWRP session (the stub proves detection and
  plan selection, not adb push/dd against TWRP).

## 7. Notes and limits

- An actual flash must still follow repo policy (Main's go; no cold boot on
  a whim). The script asks by default and refuses to flash without `--yes`
  when stdin is not a tty; `--dry-run` first is the recommended habit.
- Before running with `--yes`, check that no workstream has deliberately
  stopped the session or loaded KMS modules (`pgrep -af 'Xorg|kmsflip'`,
  `lsmod`). The tool classifies that as DEGRADED and will not flash without
  `--force`, but a human should still avoid interrupting a live test.
- The script restores the *image*, so a botched p10 is covered. It does not
  repair a broken CACHE boot-count fallback (see `gpu31-recovery.md`) or a
  damaged TWRP; p11 is out of scope by design.
- The pin is the identity of int49, not of "whatever is newest". When the
  known-good image is deliberately superseded, update
  `DEFAULT_INT49_SHA256`, `INT49_KVER` and the size/readback constant in
  the script and this report together; `--sha256` exists only as a
  documented escape hatch and prints a warning.
