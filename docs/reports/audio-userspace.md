# Audio userspace (UCM/mixer) for j7elte

**Scope:** build the missing ALSA userspace configuration for the
`Universal7580-I2S` sound card (COD3022X codec + S2801X aux codec) from the
stock ROM mixer graph, so that the already parity-verified kernel driver
(`parity/reports/audio.md`) can be driven by postmarketOS userspace.

**No hardware was available.** Everything below is static validation:
cross-compilation-independent file/source checks, ALSA configuration parsing
with the host `alsaucm` (alsa-lib 1.2.16.1), and an automated control-name /
range cross-check against the Nura kernel sources.  No playback, capture or
jack-detect test was performed.

## Result

| Deliverable | Path |
|---|---|
| ROM mixer paths (extracted) | `parity/userspace/audio/rom/mixer_paths.xml` (96,675 B, 3,381 lines, 908 `<path>`, 1,147 `<ctl>`) |
| ROM jack HAL config (extracted) | `parity/userspace/audio/rom/jack_alsa_mixer.json` (330 B) |
| ROM audio policy (extracted, reference) | `parity/userspace/audio/rom/audio_policy.conf` |
| UCM2 master | `parity/userspace/audio/ucm2/conf.d/Universal7580-I2S/Universal7580-I2S.conf` |
| UCM2 HiFi | `parity/userspace/audio/ucm2/j7elte/HiFi.conf` |
| UCM2 VoiceCall | `parity/userspace/audio/ucm2/j7elte/VoiceCall.conf` |
| Control checker | `parity/userspace/audio/validate_controls.py` |
| Package | `parity/pmaports/device/archived/j7elte-audio-ucm/APKBUILD` + sources |
| Device dependency patch | `parity/patches/userspace/audio/0001-device-samsung-j7elte-depend-on-audio-ucm.patch` |

`validate_controls.py` result: **PASS** — 54/54 distinct ROM control names
exist in the kernel, 53/53 UCM control names exist, 0 out-of-range values,
0 invalid enum texts.

## P1 — Extraction from `system.raw`

`system.raw` is the system *partition* image, so the ROM files live at `/etc`,
not `/system/etc`:

```
$ debugfs -R 'ls -l /etc' system.raw | grep -Ei 'mixer|jack|audio'
 731  100644  330       jack_alsa_mixer.json
 742  100644  96675     mixer_paths.xml
 699  100644  5532      audio_effects.conf
 700  100644  4753      audio_policy.conf
$ debugfs -R 'dump /etc/mixer_paths.xml parity/userspace/audio/rom/mixer_paths.xml' system.raw
$ debugfs -R 'dump /etc/jack_alsa_mixer.json parity/userspace/audio/rom/jack_alsa_mixer.json' system.raw
$ debugfs -R 'dump /etc/audio_policy.conf parity/userspace/audio/rom/audio_policy.conf' system.raw
```

`mixer_paths.xml` parses as well-formed XML (`xml.etree.ElementTree`;
the checker parses it).  `jack_alsa_mixer.json` content:

```json
[ { "device": "configure",
    "params": [ {"name":"loopback","value":"2"}, {"name":"device","value":"alsa"},
                {"name":"samplerate","value":"48000"}, {"name":"period","value":"960"},
                {"name":"nperiods","value":"2"} ] } ]
```

## P2 — UCM generation and control cross-check

### UCM flavour: UCM2 (not UCM1)

Alpine's `alsa-ucm-conf` (1.2.16.1) installs **only** `ucm2`:

```apk
package() {
	mkdir -p "$pkgdir"/usr/share/alsa
	cp -r ucm2 "$pkgdir"/usr/share/alsa
	...
}
```
(aports `main/alsa-ucm-conf/APKBUILD`, checked 2026-10-24).  There is no
`/usr/share/alsa/ucm` tree, so a UCM1 profile would be dead weight.  The
profile is therefore UCM2 under `/usr/share/alsa/ucm2/`.

### Card lookup path

* `card->name = "Universal7580-I2S"` —
  `sound/soc/samsung/universal7580-cod3022x.c:1129`.
* platform driver name `Universal7580-audio` — same file, `:1311`.
* `snd_card->shortname`/`longname`/`driver` are set from `card->name` because
  the machine driver sets no `driver_name`/`long_name` —
  `sound/soc/soc-core.c:1793-1799`; `-`/`_` are preserved in the driver field
  (`:1800-1810`).

UCM2 probes `conf.d/${CardDriver}/${CardLongName}.conf`
(`/usr/share/alsa/ucm2/ucm.conf`, `False.If...UseCasePath.confd1`), i.e.
`conf.d/Universal7580-I2S/Universal7580-I2S.conf`.  The master file references
`File "/j7elte/HiFi.conf"` (absolute, resolved below `ucm2/`), the same
pattern upstream uses (`/Qualcomm/apq8096/HiFi.conf`).

### Control mapping

All 54 distinct controls used by the ROM (`<ctl name="...">`, 1,147 uses)
were resolved.  `S2801 <x>` names are the S2801X codec's control names with
the `name_prefix = "S2801"` from `snd_soc_codec_conf`
(`universal7580-cod3022x.c:1122-1126`), applied by `snd_soc_cnew()` as
`"<prefix> <name>"` (`sound/soc/soc-core.c:2372`, `snd_soc_cnew()`), and
`dev_name` is filled from the aux codec in `soc-core.c:1642-1647`.

**No missing control.**  `Line-in Volume` is the only ROM control not used by
the UCM files, because it is only used by the FM paths and this profile does
not expose an FM device (the FM DAI link is present, but the ROM FM routing is
unrelated to phone audio; can be added later without new knowledge).

| ROM control | Kernel source / macro | Type | Range / enum texts |
|---|---|---|---|
| `ADC Left Gain` | `cod3022x_snd_controls[]` (value macro) | integer | 0..151 |
| `ADC Right Gain` | `cod3022x_snd_controls[]` (value macro) | integer | 0..151 |
| `ADCL Mixer LINELL Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCL Mixer LINERL Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCL Mixer MIC1L Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCL Mixer MIC2L Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCR Mixer LINELR Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCR Mixer LINERR Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCR Mixer MIC1R Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `ADCR Mixer MIC2R Switch` | `cod3022x_snd_controls[]` (value macro) | integer | 0..1 |
| `Chargepump Mode` | `cod3022x_snd_controls[]` (SOC_ENUM) | enum | VDD, HALF-VDD, CLASS-G-D, CLASS-G-A |
| `DAC Gain` | `cod3022x_snd_controls[]` (value macro) | integer | 0..151 |
| `DNC Max Gain` | `cod3022x_snd_controls[]` (value macro) | integer | 0..30 |
| `EP EP On` | `cod3022x_dapm_widgets[]` switch (SOC_DAPM_SINGLE) | bool | on/off |
| `EP Mixer ADCL Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `EP Mixer ADCR Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `Earphone Volume` | `cod3022x_snd_controls[]` | integer | 0..15 |
| `HP HP On` | `cod3022x_dapm_widgets[]` switch | bool | on/off |
| `HPL Mixer ADCL Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `HPL Mixer ADCR Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `HPR Mixer ADCL Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `HPR Mixer ADCR Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `Headphone Volume` | `cod3022x_snd_controls[]` | integer | 0..63 |
| `LINEIN LINEIN On` | `cod3022x_dapm_widgets[]` switch | bool | on/off |
| `Line-in Volume` (unused) | `cod3022x_snd_controls[]` | integer | 0..7 |
| `MIC1 Boost Volume` | `cod3022x_snd_controls[]` | integer | 0..3 |
| `MIC1 MIC1 On` | `cod3022x_dapm_widgets[]` switch | bool | on/off |
| `MIC1 Volume` | `cod3022x_snd_controls[]` | integer | 0..31 |
| `MIC2 Boost Volume` | `cod3022x_snd_controls[]` | integer | 0..3 |
| `MIC2 MIC2 On` | `cod3022x_dapm_widgets[]` switch | bool | on/off |
| `MIC2 Volume` | `cod3022x_snd_controls[]` | integer | 0..31 |
| `MonoMix Mode` | `cod3022x_snd_controls[]` (SOC_ENUM) | enum | Disable, R, L, LR-Invert, (L+R)/2, L+R |
| `S2801 CH1 DOUT Select` | `s2801x_snd_controls[]` (SOC_ENUM_EXT) | enum | DMIX_OUT, AIF4IN, RMIX_OUT |
| `S2801 CH1 Mixer En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 CH2 DOUT Select` | `s2801x_snd_controls[]` | enum | DMIX_OUT, AIF4IN, AIF3IN |
| `S2801 CH2 Mixer En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 CH3 DOUT Select` | `s2801x_snd_controls[]` | enum | DMIX_OUT, AIF4IN, AIF2IN |
| `S2801 CH3 Mixer En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 CH3 Rec En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 CH4 Mixer En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 MIX1_LVL` | `s2801x_snd_controls[]` (SOC_SINGLE_TLV) | integer | 0..7 |
| `S2801 MIX2_LVL` | `s2801x_snd_controls[]` | integer | 0..7 |
| `S2801 Mixer En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 RMIX1 En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 RMIX1_LVL` | `s2801x_snd_controls[]` | integer | 0..7 |
| `S2801 RMIX2 En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 RMIX2_LVL` | `s2801x_snd_controls[]` | integer | 0..7 |
| `S2801 SRC1 En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 SRC2 En` | `s2801x_snd_controls[]` | enum | Off, On |
| `S2801 SRC3 En` | `s2801x_snd_controls[]` | enum | Off, On |
| `SPK Mixer ADCL Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `SPK Mixer ADCR Switch` | `cod3022x_dapm_widgets[]` mixer | bool | on/off |
| `SPK SPK On` | `cod3022x_dapm_widgets[]` switch | bool | on/off |
| `Speaker Volume` | `cod3022x_snd_controls[]` | integer | 0..15 |

DAPM switch names are expanded by ASoC as `"<widget> <control>"`
(`sound/soc/soc-dapm.c:655-660`, `snd_soc_cnew(..., prefix)`); e.g.
widget `SPK` + control `SPK On`
→ `SPK SPK On`.

### PCM device mapping

`soc_new_pcm(rtd, num)` is called with the DAI-link index
(`sound/soc/soc-core.c:1432`, `sound/soc/soc-pcm.c` `snd_pcm_new(..., num, ...)`),
so devices are assigned in link order:

| PCM | DAI link | Direction | Used by UCM |
|---|---|---|---|
| `hw:Card,0` | `universal7580-cod3022x` (`i2s0-pri`, `cod3022x-aif`) | playback + capture | HiFi |
| `hw:Card,1` | `universal7580-cod3022x-sec` (`i2s0-sec`) | playback | — |
| `hw:Card,2` | `cp` (`voice call`, `cod3022x-aif2`) | playback + capture | VoiceCall |
| `hw:Card,3` | `bt` (`dumy-aif2`) | playback + capture | — |
| `hw:Card,4` | `fm` (`FM audio`) | playback | — |
| `hw:Card,5` | `playback-eax0` | playback | — |
| `hw:Card,6` | `playback-eax1` | playback | — |

The card id is `Universal7580I2S` (ALSA keeps only alphanumerics when deriving
the id from the shortname, `sound/core/init.c` `copy_valid_id_string()`), while
the UCM directory uses the *driver* string with the dash
(`Universal7580-I2S`).  `alsaucm -c hw:0` is the safe invocation.

## P3 — UCM contents

Profile has two use cases; each device sequence is a verbatim translation of
the ROM `media-*`/`incall_nb-*` path (including recursive `<path>` includes):

`HiFi` (PCM 0):
* **Earpiece** — `route-ap-tx` + `dev-handset` + `gain-media-handset`
* **Speaker** — `route-ap-tx` + `dev-speaker` + `gain-media-speaker`
* **Headphones** — `route-ap-tx` + `dev-headset` + `gain-media-headset`
* **Mic** — `route-ap-rx` + `dev-main-mic` + `gain-media-mic`
* **Headset Mic** — `route-ap-rx` + `dev-headset-mic` + `gain-media-headset-mic`

`VoiceCall` (PCM 2):
* **Earpiece / Speaker / Headphones** — `route-cp` + `dev-*` +
  `gain-incall_nb-*`
* **Mic** — `route-cp` + `dev-dual-mic` (identical to `dev-main-mic` in the
  ROM) + `gain-incall_nb-handset-mic`
* **Headset Mic** — `route-cp` + `dev-headset-mic` +
  `gain-incall_nb-headset-mic`

The ROM initial `<ctl>` block (S2801 reset, HP/EP/SPK off, chargepump, DNC) is
executed as `SectionVerb.EnableSequence` and mirrored in `DisableSequence`;
each device turns the other outputs off.  `dev-2nd-mic` and `dev-dual-mic` are
byte-identical to `dev-main-mic` in the ROM, so there is only one physical
capture path; no fake "second mic" device was invented.

VoiceCall is included because the controls exist, but it is **not usable
end-to-end on this kernel**: the `cp` link uses platform `snd-soc-dummy`
(`universal7580-cod3022x.c:1064-1074`), i.e. there is no modem audio backend.
The mixer graph is complete; audible call audio still needs a modem
driver/userspace stack that j7elte does not have in pmOS (see
`parity/reports/modem.md`).  This is stated in the VoiceCall.conf header.

## P4 — Jack

`jack_alsa_mixer.json` is an Android AudioFlinger HAL configuration (loopback
PCM device parameters); it is shipped verbatim as
`/usr/share/j7elte-audio/jack_alsa_mixer.json` for reference.

The Linux jack path is different and creates **no ALSA mixer controls**:
`drivers/misc/sec_jack.c` registers a `gpio_event` input device and two
`switch` devices (`:657`, `:663`), reports jack type/key through input events,
and calls back into the machine driver (`set_micbias`, `get_adc`).  It only
defers probe until the sound card exists (`:606`, flag set at
`universal7580-cod3022x.c:1277`).  Consequently:

* there is no `snd_jack`/`kctl` jack control, so ALSA UCM/PipeWire cannot
  auto-switch Speaker↔Headphones on plug; switching is done by selecting the
  UCM device (`alsaucm ... set _enadev Headphones`);
* no udev rule or `alsactl` state can substitute for that; a userspace
  input-event listener would be required.  Not provided here because it cannot
  be validated without the device (the input device name and event codes must
  be observed on hardware).

Jack detection itself is expected to work through `/dev/input` (sec_jack,
`CONFIG_SAMSUNG_JACK=y`, DT zones verified in `parity/reports/audio.md`); only
the *automatic ALSA routing* is missing.

## P5 — Package

`parity/pmaports/device/archived/j7elte-audio-ucm/`:

```
APKBUILD
Universal7580-I2S.conf
HiFi.conf
VoiceCall.conf
jack_alsa_mixer.json
```

`APKBUILD` installs:

```
/usr/share/alsa/ucm2/conf.d/Universal7580-I2S/Universal7580-I2S.conf
/usr/share/alsa/ucm2/j7elte/HiFi.conf
/usr/share/alsa/ucm2/j7elte/VoiceCall.conf
/usr/share/j7elte-audio/jack_alsa_mixer.json
```

`depends="alsa-ucm-conf"`; no udev rule, init script or `alsactl` state is
needed (UCM sequences are applied on device/verb selection, not at boot).
The package is plain data, no compilation.

The device package is intentionally not edited (shared-file rule); the adding
of `j7elte-audio-ucm` to `device-samsung-j7elte`'s `depends` is supplied as
`parity/patches/userspace/audio/0001-device-samsung-j7elte-depend-on-audio-ucm.patch`
(baseline-relative to pmaports `HEAD`; verified with `patch -p1 --dry-run`
against `HEAD`.  Other pending device-package changes in the working tree may
need a trivial rebase of the hunk.)

## P6 — Validation performed

1. **XML**: `mixer_paths.xml` parsed with ElementTree (908 paths, 1,147 ctl
   uses, 54 distinct names).
2. **Control names/types/ranges**: `python3 parity/userspace/audio/validate_controls.py`
   → `RESULT: PASS`:
   * kernel controls enumerated from the codec/machine sources: 109;
   * ROM names missing from the kernel: **0**;
   * UCM `cset` control names missing from the kernel: **0** (384 `cset`
     lines across 3 files, 53 distinct names);
   * enum texts and integer ranges: 0 violations
     (`DAC Gain` 151-max, `ADC * Gain` 151-max, `Headphone Volume` 63-max,
     `Speaker/Earphone Volume` 15-max, `DNC Max Gain` 30-max, S2801 mixer
     levels 7-max, boost 3-max, mic volumes 31-max — all ROM values fit).
3. **ALSA configuration parsing**: the UCM tree was loaded by the host
   `alsaucm` 1.2.16 with `ALSA_CONFIG_UCM2` pointed at a copy, using the
   virtual-card path (`conf.virt.d/<name>.conf`, since no matching hardware
   exists on the build host).  `alsaucm ... list _verbs` and
   `alsaucm ... dump text` parsed both use cases and all ten device
   definitions, including every `Value` block.  `${CardId}` was replaced by a
   dummy string only for this parse test because alsa-lib cannot substitute a
   card id without a card; the shipped files use `hw:${CardId},0/2`.
4. **Sequence execution was not tested** (requires the real card): `cset`
   strings are evaluated by alsa-lib at device-enable time.  Their names,
   values and ranges are covered by (2).

## P7 — How to test on hardware

```sh
# 0. card present?
cat /proc/asound/cards            # expect "Universal7580-I2S" / id Universal7580I2S
aplay -l                          # expect 7 links, device 0..6
amixer -c hw:0 scontrols | head   # "HP HP On", "S2801 Mixer En", ...

# 1. UCM
alsaucm -c hw:0 list _verbs       # HiFi, VoiceCall
alsaucm -c hw:0 set _verb HiFi
alsaucm -c hw:0 list _devices     # Earpiece Speaker Headphones Mic "Headset Mic"

# 2. playback
alsaucm -c hw:0 set _enadev Speaker
speaker-test -D hw:0,0 -t sine -f 440 -c 2 -l 1
alsaucm -c hw:0 set _disdev Speaker
alsaucm -c hw:0 set _enadev Headphones
speaker-test -D hw:0,0 -t sine -f 440 -c 2 -l 1   # headset plugged
alsaucm -c hw:0 set _disdev Headphones
alsaucm -c hw:0 set _enadev Earpiece
speaker-test -D hw:0,0 -t sine -f 440 -c 2 -l 1

# 3. capture
alsaucm -c hw:0 set _enadev Mic
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/mic.wav && aplay /tmp/mic.wav
alsaucm -c hw:0 set _disdev Mic
alsaucm -c hw:0 set _enadev "Headset Mic"
arecord -D hw:0,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/hsmic.wav && aplay /tmp/hsmic.wav

# 4. jack (input/switch only, no ALSA control)
evtest /dev/input/eventX          # X = sec_jack device
cat /sys/class/switch/h2w/state   # 1 = headset inserted (see sec_jack switch)
cat /sys/class/switch/send/state

# 5. fallback: raw mixer control check
amixer -c hw:0 cget "name=HP HP On"
amixer -c hw:0 cget "name=S2801 CH1 DOUT Select"
```

If the machine driver ever exposes a different `card->driver` string, place
the same master file under `conf.d/<driver>/<driver>.conf`; the lookup order is
documented in `ucm2/ucm.conf`.

## Residual risks / blockers

* **No hardware test.**  Playback/capture levels, the S2801 AIF4 loop and the
  chargepump/DNC settings are ROM values copied 1:1; they still need an
  on-device listen test.  Static checks cannot validate analog behaviour.
* **VoiceCall is graph-complete but non-functional end-to-end**: the `cp` DAI
  link has no modem backend (`platform = snd-soc-dummy`).  Missing piece is a
  Linux modem audio driver / userspace for the Samsung CP, outside this gap.
* **No ALSA jack kcontrol** in `sec_jack`/machine driver, so automatic
  headset↔speaker switching by PipeWire/UCM is not possible; the profile
  provides manual device selection plus the ROM HAL jack file for reference.
* The `S2801` control prefix depends on the ASoC aux-codec `name_prefix`
  mechanism; this tree applies it (`soc-core.c:1642-1647`), and all names were
  verified against source, but a regression would make the S2801 `cset` lines
  fail at runtime (`alsaucm` reports "control not found").
