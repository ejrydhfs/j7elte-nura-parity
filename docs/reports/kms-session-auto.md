# KMS test boots: self-arming watchdog + dmesg logger

A KMS test kernel can wedge in `insmod`, in the first atomic commit, or while
the UI is being torn down.  Until now the safety net was manual
(`/home/user/kmstest.sh` armed `wdpet.sh` and `logd.sh` from a live SSH
session), which cannot protect the *boot* itself: if the kernel dies before
SSH is up, the phone needs a battery pull.

This slice makes the protection part of the boot, gated by a marker file, and
without touching any OpenRC service or runlevel.  Nothing here flashes,
reboots, or changes the daily image's behaviour while the marker is absent.

## 1. Files

| file | installed as | purpose |
|---|---|---|
| `code/gpu31/kms/session/kms-test-arm` | `/usr/local/sbin/kms-test-arm` | arms the hardware watchdog pet and the filtered-dmesg logger; `boot`/`start`/`stop`/`status` |
| `code/gpu31/kms/session/kms-load.sh` | `/usr/local/sbin/kms-load.sh` | `<rev>` driver load: stop UI → arm → insmod helper → insmod driver → print nodes/IRQs, non-zero on any failure |
| `code/gpu31/kms/session/kms-test-install` | (run from the tree) | installs both scripts and adds/removes the single inittab hook line |

Both runtime scripts are POSIX `sh` and depend only on `/bin/sh`, `/proc`,
`/dev/watchdog` and a writable trace path.  `kms-test-arm` never invokes
`rc-service`/`rc-update`; `kms-load.sh` uses `rc-service xsession stop` only
because stopping the UI is the step under test.

## 2. Where the boot hook sits, and why

The J7 kernel is built with `CONFIG_DEVTMPFS=y` but
`# CONFIG_DEVTMPFS_MOUNT is not set` (`build/final-config:964`), so
`/dev/watchdog` does **not** exist before OpenRC's `devfs` service has mounted
devtmpfs.  The root filesystem is also mounted read-only until the `boot`
runlevel's `root` service remounts it.  Consequently:

* `/etc/local.d/*.start` is too late: the `local` service is declared
  `after *`, so it runs after `xsession`, the network and every other default
  service — exactly the window we need to protect against.
* A new OpenRC service would be both too late and a service change.
* The earliest safe point that is not inside the OpenRC service graph is a
  second `::sysinit:` line in `/etc/inittab`, after `openrc boot` and before
  `::wait:/sbin/openrc default`.

`kms-test-install install` inserts exactly that:

```
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
# kms-test-mode: self-arming watchdog before the default runlevel
::sysinit:/usr/local/sbin/kms-test-arm boot
::wait:/sbin/openrc default
```

BusyBox init runs `sysinit` lines in file order and waits for each one, so the
hook is the first thing that runs with `/dev`, `/proc`, `/sys`, `/run` mounted
and `/` read-write — and the last point before `xsession` starts.  The hook
exits 0 unconditionally; it must never fail the boot sequence.

## 3. Enable / disable

The marker file is the only switch.  No service defaults change, ever.

```
# enable: arm watchdog + logger at every boot from now on
touch /etc/kms-test-mode

# disable: next boot is the ordinary daily boot (hook becomes a no-op)
rm /etc/kms-test-mode
```

The hook line itself stays installed; without the marker it runs
`[ -e /etc/kms-test-mode ] || exit 0`.

```
/usr/local/sbin/kms-test-arm status     # marker, hook, watchdog device, pids, trace
/usr/local/sbin/kms-test-arm start      # arm this boot without rebooting (works without the marker)
/usr/local/sbin/kms-test-arm stop       # stop pet + logger; watchdog is disarmed by the close
/usr/local/sbin/kms-load.sh int82       # the actual test load (see §4)
```

`kms-test-install uninstall` removes the inittab hook (and restores the
previous inittab verbatim; a one-time copy is kept as
`/etc/inittab.pre-kms-test`).  `uninstall -p` also deletes the installed
scripts.  The marker is never touched automatically.

## 4. `kms-load.sh <rev>`

`rev` accepts `int82`, `82`, or a full `exynos-drm7580-int82.ko` path; modules
are read from `$KMS_MODDIR` (default `/home/user`).

Order of operations — arming comes **before** the first destructive step:

1. preflight: both `.ko` files exist; no KMS module is already loaded
   (`/proc/modules`, so a *built-in* `drm_kms_helper` does not falsely block a
   test module of the same name);
2. `kms-test-arm start` (bounds: `timeout 30`), so a wedge in any later step
   resets the SoC;
3. `rc-service xsession stop` (`timeout 30`) plus a bounded sweep
   (`start-ui.sh`, `fbwin`, `Xorg`: TERM, ≤5 s wait, KILL), because a surviving
   `fbwin` holds the framebuffer and the modeset path;
4. `insmod drm_kms_helper-<rev>.ko` (`timeout 30`), then
   `insmod exynos-drm7580-<rev>.ko` (`timeout 60`) — helper first, as before;
5. node check: `/dev/dri/card0`, `/dev/fb0`/`sys/class/graphics/fb0`;
6. IRQs: `decon`/`dsim` lines plus a 1 s delta of the `decon_fb` counter, so a
   dead frame IRQ is visible immediately;
7. dmesg tail (`decon|exynos|drm|teardown|vsync`), the trace path and the
   harness log.

Exit status: `0` only if every step succeeded **and** `card0` exists and the
`decon_fb` counter advanced.  `1` on any failure (each failure is printed, and
the run continues far enough to show diagnostics where that is safe).  `2` on
usage errors.  If the harness cannot be armed the script refuses to touch the
modules; `--force` loads anyway (unprotected, warned explicitly).

`timeout` only bounds userspace: a kernel that stops scheduling cannot be
bounded from userspace.  The watchdog covers that case, which is why the
harness is armed first.

## 5. Failure modes

| situation | observed behaviour | recovery |
|---|---|---|
| marker absent, hook installed | hook exits 0 before `xsession`; no pet, no logger, no trace writes. Daily boot unchanged | `touch /etc/kms-test-mode` |
| marker present, hook not installed | boot is unprotected; `kms-test-arm status` reports the missing hook and exits 1 | run `kms-test-install install` |
| marker present, watchdog armed, then kernel wedges | pet stops pinging → SoC resets after the 30 s DT timeout; the logger's last snapshot survives in `/home/user/kms.trace` (tmp+rename) | wait ~30 s; no battery pull |
| kernel wedges while `insmod` runs | `timeout` cannot fire (task is stuck in the kernel); the watchdog resets | as above; the reload after reset uses the next rev |
| `insmod` fails normally (EEXIST, bad reloc) | `kms-load.sh` prints the step, its output and dmesg; exit 1. For `drm_kms_helper` it also says when the helper is built into the kernel (module must be `=m` for the test kernel) | fix the build / unload, retry with the clean-up line printed |
| UI survives `rc-service xsession stop` | sweep TERMs then KILLs `start-ui.sh`/`fbwin`/`Xorg`; if something still holds it, `kms-load.sh` aborts before `insmod` (exit 1) | find the holder by hand; loading over a live fbdev client is the known hang |
| `/dev/watchdog` missing or already held by another pinger | `kms-test-arm start` reports "NOT armed" and exits 1; `kms-load.sh` aborts unless `--force` | find the other opener (`lsof`/`fuser`), or use `--force` for a short unprotected run |
| two `start`es (boot hook plus manual) | second one sees the live lock and exits before opening the device; no double pet, no double logger | none needed |
| stale lock after a `SIGKILL` of the daemons | lock pid is dead → the next `start` takes the directory over | none needed |
| `stop` | TERM, up to 6 s grace; magic-close byte + close disarm the watchdog (`CONFIG_WATCHDOG_NOWAYOUT` is not set). A SIGKILL still closes the fd, but is reported as an anomaly (exit 1) | re-`start` if `stop` reported a kill |
| clean shutdown | the pet is not an OpenRC service and keeps petting until init kills everything; if shutdown exceeds the 30 s watchdog window the SoC resets. That reset is harmless (the next boot is unaffected) | none |
| trace filesystem not writable | logger logs once and exits; `status` reports the trace missing; the watchdog is unaffected | fix the path or `KMS_TEST_TRACE` |
| dmesg ring wraps before a snapshot | the trace holds only the last snapshot's filtered window; pstore still keeps the panic console | re-run with a narrower filter via `KMS_TEST_DMESG_FILTER` |

The trace filter is
`decon|exynos|drm|watchdog|teardown|vsync|irq|Unable to handle|Internal error|Kernel BUG|Oops|Call trace|lockup|panic`
(case-insensitive).  Register dumps are intentionally not included: they are
the spam, while the oops header and call trace are what identify the wedge.

## 6. Verification performed (no flash, no reboot)

On the running J7, with all device paths redirected to mocks
(`KMS_TEST_WD`, `KMS_TEST_TRACE`, `KMS_TEST_LOG`, `KMS_TEST_RUNDIR`,
`KMS_TEST_MARKER`, `KMS_INITTAB`, `KMS_DEST`, `KMS_ARM`, `KMS_MODDIR`):

* `kms-test-arm start` → pet and logger live, lock pids recorded, watchdog
  file received pings at the configured interval; `start` again → same pids
  (idempotent, no second pet).
* injected a synthetic `decon` line into `/dev/kmsg` → next snapshot in the
  trace, filter working.
* `kms-test-arm stop` → exit 0 in 2.2 s, magic-close byte is the last byte of
  the mock device, both locks removed, **no surviving processes**.
* `kms-test-arm boot` with the marker absent → exit 0, no processes, no files;
  with the marker present → arms inside the hook's own redirection (nothing on
  stdout), `status` exit 0.
* `kms-test-install install` against a copy of `/etc/inittab` → one comment +
  one hook line inserted between `openrc boot` and `openrc default`; second
  install is a no-op; `check` uses the copy; `uninstall` restores the file
  byte-for-byte (`cmp` clean); backup created.
* `kms-load.sh`: no args → 2; unknown rev → preflight failure, exit 1, no UI
  stop, no arming; full run with mocked `insmod`/`rc-service`/`pkill`/`pgrep`
  → stops UI, loads helper then driver, reports `card0` missing and the flat
  `decon_fb` counter, exit 1; helper-insmod failure → exit 1 and no driver
  load; missing watchdog without `--force` → abort before the UI, exit 1;
  with `--force` → proceeds with a warning, exit 1 from the node check.

Left untouched by design: `/etc/inittab` and `/etc/kms-test-mode` on the
device (the installer was exercised only against a copy), and the real
`/dev/watchdog` (arming it for a few seconds and then disarming it was avoided
because a failed disarm would reset the phone without Main's approval; the
existing `wdpet.sh`/`kmstest.sh` flow is the hardware evidence for the pet
semantics).  `/usr/local/sbin` was removed again after the installer test.

## 7. Relationship to the old harness

`/home/user/wdpet.sh`, `/home/user/logd.sh` and `/home/user/kmstest.sh` remain
valid for an already-running session and are what earlier reports used.
`kms-test-arm`/`kms-load.sh` supersede them for scripted test boots: arming is
marker-gated at boot, the logger writes atomically, every step is bounded, the
exit status is meaningful, and no `systemctl`/`rc-update` state is required.
