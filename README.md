# inmarsat-sniffer

A standalone Inmarsat L-band decoder written in C. Decodes STD-C (Enhanced Group Call maritime safety messages) and Aero (aviation ACARS, ADS-C, CPDLC) simultaneously from a single SDR receiver. No GNU Radio, no Python, no Java runtime -- just one binary.

Supports HackRF, BladeRF, USRP (UHD), RTL-SDR, SDRplay (native API v3), and any SoapySDR device for live capture, plus VITA 49 (VRT) UDP input and IQ file playback. Built-in web dashboard (`--web`) provides a real-time Leaflet.js map with aircraft positions and decoded messages. Outputs include JSON feed (`--feed`, `--udp`), SBS/BaseStation (`--basestation`) for tar1090/VRS, and MQTT (`--mqtt`) -- all with JAERO-compatible field names for drop-in integration with existing tools.

The Aero decode chain uses JAERO's proven DSP code (MskDemodulator, BurstOqpskDemodulator, AeroL) ported from [jontio/JAERO](https://github.com/jontio/JAERO), Qt-stripped to pure C++. When [libacars-2](https://github.com/szpajder/libacars) is installed, ARINC-622 application payloads (ADS-C position reports, CPDLC controller-pilot messages) are fully decoded and reassembled.

Sister project to [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer) for Iridium L-band.

## Features

- Simultaneous STD-C EGC + Aero ACARS decode from one SDR
- 27-channel parallel demodulation with per-channel worker threads
- Aero 600/1200 baud MSK (P-channel continuous, via JAERO MskDemodulator + AeroL)
- Aero 10500 baud OQPSK (continuous forward link, via JAERO OqpskDemodulator + AeroL)
- Aero 8400 baud OQPSK (C-channel, via JAERO BurstOqpskDemodulator + AeroL)
- STD-C EGC: DBPSK demod, Viterbi k=7 FEC, frame sync and message parsing
- ADS-C position extraction from binary ARINC 620 payloads (tags 7/9/10/14/15/18/19/20)
- CPDLC (controller-pilot datalink) message surfacing via libacars
- Three-tier position extraction: ADS-C binary, coordinate regex, waypoint DB (125k fixes)
- Learned waypoints harvested from FPN (flight plan) messages at runtime
- SBS/BaseStation output (`--basestation`) for tar1090, VRS, PlanePlotter
- MQTT output (`--mqtt`) with configurable host, user/pass, topic
- JSON feed with station-id (`--feed`, `--udp`, `--station-id`)
- Built-in web dashboard with dark theme, aircraft markers, trail history, CSV export
- Aircraft database (568k entries from tar1090-db) for registration-to-ICAO-hex lookup
- AVX2, SSE4.2, and NEON SIMD kernels with automatic runtime detection
- ZMQ audio output (`-z`) for JAERO-compatible per-channel streaming
- VITA 49 (VRT) UDP input for remote/distributed SDR setups
- HackRF, BladeRF, USRP, RTL-SDR, SDRplay, and SoapySDR native backends
- MacOS (Homebrew) build support on Intel and Apple Silicon

## What it decodes

- **STD-C / EGC**: NAVAREA warnings, METAREA weather, distress alerts, SafetyNET
- **Aero ACARS**: 600/1200 baud MSK P-channel + 8400/10500 baud OQPSK C-channel
- **ADS-C**: Binary position reports with lat/lon/alt/heading/groundspeed
- **CPDLC**: Controller-pilot text messages (uplink clearances, downlink reports)

All decoded simultaneously from a single wideband capture.

## Installation

### DragonOS Noble

DragonOS has the SDR libraries and libacars pre-installed. Just clone and build:

```bash
git clone https://github.com/alphafox02/inmarsat-sniffer.git
cd inmarsat-sniffer
mkdir build && cd build
cmake ..
make -j$(nproc)
```

CMake auto-detects available backends. All should show "enabled".

### Ubuntu / Debian

```bash
git clone https://github.com/alphafox02/inmarsat-sniffer.git
cd inmarsat-sniffer

# Core dependencies
sudo apt install build-essential cmake pkg-config


# SDR libraries (install what you have)
sudo apt install librtlsdr-dev       # RTL-SDR native
sudo apt install libhackrf-dev       # HackRF One
sudo apt install libbladerf-dev      # BladeRF
sudo apt install libuhd-dev          # USRP (B2x0, N2x0, etc.)
sudo apt install libsoapysdr-dev     # SoapySDR (any device)
# SDRplay native API: install from https://www.sdrplay.com/api/

# Optional: ACARS ARINC-622/ADS-C/CPDLC
sudo apt install libacars-dev        # libacars-2 (or build from source)

# Optional: ZMQ audio output for external JAERO
sudo apt install libzmq3-dev

# Optional: MQTT broker publishing
sudo apt install libmosquitto-dev

mkdir build && cd build
cmake ..
make -j$(nproc)
```

### macOS (Homebrew)

```bash
brew install cmake librtlsdr hackrf libbladerf uhd soapysdr libacars mosquitto zmq
git clone https://github.com/alphafox02/inmarsat-sniffer.git
cd inmarsat-sniffer && mkdir build && cd build
cmake .. && make -j$(sysctl -n hw.ncpu)
```

### SDRplay users

Install the SDRplay API v3 from [sdrplay.com](https://www.sdrplay.com/api/) first. CMake finds it automatically.

### Build output

```
-- SoapySDR: enabled
-- SDRplay: enabled
-- RTL-SDR: enabled
-- HackRF: enabled
-- BladeRF: enabled
-- USRP (UHD): enabled
-- SIMD: SSE4.2 + AVX2+FMA kernels enabled (runtime-detected)
-- MQTT: enabled
-- ZMQ: enabled
-- libacars: enabled
```

## Supported SDR hardware

Native backends (no SoapySDR needed):

| Flag | Device | Notes |
|------|--------|-------|
| `-i rtl-0` | RTL-SDR Blog V3/V4 | Cheapest, 2.4 MHz BW, aero-only mode |
| `-i hackrf[-SERIAL]` | HackRF One | 20 MHz BW, good for full mode |
| `-i bladerf0` | BladeRF x40/xA4/micro | Up to 56 MHz BW |
| `-i usrp-PRODUCT-SERIAL` | Ettus USRP | B205mini, B210, N210, X310 |
| `-i sdrplay[-SERIAL]` | SDRplay RSP family | RSPdx recommended, 10 MHz BW, bias tee |
| `-i soapy-N` or `soapy:args` | Any SoapySDR device | Airspy, LimeSDR, PlutoSDR, etc. |
| `--vita49[=IP:PORT]` | VITA 49 (VRT) UDP | Remote SDR via network |

You need an antenna covering 1525-1559 MHz. A modified GPS patch, small helix, or L-band patch works. Point at the satellite -- Inmarsat birds are geostationary.

## Usage

### Quick start

```bash
# SDRplay, decode Aero ACARS on Inmarsat 4F3 (Americas, 98W)
inmarsat-sniffer -i sdrplay --satellite=4F3

# RTL-SDR
inmarsat-sniffer -i rtl-0 --satellite=4F3 --mode=aero

# HackRF
inmarsat-sniffer -i hackrf --satellite=4F3 -B
```

### With web dashboard + SBS feed

```bash
inmarsat-sniffer -i sdrplay --satellite=4F3 --web --basestation=30003
# Web map: http://localhost:8888
# SBS feed: connect tar1090/VRS to localhost:30003
```

### Push SBS to a remote aggregator

```bash
inmarsat-sniffer -i sdrplay --satellite=4F3 --basestation=myhost.net:2226
```

### JSON feed

```bash
# To stdout
inmarsat-sniffer -i rtl-0 --satellite=4F3 --feed --station-id=MY-STATION

# Via UDP (up to 4 endpoints)
inmarsat-sniffer -i rtl-0 --satellite=4F3 --udp=127.0.0.1:5555
```

### MQTT

```bash
inmarsat-sniffer -i sdrplay --satellite=4F3 \
    --mqtt=broker.local:1883 --mqtt-user=user --mqtt-pass=pass \
    --mqtt-topic=inmarsat/acars
```

### Aircraft database

```bash
# Download tar1090-db aircraft.csv (568k entries, ~33 MB)
inmarsat-sniffer --update-db

# Use for reg-to-ICAO-hex in SBS output
inmarsat-sniffer -i sdrplay --satellite=4F3 --basestation=30003
```

### Mode selection

```bash
--mode=aero    # Aero channels only (default, 2.4 MHz SDR works)
--mode=stdc    # STD-C EGC only
--mode=full    # Both (needs wider SDR, ~9 MHz)
--mode=auto    # Auto-select based on SDR bandwidth
```

## Satellites

| Flag | Name | Position | Region | Notes |
|------|------|----------|--------|-------|
| `4F3` | Inmarsat 4-F3 | 98.0W | Americas (AORW) | Best from North/South America |
| `3F5` | Inmarsat 3-F5 | 54.0W | Atlantic (AORE) | Eastern US and Europe |
| `AF1` | Alphasat / I4-AF1 | 25.0E | Indian Ocean (IOR) | Europe, Africa, Middle East |
| `F1`  | Inmarsat 4-F1 | 143.5E | Pacific (POR) | Asia-Pacific, Australia |

## Architecture

```
SDR/file/VITA49 --> channelizer (DDC per channel, SIMD-accelerated)
                        |
                        +-- STD-C EGC --> DBPSK demod --> Viterbi k=7 --> frame parser
                        |
                        +-- Aero 600/1200 --> JAERO MskDemodulator --> AeroL --+
                        |                    (continuous MSK, AFC)              |
                        +-- Aero 10500 ------> JAERO OqpskDemodulator ------->+
                        |                    (continuous OQPSK, AFC)            |
                        +-- Aero 8400 -------> JAERO BurstOqpskDemodulator -->+
                                                                               |
                                                                     libacars (optional)
                                                                     ADS-C, CPDLC, ACARS
                                                                               |
                                                               +---------------+---------------+
                                                               |               |               |
                                                          JSON feed      SBS/BaseStation     MQTT
                                                          (--feed/--udp)  (--basestation)  (--mqtt)
                                                               |
                                                          Web dashboard
                                                          (--web :8888)
```

## Bandwidth and SDR selection

| SDR bandwidth | Channels covered | Recommended hardware |
|---------------|-----------------|---------------------|
| ~2.4 MHz | 12 MSK P-channels (aero-only) | RTL-SDR Blog V3/V4 |
| ~3.2 MHz | 12 MSK + some OQPSK | RTL-SDR Blog V4 (higher rate) |
| ~6-10 MHz | Full 27-channel plan (MSK + OQPSK) | SDRplay RSPdx, Airspy R2 |
| ~10+ MHz | Full + STD-C (--mode=full) | SDRplay RSPdx, BladeRF, USRP |

Channels are automatically filtered based on your SDR's actual bandwidth -- the channelizer only adds channels whose center frequency falls within the captured spectrum. Channels outside the bandwidth are skipped (visible with `-v`). This means an RTL-SDR at 2.4 MHz simply decodes fewer channels, not incorrectly -- no wasted CPU on out-of-band noise. Center frequency and sample rate are auto-computed from the satellite table unless you override with `-c` and `-r`.

## Current status

**Working and verified live:**

- 600/1200 baud MSK P-channel ACARS decode (tested 9000+ messages, 220+ aircraft, 100% CRC pass)
- ADS-C position extraction (oceanic aircraft tracked across North/South Atlantic, Americas)
- CPDLC controller-pilot messages decoded via libacars
- SBS basestation feed verified with remote aggregator
- Per-channel threading with zero drops over multi-hour runs
- Web dashboard with live aircraft markers and trail history

**Partially verified:**

- 10500 baud OQPSK forward link -- continuous OqpskDemodulator ported, first ACARS decode observed on ch15. Signal is weak on L-band forward link and traffic is sporadic. Antenna positioning matters
- 8400 baud OQPSK C-channel -- burst demod wired, not yet verified (may need C-band dish for return link)
- STD-C EGC decode -- code path active, demod searches but hasn't synced in testing. May need stronger signal or different satellite
- Auto-calibration corrects SDR crystal PPM offset at startup (enables RTL-SDR and other SDRs with less accurate clocks)

**Not implemented:**

- Voice decoding (Inmarsat Aero carries AMBE-encoded voice on C-channel slots; would require mbelib or similar AMBE codec, same approach as DSD/OP25)
- R/T burst channel frequencies (not in satellite tables; would need frequency survey)
- C-band feeder link reception (same OQPSK demod works, just needs C-band dish + downconverter + frequency entries)

## Related projects

- [iridium-sniffer](https://github.com/alphafox02/iridium-sniffer) -- Sister project for Iridium L-band
- [JAERO](https://github.com/jontio/JAERO) -- Aero ACARS decoder (Qt GUI), DSP code ported here
- [libacars](https://github.com/szpajder/libacars) -- ACARS/ARINC-622 message decoder library
- [SatDump](https://github.com/SatDump/SatDump) -- Multi-satellite decoder
- [Scytale-C](https://github.com/cropinghigh/sdrpp-inmarsat-demodulator) -- SDR++ Inmarsat-C plugin
- [inmarsatc](https://github.com/cropinghigh/inmarsatc) -- Inmarsat-C decoder library
- [stdcdec](https://github.com/cropinghigh/stdcdec) -- Standalone STD-C decoder
- [gr-JAERO](https://github.com/muaddib1984/gr-JAERO) -- GNU Radio Inmarsat Aero RF front-end

## License

GPL-3.0-or-later. Copyright (c) 2026 CEMAXECUTER LLC.

DSP code in `jaero_dsp/` is derived from [JAERO](https://github.com/jontio/JAERO) by Jonathan Olds, MIT license.
