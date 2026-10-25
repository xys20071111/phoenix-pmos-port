#!/bin/sh
# msm8916 audio defaults: proven control set for the internal WCD codec
# (LPASS DMA is hardwired, no route mixer; see BOOT-STATUS.md "音频输出打通").
# Runs from udev when the sound card appears, so settings survive reflash.
# Idempotent and quiet: every line must tolerate re-runs, hence || true.
# Card name: direct-LPASS card was "phoenix"; since the QDSP6/modem switch
# (2026-10-16) the msm8916-modem-qdsp6.dtsi card is model "msm8916".
CARD="msm8916"
[ -e "/dev/snd/controlC0" ] || exit 0

# --- playback: digital RX gains at 0dB (68%), let userspace do volume ---
amixer -c "$CARD" cset name='RX1 MIX1 INP1' 'RX1' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX2 MIX1 INP1' 'RX2' >/dev/null 2>&1 || true
# speaker (mono) hangs off RX3 which is silent on stereo: feed it RX1
amixer -c "$CARD" cset name='RX3 MIX1 INP1' 'RX1' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX1 Digital Volume' 68% >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX2 Digital Volume' 68% >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX3 Digital Volume' 68% >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX1 Mute Switch' 0 >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX2 Mute Switch' 0 >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RX3 Mute Switch' 0 >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='RDAC2 MUX' 'RX2' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='HPHL' 'Switch' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='HPHR' 'Switch' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='SPK DAC Switch' on >/dev/null 2>&1 || true

# --- capture: handset mic (AMIC1) ready; headset mic left manual ---
amixer -c "$CARD" cset name='DEC1 MUX' 'ADC1' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='CIC1 MUX' 'AMIC' >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='ADC1 Volume' 80% >/dev/null 2>&1 || true
amixer -c "$CARD" cset name='TX1 Digital Volume' 80% >/dev/null 2>&1 || true

exit 0
