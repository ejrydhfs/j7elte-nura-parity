# gpu31-node-probe — `--probe-only` node probe + live-device capture

Date: 2026-11-19. Slice: `code/gpu31/client` (+ device `/dev/dri/renderD128`).

## 1. What changed

`code/gpu31/client/pf7580-selftest.c` gains a `--probe-only PATH` mode
(`--probe-only=PATH` also accepted, `run_probe_only()`):

- Issues exactly four ioctls: `GET_PARAM(GPU_PROD_ID)`, `CREATE_BO(4K)`,
  `MMAP_BO`, `GET_BO_OFFSET`. Prints per-ioctl `OK`/`FAIL` lines with
  numeric rc + `pf7580_strerror` text.
- Never issues `SUBMIT` (a test `jc` would fault hardware), never calls
  `WAIT_BO`, never CPU-maps the BO. The only side effect is one throwaway
  4 KiB BO on the probed fd, freed by `close()`.
- Exit codes match the existing contract: `0` PASS, `1` FAILED,
  `2` node unavailable (open fails). `--device` and the mock suite are
  untouched.

Host verification: `make check` → `MOCK SUITE: PASS`; cross
`build-aarch64/pf7580-selftest` (static aarch64) runs the mock suite
under `qemu-aarch64` and `--probe-only /nonexistent-xyz` exits 2 with the
ABI dump. (`make check-aarch64` covers the mock suite; the probe-only
no-device path was checked with the cross binary under qemu directly.)

## 2. Device result — node went away mid-run (real capture)

Timeline (all under `flock /tmp/gpsdev.lock`, short holds):

- 02:25Z: device on kernel #37 `3.10.108 SMP PREEMPT`, `/dev/dri/card0`
  (226,0, `crw-rw---- root:video`) + `/dev/dri/renderD128` (226,128,
  `crw-rw-rw- root:video`) present, `/dev/mali0` absent (int4 panfrost
  image per SESSION-STATE). Static cross binary uploaded to
  `/tmp/pf7580-probe-device` (917 936 bytes, `aarch64-linux-gnu-gcc
  -static`), `--print-abi` over SSH returned the correct wire sizes
  (40/16/24/16/16/16, T720 `0x0720`).
- 02:29–02:31Z: `/tmp/pf7580-probe-device --probe-only
  /dev/dri/renderD128` started but printed nothing past the ABI dump and
  never returned: the flock-held SSH call hit the 120 s lock timeout and
  was reaped with no `RC=` line. No `SUBMIT` was issued by this binary
  (code path cannot emit it), so the hang is in open or one of the four
  safe ioctls — or in the concurrently-pending sibling submit-fix run.
- 02:32Z+: device is **rebooted / reverted to the M1 image** (cf25ed70,
  `#37 SMP PREEMPT Thu Nov 19 01:26:22 EST 2026`): `uptime` = 1–2 min,
  `/dev/dri` absent, `/sys/class/drm` holds only `version`, dmesg has zero
  `panfrost`/`drm-probe` lines. `/dev/mali0` (10,59, `crw------- root:root`)
  and `/dev/fb0` (29,0, `crw-rw---- root:video`) present, Xorg back in `S`
  (fbdev UI up). `/tmp/pf7580-probe-device` is gone (tmpfs reboot).
  Per `docs/SESSION-STATE.md` this revert was Main's response to the int4
  defects (card0 hangs Xorg in `D`; SUBMIT deadlocks uninterruptibly).

Consequence: **no per-ioctl results exist** — the probe never completed
against a live render node, and there is currently no render node to probe.
card0-vs-renderD128 open/auth comparison likewise could not run (both nodes
absent; `stat` shows mali0/fb0 only, `ls /dev/dri` = ENOENT).

## 3. card0 vs renderD128 open/auth (expected, not executed)

Recorded intent for the next int4 (render-only) boot, when both minors exist:

1. `stat` both nodes (maj:min, mode, group) as `user` (groups
   `wheel,netdev,plugdev,abuild`).
2. `open(O_RDWR)` each node as `user`; renderD128 is world-RW on int4
   (`crw-rw-rw-`) so open should succeed, card0 is `root:video`
   (`crw-rw----`) so open succeeds via `video`?? — user is NOT in `video`
   (groups above), expect `EACCES` unless root/acl; capture the errno.
3. On each opened fd, run `--probe-only` for the four per-ioctl results.
4. `dmesg | tail` after each step for driver-side errors.

## 4. How to re-run (next render-node boot)

```
flock /tmp/gpsdev.lock ssh user@172.16.42.1 "cat > /tmp/pf7580-probe" \
  < code/gpu31/client/build-aarch64/pf7580-selftest
flock /tmp/gpsdev.lock ssh user@172.16.42.1 \
  'chmod +x /tmp/pf7580-probe; /tmp/pf7580-probe --print-abi; \
   timeout 20 /tmp/pf7580-probe --probe-only /dev/dri/renderD128; echo RC=$?; \
   timeout 20 /tmp/pf7580-probe --probe-only /dev/dri/card0; echo RC=$?; \
   dmesg | tail -n 20'
```

Keep each SSH hold < 120 s; use `timeout 20` per probe invocation; do NOT
run `--device` (it CPU-maps) or any SUBMIT path on live hardware.

## 5. Files

- `code/gpu31/client/pf7580-selftest.c` — `--probe-only` flag + runner.
- `code/gpu31/client/build-aarch64/pf7580-selftest` — static aarch64 probe
  binary used for upload (untracked build artifact, rebuilt via
  `make CROSS_COMPILE=aarch64-linux-gnu- build-aarch64/pf7580-selftest`).
