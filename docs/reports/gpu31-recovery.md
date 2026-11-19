# Recovery: self-restoring BOOT slot without physical access (gpu31-recovery)

Date: 2026-11-19. Constraint update mid-task (Main): the phone IS UP
(kernel #35, SSH/RNDIS ok); stale "link down" claims elsewhere are
disregarded. Host-side work only: no reboot, no flash, no kernel build
(those belong to G31_Integrator / G31_M1Verify). Device reads done under
short `flock /tmp/gpsdev.lock` holds; one later probe timed out because
GPS/FM workstreams hold the link — treated as contention, not outage, and
the design needs no further live reads (all device facts below were
captured before the contention window).

## 1. Why not dual-boot or watchdog

- **No second slot exists.** S-Boot (closed Samsung loader) boots only BOOT
  (`/dev/block/mmcblk0p10`, 32 MiB). The only other bootable volume is
  RECOVERY (`mmcblk0p11`, TWRP), which must stay untouched — it is the last
  resort. There is no bootloader slot metadata to flip, and S-Boot is not
  modifiable. So "dual boot slots" is answered as *one slot + a cached
  known-good image*, not as bootloader A/B.
- **Watchdog does not recover by itself.** The tree has
  `CONFIG_WATCHDOG=y` + `CONFIG_S3C2410_WATCHDOG=y` (default timeout 15 s,
  `s3c2410_wdt.c:54,272`), `/dev/watchdog` + `/dev/watchdog0` exist on the
  phone (verified live: `crw /dev/watchdog`, `crw /dev/watchdog0`), and
  the 3.10 `watchdog_dev.c:462-474` close semantics were re-read (stop only
  on magic char or without `WDIOF_MAGICCLOSE`).
  But nobody feeds it today: no `WATCHDOG_NOWAYOUT` / at-boot kicker, and a
  watchdog reset just reboots *into the same bad image* — it converts a hang
  into a boot loop, not a recovery. The watchdog is therefore the *trigger
  complement* (a hung kernel resets instead of hanging forever), while the
  boot counter is the *decision* (after N bad boots, restore). Watchdog
  arming is deliberately NOT added to the bring-up kernel: on a first GPU
  milestone a too-eager kicker risks resetting a slow-but-healthy boot.
  Revisit after M1 boots are timed on hardware.
- **Chosen design: initramfs boot-count fallback on CACHE.**
  S-Boot rewrites the kernel cmdline (`/proc/cmdline` is always
  `console=ttySAC1`), so boot state cannot travel in bootargs — and the PMU
  INFORM scratch registers would need a kernel driver. The durable,
  bootloader-independent store is CACHE (`mmcblk0p21`, 200 MiB, otherwise
  unused): a `boot_count` file plus a `known-good-boot.img` copy (≤32 MiB).
  CACHE survives p10 flashes and is writable from both initramfs and
  userspace. USERDATA stays out of this path (it carries `/usr` + `/data`;
  coupling recovery state to the desktop filesystem risks exactly the
  full-filesystem failure mode the fallback must survive).

## 2. How it works

```
flash new GPU-test image to p10
  → reboot → initramfs: boot_count++ on CACHE
      → if boot_count > 3 AND valid cache copy → dd copy→p10, reset, reboot
      → else boot the flashed kernel normally
          → userspace boot-guard: X@720x1280 + milestone device alive?
              yes → boot_count = 0 (this boot was good)
              no  → counter left high; next boot increments again
```

- **FAIL_MAX = 3.** One bad boot is noise (forced reset during a GPS/FM
  cold-boot window, dv_mmc EMMC wobble — both documented realities of this
  bench). Two is suspicion. Three consecutive unmarked boots restore.
  Post-restore, the counter resets, so a persistently failing cache copy
  cannot livelock: worst case is one restore + normal boots that keep
  failing open (see below).
- **Fail-open everywhere.** CACHE unmountable/unformatted → boot normally.
  No/invalid cache copy (magic + size checked) → boot normally and say so
  on the console. `dd` restore fails → boot the flashed kernel. The
  fallback can only *add* a restore; it can never subtract a boot that
  would otherwise have happened.
- **Failure coverage (honest).** Covers everything where the kernel reaches
  initramfs: GPU/display regression (no `/dev/fb0`, X dead), milestone miss
  (`/dev/dri` absent), userspace wedge before the guard. Does NOT cover a
  completely unbootable Image (panic before initramfs) — that class stays
  TWRP-only via `code/kernel/recovery/RECOVERY.md §3`. The patch header
  states this boundary explicitly.

## 3. Files (all new, all under `code/kernel/recovery/`)

| File | Role |
|---|---|
| `initramfs-0003-bootcount-fallback.patch` | Patch text for `patches/initramfs/0003-…`. Initramfs hunk: mount CACHE, bump counter, conditional `dd` restore + `reboot -f` (sysrq-b fallback, matching the `j7elte-power` tile precedent), fail-open paths. Integrator copies it to `patches/initramfs/` and rebuilds the ramdisk after 0001/0002 (hunk offsets are indicative — re-cut at build). |
| `boot-guard.sh` + `boot-guard.initd` | Userspace OK marker. OpenRC service (after `xsession`); waits up to 300 s for `xdpyinfo` = 720x1280 AND the milestone device (`M1`: `/dev/dri/card0`+`/dev/fb0`; `M2`: `renderD128`+fb0; `M3`: render node + non-llvmpipe `glxinfo` renderer), then resets the CACHE counter via `rootshell` (note: `sudo -n` is broken on the device — `invalid configuration: /etc must be owned by root` — so the guard uses `/usr/local/bin/rootshell`, verified present). Install on the phone only. |
| `cache-snapshot.sh` | One-shot, no-reboot snapshot: `dd` live p10 → CACHE `known-good-boot.img`, verify `ANDROID!` magic, reset counter. Run over ssh after a build proves good. Writes land on p21, never on the 94%-full root fs. |
| `decide.py` | Host-side simulator of the exact counter state machine (`sim` traces, `seq` outcome table). Acceptance evidence, §5. |
| `cross-check.sh` | Host-side consistency suite: golden simulator traces, patch/guard content grep-checks, shared-tree-clean check. `CROSS-CHECK: ALL PASS` at hand-off. |
| (untouched) `RECOVERY.md`, `check-readiness.sh`, `known-good.sha256` | Still the TWRP last resort + pre-flash gate. Verified byte-identical (`readiness-untouched`). |

`docs/reports/gpu31-review.md` (commit 625ed18) findings were checked
against this slice: all REQUIRED items target `drm-backport` (§1–§4),
display/DT/config (§6–§8), PRIME (§10), kbase-compat (§11), and Mesa (§12)
slices — **none touch `code/kernel/recovery/`**, so no review-driven
changes apply here; this report states that explicitly per the correction
order. Shared kernel tree verified clean (`git status` in the tree: empty).

## 4. Operator runbook (post-Integrator)

1. Integrator applies `initramfs-0003-…` to `patches/initramfs/`, rebuilds
   ramdisk + boot.img (stock `dt.img`, existing procedure), M1Verify
   flashes *after Main's go*.
2. On the first *good* boot: run `cache-snapshot.sh M1` (short flock hold)
   → CACHE holds the known-good copy, counter 0.
3. Install `boot-guard.sh` → `/usr/local/bin/`, `boot-guard.initd` →
   `/etc/init.d/boot-guard`, `rc-update add boot-guard default`.
4. Flash GPU-test images as before. Three consecutive guard-silent boots
   self-restore p10 from CACHE and reboot — no hands, no TWRP.
5. If the fallback ever reports "no valid cache copy", re-run step 2; if a
   boot.img cannot reach initramfs at all, use TWRP per RECOVERY.md §3.

## 5. Acceptance evidence (host-side, recorded 2026-11-19)

Simulator (the shipped state machine, not a mock of something else):

```
$ python3 code/kernel/recovery/decide.py sim OK OK FAIL FAIL FAIL FAIL
boot  1  event=ok   count=0  -> boot-flashed+mark-ok
boot  2  event=ok   count=0  -> boot-flashed+mark-ok
boot  3  event=fail count=1  -> boot-flashed(count=1)
boot  4  event=fail count=2  -> boot-flashed(count=2)
boot  5  event=fail count=3  -> boot-flashed(count=3)
boot  6  event=fail count=0  -> RESTORE-CACHE+rereboot
final: boot_count=0 restores=1

$ python3 code/kernel/recovery/decide.py sim FAIL FAIL FAIL --fail-max=3 --no-copy
... (3 boots, no restore) final: boot_count=3 restores=0     # fail-open

$ python3 code/kernel/recovery/decide.py sim FAIL OK FAIL FAIL FAIL FAIL --fail-max=2
... one OK resets mid-sequence ... final: boot_count=1 restores=1

$ sh code/kernel/recovery/cross-check.sh
CROSS-CHECK: ALL PASS   (13 checks: 3 golden traces + 5 patch + 4 guard + tree-clean)
```

Live device facts grounding the design (read-only, flock-held):
`/dev/watchdog`+`/dev/watchdog0` present, `/dev/fb0`+`/dev/mali0` present,
kernel `3.10.108`, `sudo -n` broken → `rootshell` (all captured before the
GPS/FM contention window; no reboot/flash/build performed by this slice).

## 6. Residual risks

- CACHE has no redundancy: a corrupted ext4 on p21 disables the fallback
  (fail-open) until reformatted + re-snapshotted. Acceptable: TWRP covers.
- First-boot chicken-and-egg: until the first `cache-snapshot.sh` runs, the
  fallback is a counter with nothing to restore from (reports it, boots on).
- Guard tuning (`UI_TIMEOUT=300`, `FAIL_MAX=3`) is bench-chosen; re-tune
  after M1 boot timings exist on hardware.
