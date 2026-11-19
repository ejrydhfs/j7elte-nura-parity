#!/bin/sh
# fm-mixer.sh - route BCM43438 FM analog audio through cod3022x + S2801 ABOX.
# Derived from stock etc/mixer_paths.xml (system.raw) + live `amixer -c0` evidence.
# See fm-audio-route.md for the full path diagram and control mapping.
#
# Usage:
#   ./fm-mixer.sh speaker   # FM -> speaker  (digital S2801 path + LINEIN ADC + DAC out)
#   ./fm-mixer.sh headset   # FM -> headset
#   ./fm-mixer.sh record    # FM capture path only (LINEIN ADC + S2801 CH3 rec)
#   ./fm-mixer.sh revert    # back to power-on defaults (captured from live device)
#
# DO NOT RUN YET: batch 2 applies this after the bcmfm daemon brings up the
# HCI VS 0xFC15 FM tuning path. Every control name below was verified to exist
# on the live device (`amixer -c0 scontrols`, 101 controls, 2026-11-18).
#
# Stock controls that do NOT exist live (analog bypass) are documented in
# fm-audio-route.md and skipped here; the digital route below replaces them.
set -eu

CARD=0
cset() { amixer -c$CARD cset name="$1" "$2" >/dev/null; }

route_ap_fm() {
    # <path name="route-ap-fm">: enable S2801 mixer + SRC3 (FM AIF4 lane).
    cset 'S2801 Mixer En' On
    cset 'S2801 SRC3 En' On
}

route_ap_fm_tx() {
    # <path name="route-ap-fm-tx">: playback direction.
    route_ap_fm
    cset 'S2801 CH1 Mixer En' On
    cset 'S2801 CH2 Mixer En' Off
    cset 'S2801 CH3 Mixer En' On
    cset 'S2801 CH4 Mixer En' Off
    cset 'Chargepump Mode' CLASS-G-A
    cset 'DNC Max Gain' 24
}

route_ap_fm_rx() {
    # <path name="route-ap-fm-rx"> (+ fm_radio-fm-recording extra CH3 Rec):
    # capture direction. CH1 DOUT=RMIX_OUT carries the FM PCM to AP capture.
    route_ap_fm
    cset 'S2801 RMIX1 En' Off
    cset 'S2801 RMIX2 En' On
    cset 'S2801 CH3 Rec En' On
    cset 'S2801 CH1 DOUT Select' RMIX_OUT
}

dev_fm_input() {
    # <path name="dev-fm-input">: codec ADC takes FM analog from LINEIN pins.
    cset 'ADCL Mixer MIC1L Switch' off
    cset 'ADCL Mixer MIC2L Switch' off
    cset 'ADCL Mixer LINELL Switch' on
    cset 'ADCL Mixer LINERL Switch' off
    cset 'ADCR Mixer MIC1R Switch' off
    cset 'ADCR Mixer MIC2R Switch' off
    cset 'ADCR Mixer LINELR Switch' on
    cset 'ADCR Mixer LINERR Switch' off
    cset 'MIC2 MIC2 On' off
    cset 'MIC1 MIC1 On' off
    cset 'LINEIN LINEIN On' on
}

gain_fm_recording() {
    # <path name="gain-fm_radio-fm-recording">
    cset 'Line-in Volume' 4
    cset 'ADC Left Gain' 127
    cset 'ADC Right Gain' 127
}

dev_speaker() {
    # <path name="dev-speaker"> (stock dev-codec-disable-analog-bypass folded in;
    # the *ADCL/*ADCR bypass switches do not exist on this kernel - see md).
    cset 'HP HP On' off
    cset 'EP EP On' off
    cset 'SPK SPK On' on
}

dev_headset() {
    # <path name="dev-headset">
    cset 'EP EP On' off
    cset 'SPK SPK On' off
    cset 'HP HP On' on
}

gain_fm_speaker() {
    # <path name="gain-fm_radio-speaker">
    cset 'Line-in Volume' 4
    cset 'Speaker Volume' 6
    cset 'DAC Gain' 121
}

gain_fm_headset() {
    # <path name="gain-fm_radio-headset">
    cset 'Line-in Volume' 6
}

case "${1:-}" in
    speaker)
        route_ap_fm_tx
        dev_fm_input
        dev_speaker
        gain_fm_speaker
        gain_fm_recording
        ;;
    headset)
        route_ap_fm_tx
        dev_fm_input
        dev_headset
        gain_fm_headset
        gain_fm_recording
        ;;
    record)
        route_ap_fm_rx
        dev_fm_input
        gain_fm_recording
        ;;
    revert)
        # Power-on defaults captured live via `amixer -c0 contents`.
        cset 'S2801 Mixer En' On
        cset 'S2801 SRC3 En' Off
        cset 'S2801 CH1 Mixer En' Off
        cset 'S2801 CH2 Mixer En' Off
        cset 'S2801 CH3 Mixer En' Off
        cset 'S2801 CH4 Mixer En' Off
        cset 'S2801 RMIX1 En' Off
        cset 'S2801 RMIX2 En' Off
        cset 'S2801 CH3 Rec En' Off
        cset 'S2801 CH1 DOUT Select' DMIX_OUT
        cset 'S2801 CH2 DOUT Select' DMIX_OUT
        cset 'S2801 CH3 DOUT Select' DMIX_OUT
        cset 'Chargepump Mode' CLASS-G-A
        cset 'DNC Max Gain' 24
        cset 'ADCL Mixer MIC1L Switch' off
        cset 'ADCL Mixer MIC2L Switch' off
        cset 'ADCL Mixer LINELL Switch' off
        cset 'ADCL Mixer LINERL Switch' off
        cset 'ADCR Mixer MIC1R Switch' off
        cset 'ADCR Mixer MIC2R Switch' off
        cset 'ADCR Mixer LINELR Switch' off
        cset 'ADCR Mixer LINERR Switch' off
        cset 'LINEIN LINEIN On' off
        cset 'MIC1 MIC1 On' off
        cset 'MIC2 MIC2 On' off
        cset 'HP HP On' off
        cset 'EP EP On' off
        cset 'SPK SPK On' off
        cset 'Line-in Volume' 6,6
        cset 'ADC Left Gain' 127
        cset 'ADC Right Gain' 151
        cset 'DAC Gain' 127,127
        cset 'Speaker Volume' 6
        cset 'Headphone Volume' 57,57
        ;;
    *) echo "usage: $0 {speaker|headset|record|revert}" >&2; exit 1 ;;
esac
echo "fm-mixer: $1 applied"
