# inmarsat-sniffer

A standalone Inmarsat L-band decoder written in C. Decodes STD-C (Enhanced Group Call maritime safety messages) and Aero (aviation ACARS) simultaneously from a single SDR receiver. No GNU Radio, no Python, no Java runtime -- just one binary.

Supports RTL-SDR (native librtlsdr), SDRplay (native API v3), and any SoapySDR-supported device for live capture, or processes IQ recordings from file. Built-in web dashboard (`--web`) provides a live Leaflet.js map with aircraft positions and maritime safety messages. JSON feed output (`--feed`, `--udp`) uses JAERO-compatible field names for drop-in integration with existing tools.

When [libacars-2](https://github.com/szpajder/libacars) is installed, ARINC-622 application payloads (ADS-C position reports, CPDLC controller-pilot messages) are fully decoded and reassembled -- not just raw ACARS text.

## What it decodes

- **STD-C / EGC**: NAVAREA warnings, METAREA weather forecasts, distress alerts, coastal warnings, SafetyNET messages with position extraction
- **Aero ACARS**: 600 and 1200 baud BPSK channels, plus 8400 and 10500 baud OQPSK high-rate channels
- **ADS-C / CPDLC**: Full sublayer decoding via libacars (optional, auto-detected at build time)

All protocols decoded simultaneously from a single wideband capture.

## Supported SDR hardware

Native backends (no SoapySDR needed):

- **RTL-SDR** Blog V3/V4 with LNA (`-i rtl-0`) -- cheapest option, works for narrowband modes
- **SDRplay** RSP1/RSP1A/RSP1B/RSP2/RSPduo/RSPdx (`-i sdrplay`) -- full bandwidth, bias tee support

Via SoapySDR (any device with an L-band-capable SoapySDR module):

- Airspy Mini/R2 via SoapyAirspy
- HackRF via SoapyHackRF
- LimeSDR via SoapyLimeSDR
- PlutoSDR via SoapyPlutoSDR
- BladeRF via SoapyBladeRF

You need an antenna that covers 1525-1559 MHz. A modified GPS patch antenna, a small helix, or an L-band patch will work. Point it at the satellite -- Inmarsat birds are geostationary, so once aimed, it stays put.

## Installation

### DragonOS

DragonOS has the SDR libraries and libacars pre-installed. Clone and build:

```bash
git clone https://github.com/alphafox02/inmarsat-sniffer.git
cd inmarsat-sniffer
mkdir build && cd build
cmake ..
make -j$(nproc)
```

CMake auto-detects available SDR backends and libacars. You should see all backends enabled in the cmake output.

### Ubuntu / Debian

```bash
# Build tools
sudo apt install build-essential cmake pkg-config

# At least one SDR backend (pick what you have):
sudo apt install librtlsdr-dev                    # RTL-SDR native
sudo apt install libsoapysdr-dev soapysdr-module-rtlsdr  # or via SoapySDR

# Optional: libacars for full ACARS sublayer decoding
# (build from source: https://github.com/szpajder/libacars)

git clone https://github.com/alphafox02/inmarsat-sniffer.git
cd inmarsat-sniffer
mkdir build && cd build
cmake ..
make -j$(nproc)
sudo make install   # optional, installs to /usr/local/bin
```

### SDRplay users

Install the SDRplay API v3 from [sdrplay.com](https://www.sdrplay.com/api/) first. CMake will find it automatically.

### Build output

CMake prints which backends are available:

```
-- SoapySDR: enabled
-- SDRplay: enabled
-- RTL-SDR: enabled
-- libacars: enabled
```

If a backend shows "not found", install the corresponding -dev package and re-run cmake.

## Usage

### Quick start

```bash
# RTL-SDR, decode everything on Inmarsat 4F3 (Americas, 98W)
inmarsat-sniffer -i rtl-0 --satellite=4F3

# SDRplay
inmarsat-sniffer -i sdrplay --satellite=4F3

# SoapySDR
inmarsat-sniffer -i soapy-0 --satellite=4F3
```

### With web dashboard

```bash
inmarsat-sniffer -i rtl-0 --satellite=4F3 --web
# open http://localhost:8888 in your browser
```

### JSON feed

```bash
# To stdout (pipe to jq, Node-RED, etc)
inmarsat-sniffer -i rtl-0 --satellite=4F3 --feed

# Forward via UDP (up to 4 endpoints)
inmarsat-sniffer -i rtl-0 --satellite=4F3 --udp=127.0.0.1:5555

# Both at once
inmarsat-sniffer -i rtl-0 --satellite=4F3 --feed --udp=192.168.1.100:5555
```

### Mode selection

```bash
# STD-C only (narrower BW, fine for RTL-SDR without LNA)
inmarsat-sniffer -i rtl-0 --satellite=4F3 --mode=stdc

# Aero only
inmarsat-sniffer -i rtl-0 --satellite=4F3 --mode=aero

# Full (both) -- this is the default when bandwidth allows
inmarsat-sniffer -i rtl-0 --satellite=4F3 --mode=full
```

### File input

```bash
# Format auto-detected from extension
inmarsat-sniffer -f recording.cf32 --satellite=4F3
inmarsat-sniffer -f recording.ci16 --satellite=4F3

# Explicit format
inmarsat-sniffer -f recording.raw --format=ci8 --satellite=4F3

# From stdin
cat recording.cf32 | inmarsat-sniffer -f - --format=cf32 --satellite=4F3
```

### List devices and satellites

```bash
inmarsat-sniffer --list              # show available SDR devices
inmarsat-sniffer --list-satellites   # show known satellite channel tables
```

## Satellites

| Flag | Name | Position | Region | Notes |
|------|------|----------|--------|-------|
| `4F3` | Inmarsat 4-F3 | 98.0W | Americas (AORW) | Best from North/South America |
| `3F5` | Inmarsat 3-F5 | 54.0W | Atlantic (AORE) | Visible from eastern US and Europe |
| `AF1` | Alphasat / I4-AF1 | 25.0E | Indian Ocean (IOR) | Europe, Africa, Middle East |
| `F1`  | Inmarsat 4-F1 | 143.5E | Pacific (POR) | Asia-Pacific, Australia |

The `--satellite` flag selects the channel frequency table. Center frequency and sample rate are computed automatically based on the satellite's channel plan and your SDR's bandwidth.

## Web dashboard

Start with `--web` (default port 8888) or `--web=9090` for a custom port.

- Dark-themed Leaflet map with CartoDB basemap
- Aircraft position markers with trail history from ADS-C reports
- STD-C/EGC message markers with decoded text
- Live scrolling message feed panel
- Layer toggle controls for STD-C and Aero overlays
- Server-Sent Events for 1 Hz live updates, no polling

## JSON output

The `--feed` and `--udp` options emit one JSON object per line.

### Aero / ACARS

Uses JAERO-compatible field names so existing feed consumers work without changes:

```json
{
  "source": "inmarsat-sniffer",
  "satellite": "4F3",
  "TIME": 1710720000,
  "TIME_UTC": "2026-03-18 01:00:00",
  "NONACARS": false,
  "REG": "N12345",
  "FLIGHT": "AA100",
  "MODE": "2",
  "LABEL": "H1",
  "BI": "6",
  "TAK": "!",
  "MESSAGE": "POSRPT LAT 40.123 LON -73.456 ...",
  "lat": 40.123,
  "lon": -73.456,
  "alt": 35000,
  "channel": 5
}
```

### STD-C / EGC

```json
{
  "source": "inmarsat-sniffer",
  "type": "egc",
  "satellite": "4F3",
  "service": 4,
  "priority": 1,
  "text": "NAVAREA IV 0123/26 EASTERN ATLANTIC ...",
  "lat": 35.5,
  "lon": -45.2,
  "timestamp": 1710720000.123
}
```

## All options

```
Input (one required):
  -f, --file=FILE         Read IQ samples from file (or - for stdin)
  -l, --live              Capture live from SDR (implied by -i)
  --format=FMT            IQ format: ci8 (default), ci16, cf32

SDR device:
  -i, --interface=IFACE   rtl-N, sdrplay[-SERIAL], soapy-N, soapy:driver=X
  -r, --sample-rate=HZ    Sample rate (default: auto from satellite)
  -B, --bias-tee          Enable bias tee power
  --soapy-gain=DB         Overall gain in dB (default: 40)
  --soapy-gain-element=NAME:DB   Per-element gain (repeatable)
  --soapy-setting=K:V     SoapySDR device setting (repeatable)
  --sdrplay-gain=VAL      SDRplay gain reduction (default: AGC)

Satellite:
  --satellite=SAT         Satellite designator: 4F3, 3F5, AF1, F1
  --mode=MODE             auto (default), aero, stdc, full

Output:
  --web[=PORT]            Web dashboard (default port: 8888)
  --feed                  JSON lines to stdout
  --udp=HOST:PORT         Send JSON via UDP (repeatable, max 4)
  -v, --verbose           Verbose output to stderr

Info:
  --list                  List available SDR devices
  --list-satellites       List known satellite channel tables
  -h, --help              Show help
```

## How it works

```
SDR/file --> channelizer (DDC per channel)
                |
                +-- STD-C EGC ch --> DBPSK demod --> Viterbi k=7 --> EGC parser --+
                |                                                                 |
                +-- Aero 600/1200 --> BPSK demod ----+                            |
                |                                    +--> aero decoder            |
                +-- Aero 8400/10500 --> OQPSK demod -+    (deinterleave,          |
                                                          descramble, CRC,        |
                                                          ACARS extract)          |
                                                          |                       |
                                                          +-- libacars (optional) |
                                                          |   ADS-C, CPDLC       |
                                                          |                       |
                                                          v                       v
                                                     JSON feed / web dashboard / stderr
```

## Bandwidth and SDR selection

The auto mode picks what to decode based on your SDR's sample rate:

- **~600 kHz**: STD-C EGC only (one narrowband channel)
- **~2.4 MHz**: STD-C + a few Aero channels (RTL-SDR sweet spot)
- **~6-10 MHz**: Full satellite channel plan (SDRplay, Airspy, etc)

An RTL-SDR Blog V3 at 2.4 Msps can decode STD-C and several Aero channels simultaneously. For the full channel plan you need a wider-bandwidth SDR like an RSP1A or Airspy.

## Related projects

- [JAERO](https://github.com/jontio/JAERO) -- Aero ACARS decoder (Qt GUI, Windows/Linux)
- [SatDump](https://github.com/SatDump/SatDump) -- Multi-satellite decoder with Inmarsat-C support
- [Scytale-C](https://github.com/cropinghigh/sdrpp-inmarsat-demodulator) -- SDR++ Inmarsat-C plugin
- [inmarsatc](https://github.com/cropinghigh/inmarsatc) -- Inmarsat-C decoder library (protocol reference)
- [stdcdec](https://github.com/cropinghigh/stdcdec) -- Standalone STD-C decoder using inmarsatc
- [libacars](https://github.com/szpajder/libacars) -- ACARS/ARINC-622 message decoder library
- [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer) -- Sister project for Iridium L-band

## License

GPL-3.0-or-later. Copyright (c) 2026 CEMAXECUTER LLC.
