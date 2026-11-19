# FM audio route: BCM43438 -> cod3022x -> S2801 ABOX -> AP

## Path diagram

```
BCM43438 FM tuner --(analog stereo L/R)--> cod3022x LINEIN pins
cod3022x ADC (ADCL/ADCR mixers select LINELL/LINELR, LINEIN On)
  --(AIF4 PCM, dai-link 4, "FM audio cod3022x-aif-4")--> S2801 SRC3
S2801 ABOX mixer (Mixer En + SRC3 En)
  +- TX (playback): CH1/CH3 mixers -> DMA -> AP opens hw:0,4 (pcm_fm_out)
  |   -> cod3022x DAC -> SPK (speaker) or HPL/HPR (headset)
  +- RX (record): CH3 Rec + RMIX chain, CH1 DOUT=RMIX_OUT -> hw:0,0 capture
```

Stock names vs reality: there is no `hw:0,0` FM capture device in the live
`arecord -l` list (capture devices: 0 i2s0-pri, 2 voice, 3 BT). The HAL's
`fm_radio-fm-recording` route selects `route-ap-fm-rx`, i.e. the S2801 RX/CH3
rec chain, and the PCM comes back through the primary capture path (hw:0,0).
Playback is `hw:0,4` (`aplay -l`: `card 0: device 4: FM audio cod3022x-aif-4`),
matching `<pcmdai fmradio_link="4" />`. `arecord -l` shows no device 4 because
the FM dai-link is playback-only from the AP side (AP reads FM via the ABOX
loopback into the primary capture stream, not via dai-link 4 capture).

## Stock evidence (system.raw)

`etc/mixer_paths.xml` (96675 bytes):

- `<pcmdai fmradio_link="4" />` (line 8) -> dai-link 4 = FM.
- `<path name="route-ap-fm">`: `S2801 Mixer En=On`, `S2801 SRC3 En=On`.
- `<path name="route-ap-fm-tx">` = route-ap-fm + `CH1 Mixer En=On`,
  `CH2=Off`, `CH3=On`, `CH4=Off`, `Chargepump Mode=CLASS-G-A`, `DNC Max Gain=24`.
- `<path name="route-ap-fm-rx">` = route-ap-fm + `RMIX1 En=Off`, `RMIX2 En=On`,
  `CH3 Rec En=On`, `CH1 DOUT Select=RMIX_OUT`.
- `<path name="dev-fm-input">` (line 239): ADCL <- LINELL only, ADCR <- LINELR
  only, all MIC1/MIC2 off, `LINEIN LINEIN On=1`. This is the FM analog input.
- `<path name="dev-fm-speaker">` (line 176): dev-speaker + `SPK Mixer ADCL/ADCR=1`.
- `<path name="dev-fm-headset">` (line 182): dev-headset + `HPL Mixer ADCL=1`,
  `HPR Mixer ADCR=1`.
- `<path name="fm_radio-speaker">`: route-ap-fm-tx + dev-speaker.
- `<path name="fm_radio-speaker-headset">`: route-ap-fm-tx + dev-speaker + dev-headset.
- `<path name="fm_radio-headset">`: route-ap-fm-tx + dev-headset.
- `<path name="fm_radio-fm-recording">`: route-ap-fm-rx + `CH3 Rec En=On`.
- Gains: speaker `Line-in=4, Speaker=6, DAC=121`; headset `Line-in=6`;
  recording `Line-in=4, ADC L/R=127`.
- Nothing named `pcm_fm_out` in mixer_paths; the name lives in the HAL.

`etc/audio_policy.conf`: `AUDIO_DEVICE_IN_FM` listed under primary inputs.

`lib/hw/audio.primary.universal7580.so` (92384 bytes, ARM32, stripped): FM
strings are `fm_start`, `fm_stop`, `fm_mode`, `fm_radio_mute`,
`fm_radio_volume`, `pcm_fm_out`, `cannot open pcm_fm_out stream: %s`,
`fm_radio-fm-recording`, `fm_radio`, `Is FM Running`, `FM mute`, plus
`audio_route_apply_path`/`audio_route_init` (tinyALSA audio_route). So the HAL
opens a `pcm_fm_out` stream and applies the `fm_radio*` routes by name. Exact
card/device numbers and `fm_start` internals need ARM disassembly (sibling slice).

## Live evidence (j7elte, 2026-11-18)

- `aplay -l`: card 0 devices 0 (i2s0-pri), 1 (i2s0-sec), 2 (voice), 3 (BT),
  4 (**FM audio cod3022x-aif-4**), 5/6 (eax). -> playback = **hw:0,4**.
- `arecord -l`: card 0 devices 0, 2, 3 (no FM entry; FM returns via RX/CH3-rec
  chain into the primary capture stream, opened as hw:0,0 by apps).
- `amixer -c0 scontrols`: 101 controls. ALL stock S2801/codec controls used by
  the FM paths exist verbatim: `S2801 Mixer En`, `S2801 SRC3 En`,
  `S2801 CH1/2/3/4 Mixer En`, `S2801 RMIX1/2 En`, `S2801 CH3 Rec En`,
  `S2801 CH1/2/3 DOUT Select`, `Chargepump Mode` (items VDD/HALF-VDD/CLASS-G-D/
  CLASS-G-A, default 3), `DNC Max Gain` (default 24), `ADCL/ADCR Mixer LINExx
  Switch` x4, `LINEIN LINEIN On`, `MIC1/MIC2 ... On`, `Line-in Volume`
  (default 6,6), `ADC Left Gain` (127), `ADC Right Gain` (151),
  `DAC Gain` (127,127), `Speaker Volume` (6), `Headphone Volume` (57,57),
  `SPK/HP/EP ... On`.
- MISMATCH (documented, script works around it): the six analog-bypass
  switches `HPL/HPR/EP/SPK Mixer ADCL/ADCR Switch` from `dev-codec-disable-
  analog-bypass`, `dev-fm-speaker`, `dev-fm-headset` do **not** exist live.
  Only `* Mixer DACL/DACR Switch` variants exist (all default off). So the live
  kernel has no analog bypass path; FM must go through the digital route
  (LINEIN ADC -> S2801 -> DAC out), which is exactly what fm-mixer.sh sets up.
  Playback-out DAC switches (`SPK/HPL/HPR Mixer DAC*`) default off and are left
  to the normal output path; fm-mixer.sh only asserts SPK/HP power (`dev-*`).
- DOUT select enums live: CH1 `DMIX_OUT/AIF4IN/RMIX_OUT` (stock RX wants
  RMIX_OUT); CH2 `DMIX_OUT/AIF4IN/AIF3IN`; CH3 `DMIX_OUT/AIF4IN/AIF2IN`.
  Stock `route-ap-fm-rx` sets only CH1 DOUT; script mirrors that exactly.

## What fm-mixer.sh does (not yet applied on device)

- `speaker`: route-ap-fm-tx + dev-fm-input + dev-speaker + gain-fm_radio-speaker
  + gain-fm_radio-fm-recording (ADC side).
- `headset`: route-ap-fm-tx + dev-fm-input + dev-headset + gain-fm_radio-headset
  + ADC gains.
- `record`: route-ap-fm-rx (+CH3 Rec) + dev-fm-input + recording gains.
- `revert`: restores every touched control to its live power-on default.
