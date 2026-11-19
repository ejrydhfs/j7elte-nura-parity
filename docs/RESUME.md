# Resume runbook (device off USB, 2026-11-19)

Last known good: kernel #34 (`/tmp/bindboot.img` = known-good, `/tmp/kernel-boot.img` = GPU M1, unverified).
Recovery needs a physical power-cycle (hold Power ~10-15 s) and USB re-plug; the host has no fastboot/root access.

Once `ping 172.16.42.1` answers:
```
/tmp/on_reconnect.sh            # just report kernel + X state
```

## 1. GPS: chip reply to config id 0x01  (needs ONE cold boot)
```
rc-service j7elte-gpsd stop
/data/opt/stock/tmp/gpsesw-new -D ... --glme-stream -L 15000
```
Expect: banner `e0 00 ... 5f e0 01`, host cfg1 (225 B), then a chip reply to id 0x01.
Exit 7 = chip mute → next boot. Exactly one golden run per boot.

## 2. FM: audio evidence + RDS (BT must stay up)
```
bcmfm info ; bcmfm tune <station>        # proven: status 0x00 @ 94 MHz
sh code/userspace/fm/fm-mixer.sh          # ROUTE-DONE n=20
arecord -D hw:0,0 -f S16_LE -r 44100 -d 5 /data/fm.wav && analyse RMS/spectrum
```
Known: R-type CCs carry no values; `W1B(0x4D,1,8)` re-arms a 2.9k/s flood (see fm-backend.md §8).

## 3. GPU M1 boot test (only with physical access at hand)
```
sha256sum /tmp/kernel-boot.img   # 53acd61a…
# flash via p10 block node, reboot, then code/kernel/m1/m1-verify.sh
```
Fallback if it does not come up: `/tmp/bindboot.img` (known-good) via another physical cycle.

## 4. Modem: TZ verdict from `docs/reports/modem-tz.md`; no device action until then.

## Repo
All work committed locally (29 commits ahead of origin). `git push` fails 403: the host token is
`alosslessdev`, the remote is `ejrydhfs/j7elte-nura-parity` — needs the right credentials.
