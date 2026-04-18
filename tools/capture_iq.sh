#!/bin/bash
#
# capture_iq.sh — Record IQ samples for inmarsat-sniffer development
#
# Captures raw IQ from RTL-SDR or SDRplay, centered on the correct
# satellite frequency with the right sample rate. Output file can be
# played back with:
#
#   inmarsat-sniffer -f capture.cu8 --format=cu8 --satellite=4F3
#   inmarsat-sniffer -f capture.cs16 --format=ci16 --satellite=4F3
#
# Usage:
#   ./capture_iq.sh --sdr=rtl --satellite=4F3
#   ./capture_iq.sh --sdr=rtl --satellite=4F3 --bias-tee
#   ./capture_iq.sh --sdr=sdrplay --satellite=4F3
#   ./capture_iq.sh --sdr=sdrplay --satellite=4F3 --rate=6000000
#

set -e

# Defaults
SDR=""
SAT=""
BIAS_TEE=0
MAX_MB=500
RATE=""
GAIN=""
OUTPUT=""

usage() {
    cat <<EOF
Usage: $0 --sdr=TYPE --satellite=SAT [options]

Required:
  --sdr=TYPE         SDR type: rtl or sdrplay
  --satellite=SAT    Satellite: 4F3, 3F5, AF1, F1

Options:
  --bias-tee         Enable bias tee power
  --rate=HZ          Sample rate (default: 2400000 for rtl, 6000000 for sdrplay)
  --gain=DB          Tuner gain in dB (default: max for rtl, 40 for sdrplay)
  --max-mb=MB        Maximum file size in MB (default: 500)
  --output=FILE      Output filename (default: auto-generated)
  -h, --help         Show this help

Output format:
  RTL-SDR:  unsigned 8-bit IQ (.cu8)  — playback: --format=cu8
  SDRplay:  signed 16-bit IQ (.cs16)  — playback: --format=ci16

Example:
  $0 --sdr=rtl --satellite=4F3 --bias-tee
  $0 --sdr=sdrplay --satellite=4F3 --rate=6000000

Then send the file for analysis. Playback:
  inmarsat-sniffer -f capture_4F3_rtl_20260418.cu8 --format=cu8 --satellite=4F3
EOF
    exit "${1:-0}"
}

# Satellite center frequencies (Hz) — aero mode, matches inmarsat-sniffer auto-tune
sat_center() {
    case "$1" in
        4F3) echo 1545604000 ;;  # 98W Americas
        3F5) echo 1545152000 ;;  # 54W Atlantic
        AF1) echo 1545112000 ;;  # 25E Indian Ocean
        F1)  echo 1545118000 ;;  # 143.5E Pacific
        *)   echo "Unknown satellite: $1" >&2; exit 1 ;;
    esac
}

# Parse arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --sdr=*)      SDR="${1#*=}" ;;
        --satellite=*) SAT="${1#*=}" ;;
        --bias-tee)   BIAS_TEE=1 ;;
        --rate=*)     RATE="${1#*=}" ;;
        --gain=*)     GAIN="${1#*=}" ;;
        --max-mb=*)   MAX_MB="${1#*=}" ;;
        --output=*)   OUTPUT="${1#*=}" ;;
        -h|--help)    usage 0 ;;
        *)            echo "Unknown option: $1" >&2; usage 1 ;;
    esac
    shift
done

[ -z "$SDR" ] && { echo "Error: --sdr required (rtl or sdrplay)" >&2; usage 1; }
[ -z "$SAT" ] && { echo "Error: --satellite required (4F3, 3F5, AF1, F1)" >&2; usage 1; }

CENTER=$(sat_center "$SAT")
DATE=$(date +%Y%m%d_%H%M%S)

case "$SDR" in
    rtl)
        [ -z "$RATE" ] && RATE=2400000
        [ -z "$GAIN" ] && GAIN=49.6
        EXT="cu8"
        [ -z "$OUTPUT" ] && OUTPUT="capture_${SAT}_rtl_${DATE}.${EXT}"

        # Calculate samples for max file size
        # cu8 = 2 bytes per complex sample (1 byte I + 1 byte Q)
        MAX_BYTES=$((MAX_MB * 1024 * 1024))
        MAX_SAMPLES=$((MAX_BYTES / 2))
        DURATION=$((MAX_SAMPLES / RATE))

        echo "=== IQ Capture ==="
        echo "SDR:       RTL-SDR"
        echo "Satellite: $SAT"
        echo "Center:    $(echo "scale=3; $CENTER / 1000000" | bc) MHz"
        echo "Rate:      $(echo "scale=1; $RATE / 1000000" | bc) MHz"
        echo "Gain:      ${GAIN} dB"
        echo "Bias-T:    $([ $BIAS_TEE -eq 1 ] && echo ON || echo OFF)"
        echo "Format:    cu8 (unsigned 8-bit IQ)"
        echo "Max size:  ${MAX_MB} MB (~${DURATION}s)"
        echo "Output:    $OUTPUT"
        echo ""

        if ! command -v rtl_sdr &>/dev/null; then
            echo "Error: rtl_sdr not found. Install rtl-sdr tools." >&2
            exit 1
        fi

        if [ $BIAS_TEE -eq 1 ]; then
            if command -v rtl_biast &>/dev/null; then
                echo "Enabling bias tee via rtl_biast..."
                rtl_biast -b 1 2>/dev/null || true
            else
                echo "Warning: rtl_biast not found, bias tee not enabled." >&2
                echo "Install rtl-sdr-blog tools for bias tee support." >&2
            fi
        fi

        echo "Recording... press Ctrl+C to stop early."
        echo ""
        rtl_sdr -f "$CENTER" -s "$RATE" -g "$GAIN" -n "$MAX_SAMPLES" "$OUTPUT"

        # Disable bias tee after recording
        if [ $BIAS_TEE -eq 1 ] && command -v rtl_biast &>/dev/null; then
            rtl_biast -b 0 2>/dev/null || true
        fi
        ;;

    sdrplay)
        [ -z "$RATE" ] && RATE=6000000
        [ -z "$GAIN" ] && GAIN=40
        EXT="cs16"
        [ -z "$OUTPUT" ] && OUTPUT="capture_${SAT}_sdrplay_${DATE}.${EXT}"

        # cs16 = 4 bytes per complex sample (2 bytes I + 2 bytes Q)
        MAX_BYTES=$((MAX_MB * 1024 * 1024))
        MAX_SAMPLES=$((MAX_BYTES / 4))
        DURATION=$((MAX_SAMPLES / RATE))

        echo "=== IQ Capture ==="
        echo "SDR:       SDRplay (via SoapySDR)"
        echo "Satellite: $SAT"
        echo "Center:    $(echo "scale=3; $CENTER / 1000000" | bc) MHz"
        echo "Rate:      $(echo "scale=1; $RATE / 1000000" | bc) MHz"
        echo "Gain:      ${GAIN} dB"
        echo "Bias-T:    $([ $BIAS_TEE -eq 1 ] && echo ON || echo OFF)"
        echo "Format:    cs16 (signed 16-bit IQ)"
        echo "Max size:  ${MAX_MB} MB (~${DURATION}s)"
        echo "Output:    $OUTPUT"
        echo ""

        if ! command -v rx_sdr &>/dev/null; then
            echo "Error: rx_sdr not found. Install SoapySDR utils:" >&2
            echo "  sudo apt install soapysdr-tools" >&2
            exit 1
        fi

        BIAS_ARG=""
        [ $BIAS_TEE -eq 1 ] && BIAS_ARG="-t biasT_ctrl=true"

        echo "Recording... press Ctrl+C to stop early."
        echo ""
        rx_sdr -d driver=sdrplay -f "$CENTER" -s "$RATE" -g "$GAIN" \
            -F CS16 $BIAS_ARG -n "$MAX_SAMPLES" "$OUTPUT"
        ;;

    *)
        echo "Error: unknown SDR type '$SDR' (use rtl or sdrplay)" >&2
        exit 1
        ;;
esac

SIZE=$(du -h "$OUTPUT" | cut -f1)
echo ""
echo "=== Done ==="
echo "Saved: $OUTPUT ($SIZE)"
echo ""
echo "Playback command:"
case "$SDR" in
    rtl)     echo "  inmarsat-sniffer -f $OUTPUT --format=cu8 --satellite=$SAT -r $RATE" ;;
    sdrplay) echo "  inmarsat-sniffer -f $OUTPUT --format=ci16 --satellite=$SAT -r $RATE" ;;
esac
