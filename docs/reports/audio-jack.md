# Automatic audio jack routing for j7elte

**Scope.** Make headset/headphone insertion switch the ALSA route without
manual `alsaucm` calls, on top of the UCM2 profile from
`parity/reports/audio-userspace.md` and the parity-verified kernel
(`parity/reports/audio.md`).

**No hardware was available.** Everything below is static analysis of the
kernel source/DT plus host-side execution of the daemon against synthetic
sysfs/`alsaucm`/`amixer` stand-ins (including an aarch64 build under
qemu-user).  No real codec, jack or PCM was exercised.

## Result

| Deliverable | Path |
|---|---|
| Daemon source | `parity/userspace/audio/jack/j7elte-audio-jack.c` |
| Config | `parity/userspace/audio/jack/j7elte-audio-jack.conf` |
| OpenRC service | `parity/userspace/audio/jack/j7elte-audio-jack.initd` |
| systemd unit | `parity/userspace/audio/jack/j7elte-audio-jack.service` |
| Package | `parity/pmaports/device/archived/j7elte-audio-jack/` (same four files + `APKBUILD`) |
| Package patch | `parity/patches/userspace/audio/0003-j7elte-audio-jack-package.patch` |
| Device dependency patch fragment | `parity/patches/userspace/audio/0004-device-samsung-j7elte-depend-on-audio-jack.patch` |

The daemon is single-threaded C11, libc only; runtime dependencies are
`alsa-utils` (`alsaucm`, `amixer`) and `j7elte-audio-ucm`.

## P1 — `sec_jack` interface (verified)

`drivers/misc/sec_jack.c` is the only jack driver in the tree
(`CONFIG_SAMSUNG_JACK=y`, `parity/pmaports/device/archived/linux-samsung-j7elte/config-samsung-j7elte.aarch64:1065`,
`CONFIG_SWITCH=y` at `:2924`, `CONFIG_INPUT_GPIO=y` at `:1542`).

### What it registers

| Interface | Evidence |
|---|---|
| switch device `h2w` | `sec_jack.c:78-81`; `switch_dev_register()` at `:657` |
| switch device `send_end` (headset button) | `sec_jack.c:84-87`; `switch_dev_register()` at `:663` |
| class `audio` / device `earjack` with attributes `state`, `key_state`, `mic_adc` | `sec_jack.c:735-767`, attrs at `:367`, `:382`, `:400` |

There is **no** `snd_jack`: neither `sec_jack.c` nor the machine/codec
drivers call `snd_jack_new()`/`snd_jack_report()` (grep over
`drivers/misc/sec_jack.c`, `sound/soc/samsung/universal7580-cod3022x.c`,
`sound/soc/codecs/cod3022x.c` returns nothing).  `CONFIG_SND_JACK=y` is
enabled but unused for this codec.  The machine driver only wires
callbacks: `sec_jack_register_button_notify_cb()` when
`samsung,use-externel-jd` is present (`universal7580-cod3022x.c:798-807`,
DT property at `exynos7580-j7elte_rev00.dts:1125`) and sets
`jack_controls.snd_card_registered = 1` at `:1277`; `sec_jack_probe()`
defers until then (`sec_jack.c:606-610`).

Consequence: ALSA/UCM/PipeWire cannot observe the plug.  The switch
`state` attribute and its uevent are the only type-carrying interface.

### State values (the jack type)

`include/linux/sec_jack.h:20-25`:

```
SEC_JACK_NO_DEVICE      = 0x0
SEC_HEADSET_4POLE       = 0x1   /* headphone + mic */
SEC_HEADSET_3POLE       = 0x2   /* headphone, no mic */
SEC_EXTERNAL_ANTENNA    = 0x4
```

`sec_jack_set_type()` stores the value and publishes it with
`switch_set_state(&switch_jack_detection, jack_type)` (`sec_jack.c:283-299`,
report at `:298`).  The type comes from the ADC zone table
(`determine_jack_type()`, `sec_jack.c:309-352`): each zone is
`adc_high, delay_us, check_count, jack_type`, and a zone is only accepted
after `check_count` samples inside it (`:333-346`).  j7elte's zones
(`exynos7580-j7elte_rev00.dts:1077-1080`):

```
<&det_zones 3    10000 10 0>,   /* <= 3   : no jack */
<&det_zones 950  10000 10 2>,   /* <= 950 : 3-pole  */
<&det_zones 2400 10000 10 1>,   /* <= 2400: 4-pole  */
<&det_zones 9999 10000 10 1>    /* above  : 4-pole  */
```

Presence itself is the detect GPIO (`detect-gpio = <&gpa3 1 0>`,
`key-gpio = <&gpa0 4 0>`, `det-dbtime = <50>` at
`exynos7580-j7elte_rev00.dts:1068-1088`), debounced in
`sec_jack_detect_work()` for `det_debounce_time_ms` in 10 ms steps
(`sec_jack.c:428-455`).

So the distinction the assignment asks for is exactly the switch state:
**4-pole = headset with microphone (state 1)**, **3-pole = headphone
without microphone (state 2)**.  There is no separate "mic present" bit;
the raw mic-bias ADC is only exposed for factory tests
(`earjack_mic_adc_show()`, `sec_jack.c:385-400`) and the `/sys/class/audio/earjack/state`
attribute is a boolean "is 4-pole" view (`:370-383`), not a full type.

### Event source (what the daemon subscribes to)

`switch_set_state()` (`drivers/switch/switch_class.c:76-113`) sends
`KOBJ_CHANGE` with `SWITCH_NAME=<name>` and `SWITCH_STATE=<decimal state>`
(`:90-107`) whenever the value changes; the `state` sysfs attribute prints
`"%d\n"` (`:29-41`).  This is published on
`NETLINK_KOBJECT_UEVENT`, group 1 — usable without udev, which is what the
daemon does (`uevent_open()`, `uevent_parse()`), filtering
`SUBSYSTEM=switch`, `ACTION=change`, `SWITCH_NAME=h2w`.

## P2 — Daemon design

`j7elte-audio-jack.c` (1274 lines, no external libraries):

1. **Input** — netlink uevent socket (primary, zero latency) plus a
   periodic re-read of `/sys/class/switch/h2w/state` (default every 5 s;
   covers lost uevents, a switch device that appears after the daemon, a
   manual mixer change, and kernels where the uevent is filtered).  If the
   netlink socket cannot be bound (no CAP_NET_ADMIN), polling drops to
   500 ms automatically.
2. **Debounce** — a candidate state is only committed when it is still
   read back after `settle_ms` (default 300 ms).  This sits on top of the
   kernel's own debounce (50 ms detect + 10 x 10 ms zone sampling), so a
   slow insert or dirty contact cannot produce a half-inserted state.
   States are coalesced, not queued: only the last one is applied.
3. **Mapping** (overridable in the config):

   | switch state | playback device | capture device |
   |---|---|---|
   | 0 (no jack) | `Speaker` | `Mic` |
   | 1 (4-pole) | `Headphones` | `Headset Mic` |
   | 2 (3-pole) | `Headphones` | `Mic` |
   | 4 (external antenna) | `Speaker` | `Mic` |
   | anything else | `Speaker` | `Mic` (safe default) |

4. **Application** — one `alsaucm -c hw:0 -b -` process with a batch on
   stdin:

   ```
   set _verb "HiFi"
   set _enadev "<playback device>"
   set _enadev "<capture device>"
   ```

   Values are quoted because alsaucm's batch parser splits on whitespace
   unless a token starts with a quote (`alsa-utils/alsaucm/usecase.c`,
   `parse_line()`); without quotes `Headset Mic` would be split into
   `Headset` + `Mic`.  Names containing `"` or a newline are rejected by
   the daemon instead of being silently mangled (still configurable
   through `out_<state>`/`in_<state>`).

   `_disdev` is not needed: every `EnableSequence` in
   `j7elte/HiFi.conf` writes the full set of switches it owns (playback
   gates `HP HP On`/`EP EP On`/`SPK SPK On`; capture ADC mixers), so
   enabling the new devices after a fresh `_verb` is idempotent, also
   re-asserts the route after resume or manual tampering, and does not
   leave the previous device enabled.  Playback and capture sequences
   touch disjoint controls (verified in the UCM file: the playback devices
   only write S2801/HP/EP/SPK, the capture devices only the ADC mixer and
   MIC1/MIC2 gates), so both can be enabled in one session.
5. **Verification + fallback** — in batch mode a failing command aborts
   `alsaucm` with `EXIT_FAILURE` (`usecase.c`, the batch loop
   `my_exit(context, EXIT_FAILURE)`), so the exit status is used; the
   exception is the `-n`/no-open path, which prints
   `command 'set' requires an open card` and still exits 0 (observed),
   so the status alone is not relied upon.  The daemon additionally reads
   back the DAPM gates with
   `amixer -c hw:0 cget "name=..."` and requires
   `HP HP On=on` / `SPK SPK On=off` / `EP EP On=off` and the matching
   MIC1/MIC2 gate.  If the readback mismatches (or `alsaucm` fails, is
   missing, or times out after `cmd_timeout_ms`), it replays the ROM
   values with plain `amixer sset`: the playback verb block, all three
   output gates, `DAC Gain 121`, `S2801 MIX1_LVL 0`, the transducer volume
   (`Speaker Volume 3` / `Headphone Volume 36` / `Earphone Volume 0`) and
   the complete MIC1 or MIC2 capture route including `MIC1/MIC2 Volume 11`
   and `Boost Volume 2`.  Those are the exact values of the UCM sequences
   in `HiFi.conf`, so the fallback is full-fidelity, not a stub — but it
   is only used when the UCM path cannot be confirmed.
6. **Retry** — a failed application (card not probed yet at boot, codec
   busy) is retried every `retry_ms` (5 s) with rate-limited logging;
   the desired state is kept.  After 3 consecutive failures for the same
   state the interval backs off to 5 minutes, so a persistent problem
   (e.g. wrong readback names) cannot turn into a mixer-write loop; the
   counter resets on success or on the next jack change.
7. **Optional hook** — `hook = /path` is executed with
   `state out_device in_device` after every successful change, as the
   integration point for a session manager (see residual risks).
8. **Lifecycle** — `SIGTERM`/`SIGINT` exit cleanly, `SIGHUP` reloads the
   config and re-applies.  No daemonizing (service manager supervises),
   no temporary files, no shell involved (`execv` only), all commands
   bounded by a timeout and killed on expiry.

### Configuration (`/etc/j7elte-audio-jack.conf`)

Complete and commented: `card`, `verb`, `switch`, optional `state_file`
(test hook), `settle_ms`, `poll_ms`, `retry_ms`, `cmd_timeout_ms`,
`verify`, absolute `alsaucm`/`amixer` paths, `hook`, and `out_<state>`/
`in_<state>` mappings.  Unknown keys are logged and ignored; missing file
means built-in defaults, so the daemon is functional with no config.

### CLI

```
j7elte-audio-jack                        # daemon
j7elte-audio-jack --once                 # apply the current sysfs state, exit
j7elte-audio-jack --state 1 --dry-run    # print the UCM batch for state 1
j7elte-audio-jack --self-test            # internal parser/mapping checks
```

`--once` and `--state` are the on-device smoke tests (they need no
uevent, only the card); exit status is 0 only if the route verified.

## P3 — Packaging

`parity/pmaports/device/archived/j7elte-audio-jack/APKBUILD`:

```
pkgname=j7elte-audio-jack, pkgver=0.1, arch=aarch64, license=GPL-3.0-or-later
depends="alsa-utils j7elte-audio-ucm"
options="!check"            # check() would run an aarch64 binary on the build host
build (): $CC $CFLAGS -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -o ...
package: /usr/bin/j7elte-audio-jack, /etc/j7elte-audio-jack.conf,
         /etc/init.d/j7elte-audio-jack, /usr/lib/systemd/system/j7elte-audio-jack.service
```

* OpenRC: `/etc/init.d/j7elte-audio-jack` (`command_background`,
  pidfile, `after alsa sound`, `use logger`).
* systemd: `Type=simple`, `Restart=on-failure`, `After=sound.target`,
  `RestrictAddressFamilies=AF_NETLINK`, `ProtectSystem=strict`,
  `NoNewPrivileges`, `MemoryDenyWriteExecute`.
* The shared `device-samsung-j7elte/APKBUILD` was **not** edited.  The
  dependency addition is
  `parity/patches/userspace/audio/0004-device-samsung-j7elte-depend-on-audio-jack.patch`:

  ```diff
  @@ -13,6 +13,7 @@
   	linux-samsung-j7elte
   	firmware-samsung-j7elte
   	j7elte-audio-ucm
  +	j7elte-audio-jack
   	j7elte-bt
  ```

  It is baseline-relative to the current working-tree device `APKBUILD`
  (which already carries `j7elte-audio-ucm` from patch 0002) and was
  verified with `patch -p1 --dry-run`.  The consolidated
  `parity/patches/userspace/0001-device-deps.patch` (owned by the device
  packaging work) needs the same one-line addition when it is
  regenerated; it was deliberately **not** edited here to avoid
  conflicting with concurrent work on the shared device package.  If the
  device package is regenerated from `HEAD`, rebase the 0004 hunk.
* `0003-j7elte-audio-jack-package.patch` contains all four package files
  and applies cleanly to pmaports `HEAD` (`git apply --check`).

## P4 — Validation performed

1. **Compilation, warnings-clean.** Host GCC with
   `-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion`: 0 warnings.
   The APKBUILD `build()` command was executed verbatim with
   `CC=gcc CFLAGS="-O2 -pipe" LDFLAGS="-Wl,-O1"`.
2. **Cross-compilation.** aarch64 GCC 10.3
   (`toolchains/gcc-arm-10.3-.../aarch64-none-linux-gnu-gcc`) produces a
   dynamic and a static aarch64 ELF; the static binary's `--self-test`
   runs under `qemu-aarch64` (target kernel 3.7 ABI, same ISA as the
   device).
3. **Self-test (17 checks, PASS)** — `--self-test` asserts the state
   mapping for 0/1/2/4 and for an out-of-range value, the exact (quoted)
   UCM batch for a 4-pole plug, UCM-name validation, netlink uevent
   parsing (accepts `h2w` state 1, rejects `send_end`, rejects
   `ACTION=add`) and `amixer cget` value parsing for
   boolean/integer/missing outputs.
4. **Real `alsaucm` parsing of the batch.** A UCM2 virtual card
   (`ALSA_CONFIG_UCM2` pointing at a copy of the system tree with
   `conf.virt.d/j7test.conf` and a device set named `Speaker`,
   `Headphones`, `Mic`, `Headset Mic`) was used to exercise the actual
   batch parser without hardware.  The daemon's exact batch was accepted
   with rc=0 for states 0, 1, 2 and 4, while the control case
   `set _enadev Headset Mic` (unquoted) fails with
   `failed to set _enadev=Headset: No such file or directory` and rc=1 —
   confirming both the quoting requirement and that the daemon's batch is
   well formed.  This validates parsing/device selection only; the
   `cset` sequences were not executed (they need the real controls).
5. **End-to-end control flow without hardware.** With fake
   `alsaucm`/`amixer` executables (a fake mixer state in files) and a
   temporary `state_file`:
   * `--state 1` produced exactly
     `set _verb "HiFi" / set _enadev "Headphones" / set _enadev "Headset Mic"`;
   * a fake `alsaucm` that changed nothing was caught by the readback
     check, the `amixer` fallback then produced `HP HP On=on`,
     `SPK SPK On=off`, `MIC2 MIC2 On=on`, `MIC1 MIC1 On=off` and the run
     exited 0;
   * the fallback path was also reached for a hard `alsaucm` failure
     (fake helper exits 1: log `alsaucm failed (rc=1)`) and by the timeout
     kill path (fake helper sleeps; `cmd_timeout_ms=400`, log
     `timed out after 400 ms and was killed`, `alsaucm failed (rc=-2)`),
     both ending with the expected gates `HP HP On=on`,
     `SPK SPK On=off`, `MIC1 MIC1 On=on` for a 3-pole plug;
   * the daemon loop, started with `poll_ms=100`, followed the sequence
     `0 -> 2 -> 1 -> 0 -> 4`, applied each state once (no duplicates for
     repeated reads), logged the transition, ran the hook with
     `state out in`, and exited 0 on `SIGTERM`;
   * the hook log shows the expected mapping for every state, including
     state 4 treated as "no jack".
6. **Service/package syntax.**
   * `sh -n APKBUILD`, `sh -n j7elte-audio-jack.initd` → OK.
   * `systemd-analyze verify j7elte-audio-jack.service` → only
     "Command /usr/bin/j7elte-audio-jack is not executable" (absent on
     the build host), no unit syntax errors.
   * `package()` was executed with a fake `$pkgdir`; the installed tree is
     exactly `/usr/bin/j7elte-audio-jack` (0755),
     `/etc/j7elte-audio-jack.conf` (0644),
     `/etc/init.d/j7elte-audio-jack` (0755),
     `/usr/lib/systemd/system/j7elte-audio-jack.service` (0644).
   * `sha512sum -c` against the `sha512sums` in `APKBUILD` → all OK.
7. **UCM name cross-check.** The daemon's device names all exist in the
   shipped profile: `HiFi.conf` defines `SectionDevice."Earpiece"`,
   `."Speaker"`, `."Headphones"`, `."Mic"`, `."Headset Mic"`, and the
   master profile defines `SectionUseCase."HiFi"`
   (`ucm2/conf.d/Universal7580-I2S/Universal7580-I2S.conf`).  `alsaucm`
   accepts the verb/device identifiers used in the batch (same syntax as
   the manual commands in `audio-userspace.md` P7).
8. **Kernel config prerequisites** checked in
   `config-samsung-j7elte.aarch64`: `CONFIG_SAMSUNG_JACK=y` (1065),
   `CONFIG_SWITCH=y` (2924), `CONFIG_INPUT_GPIO=y` (1542),
   `CONFIG_SND_JACK=y` (2426, unused).

## P5 — Test procedure on hardware

```sh
# 1. jack event visible at all?
evtest /dev/input/eventX          # button events (KEY_MEDIA etc.)
cat /sys/class/switch/h2w/state   # 0 / 1 / 2 / 4
udevadm monitor -k -p | grep SWITCH   # SWITCH_NAME=h2w SWITCH_STATE=1

# 2. route application without the daemon (sanity check of the UCM path)
j7elte-audio-jack --state 1 --dry-run
j7elte-audio-jack --state 1       # exit 0 == readback verified
amixer -c hw:0 cget "name=HP HP On"

# 3. daemon
/usr/bin/j7elte-audio-jack --config /etc/j7elte-audio-jack.conf &
# plug a 3-pole plug:  Speaker -> Headphones (capture stays Mic)
# plug a 4-pole plug:  Headphones + Headset Mic
# pull out:            Speaker + Mic
# (the daemon logs "jack state N: out=... in=...")

# 4. services
rc-service j7elte-audio-jack start     # OpenRC
systemctl enable --now j7elte-audio-jack
```

Playback/capture checks: `speaker-test -D hw:0,0`, `arecord -D hw:0,0 ...`
after each transition.  If `alsaucm` fails on device, the daemon's log
shows the `alsaucm` output and whether the fallback verified.

## Residual risks / blockers

* **No hardware test.**  The daemon, its debounce, fallback and packaging
  were only exercised against synthetic interfaces.  Two things can only
  be confirmed on the device: (a) that the `h2w` uevent reaches a group-1
  netlink listener (kernel/Android `switch` class, unmodified here), and
  (b) that the DAPM readback names (`HP HP On`, `SPK SPK On`,
  `EP EP On`, `MIC1 MIC1 On`, `MIC2 MIC2 On`) report `on/off` through
  `amixer cget` as expected.  If the readback names differ, only
  `verify_route()`/`fallback_apply()` need editing; worst case `verify=no`
  makes the daemon trust `alsaucm` and skip the fallback.
* **PipeWire/WirePlumber ownership.**  The UCM profile exposes
  `PlaybackPriority`/`CapturePriority`, so a PipeWire session may select
  `Headphones` itself once it notices the device — but it has no reason
  to (no jack kcontrol), and it will not follow this daemon.  When a
  PipeWire/WirePlumber session is running, external `alsaucm` device
  selection can be overridden by its own profile/route logic on the next
  stream start, and session-manager state can override the mixer.  The
  `hook` exists to drive such a session manager (e.g. a script calling
  `wpctl`/`pactl`); the robust long-term fix is a kernel patch that
  registers `snd_jack_new()`/`snd_jack_report()` in `sec_jack` and keys
  off `SND_JACK_HEADPHONE|SND_JACK_MICROPHONE`.  That is a kernel change,
  outside this userspace gap, and is not included.
* **External antenna (state 4)** is mapped to the unplugged route; the
  ROM uses it for FM/T-DMB, and this profile has no FM device.  This is
  deliberate and safe, not a missing feature of the jack code.
* **Headset buttons** (`send_end` switch, `KEY_MEDIA`/`KEY_VOLUMEUP` etc.
  from the gpio_event device, `sec_jack.c:105-115`, `:490-506`) are not
  handled; they are media-key input and belong to a key handler, not to
  routing.
* **Brief silence on plug.**  `set _verb HiFi` re-runs the verb
  `EnableSequence`, which turns all outputs off before the new device is
  enabled.  That is a handful of register writes (no PCM reconfiguration)
  and was not audible-tested; a click is possible with a stream running.
* **Verification depends on control readback semantics.**  The daemon
  treats a `cget` mismatch as "UCM did not apply" and falls back; if any
  of the five readback control names behaved differently on the real
  codec, the daemon would end up using the amixer fallback (still
  correct) or, if the fallback also fails verification, retry with a
  5-minute backoff.  `verify = no` disables the readback and trusts
  `alsaucm`; this should be tried first if the device log shows endless
  "did not verify" messages.
* **Batch mode aborts on the first failing command** (upstream
  behaviour), so a missing control in the middle of a device sequence
  leaves the mixer partially configured; the subsequent fallback rewrites
  the complete route, and the 5-minute backoff bounds the retry churn.
