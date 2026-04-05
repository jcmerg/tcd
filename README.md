# tcd — Hybrid Digital Voice Transcoder

Fork of [n7tae/tcd](https://github.com/n7tae/tcd) with bug fixes and usability improvements.

## Overview

tcd is a hybrid transcoder for the [URF reflector](https://github.com/jcmerg/urfd). It bridges between hardware-based vocoders (DVSI AMBE) and software-based vocoders (Codec2 for M17, IMBE for P25, and optionally MD380 for DMR/YSF).

## Requirements

- Linux (systemd-based, Debian/Ubuntu recommended)
- [imbe_vocoder](https://github.com/jcmerg/imbe_vocoder) library
- [libftd2xx](https://ftdichip.com/drivers/d2xx-drivers/) FTDI driver
- DVSI AMBE hardware (see Device Configurations below)
- [md380_vocoder](https://github.com/jcmerg/md380_vocoder) *(optional — required only when building with `md380=true`)*

### Hardware Requirements by Build Type

| Build | Min. Hardware | Enabled Features |
|-------|---------------|------------------|
| `make` (default) | 2× AMBE3000 (DV3000), **or** 1× AMBE3003 (DV3003), **or** DV3000+DV3003 | All codecs; no software DMR fallback |
| `make md380=true` | 1× AMBE device (DV3000 or DV3003) | All codecs + software DMR via MD380, DMR re-encode after AGC |

## Device Configurations

tcd auto-detects the connected devices and selects the appropriate mode:

### 1. Mixed Mode: DV3000 + DV3003 (recommended, 2 modules)

Use a DV3000 (1 D-Star channel) together with a DV3003 (1 D-Star + 2 DMR channels) for concurrent 2-module cross-mode transcoding. With `md380=true` build and `DMRReEncode = true`, DMR/YSF output is re-encoded via MD380 software vocoder to apply AGC-normalized audio levels. Without md380, AGC still applies to all cross-mode paths (e.g. DMR→D-Star) but DMR→DMR passes through the original AMBE unchanged.

```ini
Modules = FS            # 2 modules (first = DV3000 D-Star, second = DV3003 mixed)
DeviceSerial = DQ015SBR # DV3000
DeviceSerial = DKB7FXGE # DV3003
```

Channel assignment (automatic):
- **DV3000 ch0**: D-Star for first module (F)
- **DV3003 ch0**: D-Star for second module (S)
- **DV3003 ch1**: DMR for first module (F)
- **DV3003 ch2**: DMR for second module (S)

### 2. Single Device + MD380 (1 module, requires `md380=true` build)

One DVSI device for D-Star, MD380 software vocoder for DMR/YSF. Only supports 1 transcoded module. Works on ARM and x86_64. Build with `make md380=true` (or `build.sh --with-md380`).

```ini
Modules = S
DeviceSerial = DQ015SBR
```

**AGC limitation**: In this mode, DMR/YSF→DMR/YSF audio is **not re-encoded** after AGC. The original AMBE data passes through unchanged because the MD380 cannot safely decode and re-encode in the same pipeline (shared encoder/decoder state). AGC still normalizes audio for all cross-mode paths (DMR→D-Star, DMR→M17, etc.). To get AGC on DMR/YSF→DMR/YSF, use a two-device configuration.

### 3. Two Same-Type Devices (1-3 modules)

Two DV3000s (1 module) or two DV3003s (up to 3 modules). One device handles D-Star, the other DMR/YSF. With `md380=true` build, DMR re-encode after AGC is available (same as Mixed Mode).

```ini
Modules = AFS           # up to 3 with DV3003 pair
```

## Device Discovery

List connected FTDI devices and their serial numbers:

```bash
tcd --list-devices
```

If you have other FTDI devices (USB serial adapters etc.), use `DeviceSerial` in tcd.ini to whitelist your AMBE device by serial number. The tcd automatically unbinds ftdi_sio from AMBE devices at startup.

## Build & Install

### Quick start (build.sh)

```bash
git clone https://github.com/jcmerg/tcd.git
cd tcd
sudo bash build.sh               # without md380 (requires ≥2 AMBE devices)
sudo bash build.sh --with-md380  # with md380 (1 AMBE device sufficient)
```

The script clones/updates both tcd and urfd repos (tcd has symlinks to urfd source files), builds and installs the binary, and restarts the service.

Edit `/usr/local/etc/tcd.ini` after install (see Configuration below).

### Manual build

```bash
git clone https://github.com/jcmerg/tcd.git
git clone https://github.com/jcmerg/urfd.git
cd tcd
cp config/* .
```

Edit configuration:
- `tcd.ini` — set ServerAddress, Modules, DeviceSerial, gain values
- `tcd.mk` — set `md380 = true` to enable the MD380 software vocoder
- `tcd.service` — already configured for `/usr/local/bin/tcd`

Build and install:

```bash
make -j$(nproc)           # without md380 (default)
make -j$(nproc) md380=true  # with md380 software vocoder
sudo make install
```

## Configuration (tcd.ini)

```ini
Port = 10100
ServerAddress = 172.16.200.10   # IP of the urfd reflector
Modules = FS                    # must match urfd.ini [Transcoder] section

# Whitelist AMBE devices by serial (use 'tcd --list-devices' to find them)
DeviceSerial = DQ015SBR
DeviceSerial = DKB7FXGE

# Output Gain — post-AGC, per target codec, in dB
# AMBE2+ (DMR/YSF) is inherently ~15-20 dB louder than AMBE (D-Star) at the
# same PCM input level. This is a known codec characteristic confirmed by
# xlxd/ambed (20 dB offset) and DVSwitch (~15 dB).
#
# How OutputGainDMR is applied depends on the build and config:
#   - With md380 + DMRReEncode=true: gain applied on PCM before re-encode.
#     Typical value: -16 (matches the ~15-20 dB codec difference).
#   - Without md380 or DMRReEncode=false: gain applied via AMBE2+ bitstream
#     b2 parameter manipulation (no decode/re-encode needed).
#     Typical value: 0 (b2 adjustment is more sensitive, -16 would be too much).
OutputGainDStar = 0             # D-Star output
OutputGainM17   = 0             # M17/Codec2 output
OutputGainDMR   = 0             # DMR/YSF output (see note above; -16 with md380 re-encode)
OutputGainIMBE  = 0             # P25/NXDN output
OutputGainUSRP  = 0             # USRP output

# DVSI Hardware Gains in dB (-40 to +40) — inside the DVSI chip
DStarGainIn     = 0
DStarGainOut    = 0
DmrGainIn       = 0
DmrGainOut      = 0

# USRP/SVX PCM gains in dB (-40 to +40)
UsrpGainIn      = 0
UsrpGainOut     = 0

# AMBE2+ bitstream gain — adjusts b2 (delta-gamma) in AMBE2+ frames directly.
# No decode/re-encode needed, also repairs FEC errors in the A partition.
# Active when DMR Re-encode is not handling the gain (no md380, or DMRReEncode=false).
AmbeGain        = true          # enable bitstream gain for DMR/YSF passthrough
AmbeGainDb      = -2            # -30 to 0 dB (each 2 dB ≈ 1 b2 step)

# Software vocoder (only effective with md380=true build)
DmrReencodeGain = 0             # additional gain for MD380 DMR re-encode only
DMRReEncode     = false         # true = re-encode DMR via MD380 after AGC (requires md380=true build)

# AGC (Automatic Gain Control)
AGC             = true
AGCTarget       = -16
AGCAttack       = 50
AGCRelease      = 500
AGCMaxGainUp    = 30
AGCMaxGainDown  = 24
AGCNoiseGate    = -55

# Web Dashboard & Monitoring
Monitor          = true
MonitorHttpPort  = 8080
MonitorStatsPort = 8081

# Stats CSV Logging (enable for tuning, disable in normal operation)
StatsLog       = false
StatsLogDir    = /tmp/tcd-stats
StatsLogRetain = 24
```

### Audio gain

All transcoding passes through PCM as intermediate format:

```
Source Codec --[DVSI GainIn]--> PCM --[AGC]--> normalized PCM --[OutputGain]--> Target Codec
```

#### Output Gain (post-AGC)

Applied after AGC, independently per target codec. AMBE2+ (DMR/YSF) is inherently ~15-20 dB louder than AMBE (D-Star) at the same PCM input level — this is a known codec characteristic also addressed by xlxd/ambed (20 dB offset) and DVSwitch (~15 dB).

| Parameter | Default | Applied to |
|-----------|---------|------------|
| `OutputGainDStar` | `0` | D-Star DVSI encode |
| `OutputGainM17` | `0` | M17/Codec2 encode |
| `OutputGainDMR` | `0` | DMR/YSF (see below) |
| `OutputGainIMBE` | `0` | P25/NXDN (IMBE encoder) |
| `OutputGainUSRP` | `0` | USRP output |

Gains are applied on local copies in each encode function and in the DVSI FeedDevice thread — the shared PCM buffer is never modified.

**OutputGainDMR** has two operating modes depending on the build:

| Mode | When | How gain is applied | Recommended value |
|------|------|--------------------|--------------------|
| **PCM gain** | `md380=true` + `DMRReEncode=true` | Applied to PCM before md380 re-encode. Also provides AGC normalization for DMR→DMR/YSF. | `-16` |
| **Bitstream gain** | No md380, or `DMRReEncode=false` | Adjusts the b2 (delta-gamma) parameter directly in the AMBE2+ frame. No decode/re-encode, no quality loss, but no AGC on DMR→DMR/YSF. | `0` (b2 is more sensitive; -16 would over-attenuate) |

With the **PCM gain** path (md380 re-encode), `-16` compensates for the full ~15-20 dB codec loudness difference. With the **bitstream gain** path, the b2 parameter operates in the vocoder's internal log-domain with DPCM amplification (~2 dB per step), so `0` already provides appropriate attenuation at the default conversion factor. Adjust in small steps (±2-4 dB) if needed.

#### DVSI Hardware Gain

Gain values (in dB, range -40 to +40) applied inside the DVSI chip:

| Parameter | Direction | Description |
|-----------|-----------|-------------|
| `DStarGainIn` | D-Star AMBE → PCM | Boost quiet D-Star audio |
| `DStarGainOut` | PCM → D-Star AMBE | Reduce PCM for D-Star encoding |
| `DmrGainIn` | DMR AMBE2+ → PCM | Attenuate hot DMR audio |
| `DmrGainOut` | PCM → DMR AMBE2+ | Attenuate PCM for DMR encoding |
| `UsrpGainIn` | PCM from USRP → internal | Adjust incoming USRP audio |
| `UsrpGainOut` | Internal → USRP | Adjust outgoing USRP audio |

**Note**: SVX gain is configured in urfd.ini (`RxGain`/`TxGain`), not in tcd.

#### AGC (Automatic Gain Control)

The AGC normalizes audio levels after decode and before encode. It tracks gain per stream (per transmission), so each user gets independent level adjustment.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AGC` | `false` | Enable/disable AGC (`true` or `false`) |
| `AGCTarget` | `-16` | Target RMS level in dBFS. Lower = quieter output. |
| `AGCAttack` | `50` | Attack time in ms. How fast loud signals are dampened. |
| `AGCRelease` | `500` | Release time in ms. How fast quiet signals are raised. |
| `AGCMaxGainUp` | `20` | Maximum amplification in dB. Limits noise boost on quiet input. |
| `AGCMaxGainDown` | `24` | Maximum attenuation in dB. Limits reduction on loud input. |
| `AGCNoiseGate` | `-55` | Noise gate threshold in dBFS. Below this level, gain is frozen. |

#### DMR Re-encode (requires `md380=true` build)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `DMRReEncode` | `false` | Re-encode DMR/YSF output via MD380 after AGC. Without re-encode, DMR→DMR listeners receive un-normalized audio (AGC still applies to cross-mode paths like DMR→D-Star). Ignored with a warning if md380 is not compiled in. |
| `DmrReencodeGain` | `0` | Additional gain (dB) applied before MD380 re-encode. Normally 0 — use `OutputGainDMR` instead. |

#### AMBE2+ Bitstream Gain (experimental)

Adjusts the b2 (delta-gamma) gain parameter directly in AMBE2+ frames without decode/re-encode. Active when DMR Re-encode is not handling the gain. Also repairs Golay FEC errors in the A partition as a side effect.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AmbeGain` | `true` | Enable bitstream gain for DMR/YSF passthrough |
| `AmbeGainDb` | `-2` | Gain in dB (-30 to 0). Each 2 dB ≈ 1 b2 step. Even -2 dB (1 step) repairs FEC errors and reduces choppy audio. |

AmbeGain and DMR Re-encode are mutually exclusive at runtime — if Re-encode handled the gain via PCM, AmbeGain is skipped. Both can be configured simultaneously; Re-encode takes precedence when active.

Gain limits are asymmetric by design: attenuation (down) is safe, amplification (up) risks noise. Typical DMR input sits at -35 dBFS, so +20 dB up is needed to reach -16 target.

**AGC algorithm details:**
- **Sliding RMS window** (3 frames / 60ms): smooths consonant/vowel variation, tracks syllable-level energy
- **Per-stream long-term average gain**: tracks the typical gain needed for each speaker via slow EMA (~2s)
- **Gate with gain decay**: during silence, gain drifts toward the speaker's long-term average gain, so the first syllable after a pause starts at the right level
- **Fast post-gate release**: first 5 frames after gate opening use 5x release speed (capped at alpha=0.5) for quicker recovery
- **Peak limiter**: hard limit at -0.1 dBFS, never clips

**Without AGC**, the static gain values must compensate for all level differences between codecs and users. Tuning is tedious and every route needs individual attention.

**With AGC**, static gains only do coarse matching (D-Star is inherently quieter, so a small boost remains). The AGC automatically adjusts for different microphone levels, radio models, and codec characteristics.

All AGC and gain parameters can be changed at runtime via the web dashboard (no restart required).

## Monitoring

### Web Dashboard

Access `http://<tcd-host>:8080` in a browser. Features:

- **Signal flow diagram**: Shows the active codec path with live dBFS levels at each stage
- **VU meters**: Pre-AGC and post-AGC with peak hold
- **AGC controls**: Enable/disable, target, attack, release, gain limits (up to 40dB) — changes apply live
- **Gain sliders**: Grouped into Output Gain (post-AGC) / DVSI Hardware / USRP-SVX / Software Vocoder
- **DVSI device status**: Serial, type, role (dstar/dmr/mixed), vocoder slots used/total, active module letters, buffer depth
- **MD380 software vocoder**: Re-encode on/off, cached streams, encode/decode/re-encode counters, active module
- **Reflector status**: Connected/disconnected, packet counters
- **Save to INI**: Persist current settings to tcd.ini
- **Mobile responsive**: Signal flow scrollable, config labels compact, grid collapses on narrow screens

### ncurses Terminal Monitor (tcdmon)

For SSH access to the transcoder host:

```bash
tcdmon                      # connect to localhost:8081
tcdmon 172.16.20.20 8081    # connect to remote host
```

Shows VU bars, signal flow, AGC state, DVSI device slots with active modules, MD380 vocoder status with counters, output gain summary. Press `q` to quit.

### Stats CSV Logging

When `StatsLog = true`, tcd writes a CSV file per stream to `StatsLogDir`:

```
F_20260402_003512_43521.csv
```

Format: `ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate`

Files older than `StatsLogRetain` hours are automatically deleted. Useful for post-hoc AGC analysis with gnuplot, Excel, or Python.

### agc-analyze

Installed as `/usr/local/bin/agc-analyze` by `build.sh`. Analyzes stats CSV files for AGC tuning:

```bash
agc-analyze                            # detail view, default dir /tmp/tcd-stats
agc-analyze -s                         # summary table (one line per stream)
agc-analyze -s -c dmr                  # filter by codec (dstar, dmr, p25, usrp, svx)
agc-analyze -s -m F                    # filter by module
agc-analyze file.csv                   # single file detail
```

Shows per-stream metrics: duration, speech/gated frame ratio, input/output RMS, AGC gain range, clipping percentage. Summary mode (`-s`) is useful for comparing gain settings across many transmissions.

## DV3003 Notes

The DV3003 (AMBE-3003F) requires special handling compared to the DV3000:

- **PKT_COMPAND=0**: All DV3003 channels need companding explicitly disabled. The DV3003 defaults to companding ON (CP_ENABLE pullup), while the DV3000 defaults to OFF (board wiring). Without this fix, DMR/YSF channels receive mu-law PCM interpreted as linear, causing audio distortion
- **350-byte serial flush**: Before soft reset, 350 zero bytes are sent to clear any stale serial state
- **Mixed-mode**: A single DV3003 can have channels configured for different codecs (D-Star + DMR) simultaneously

## Managing tcd

```bash
sudo systemctl start tcd
sudo systemctl stop tcd
sudo systemctl status tcd
sudo journalctl -u tcd -f          # follow logs
```

## Changes from upstream

### Hardware & Device Support
- **Mixed-mode DV3000+DV3003**: Concurrent 2-module transcoding with per-channel codec configuration
- **DV3003 D-Star support**: PKT_COMPAND fix, 350-byte flush, per-channel encoding
- **FTDI robustness**: Auto-unbind ftdi_sio at startup, device whitelist by serial number, buffer flush on overflow
- **SVX codec path**: Separate `ECodecType::svx` for independent SVX audio handling

### Audio Processing
- **Per-codec output gains**: Independent post-AGC gains for D-Star, M17/Codec2, DMR/YSF, P25/NXDN, USRP — applied in encode functions and DVSI FeedDevice (shared buffer never modified)
- **AMBE2+ bitstream gain**: When md380 re-encode is not active, `OutputGainDMR` adjusts the b2 (delta-gamma) parameter directly in the AMBE2+ frame — no vocoder decode/re-encode needed, no quality loss. Uses Golay(24,12) FEC recomputation for the A partition.
- **AGC v3**: Sliding RMS window (60ms), per-stream speaker tracking, gate-with-decay to speaker average, fast post-gate release (5x/5 frames), asymmetric limits (up to 40dB), noise gate with hysteresis, peak limiter, live reconfiguration from dashboard
- **DMR re-encode** (requires `md380=true`): Active when AGC or OutputGainDMR is set — ensures DMR→DMR passthrough is also normalized. Can be disabled with `DMRReEncode = false` to preserve original AMBE quality
- **MD380 stream isolation**: Save/restore encoder state per stream, mutex around all MD380 calls

### Monitoring & Configuration
- **Web dashboard**: Embedded mongoose HTTP+WS with signal flow, VU meters, grouped gain sliders (Output/DVSI/USRP/Software), AGC controls, DVSI slot tracking, MD380 vocoder status, save-to-INI, mobile responsive
- **ncurses monitor** (`tcdmon`): SSH-friendly terminal UI with device slots, MD380 counters, active modules, output gain summary
- **Stats CSV logging**: Per-stream per-codec AGC/level recording with auto-cleanup, `agc-analyze` CLI tool for post-hoc analysis
- **Config key renames**: `DmrGainIn/Out` (was DmrYsfGainIn/Out), `UsrpGainIn/Out` (was UsrpRxGain/TxGain), gain ranges extended to ±40 dB

### Reliability
- **Optional MD380**: Build with `md380=true` / `--with-md380` to enable; without it, ≥2 AMBE devices are required and DMR re-encode is unavailable
- **Performance**: FTDI event notification instead of busy-poll, condition variables, ~5% CPU on Pi
- **Thread safety**: Mutex around IMBE vocoder, SIGPIPE ignored for clean reconnection
- **Error handling**: SendToReflector retry with backoff, graceful queue overflow, ReadDevice timeout recovery
- **Reconnect backoff**: Exponential backoff (0.5s → 5s max) when reflector is unreachable, prevents log spam and CPU waste
- **Usability**: `--list-devices` CLI, `DeviceSerial` config, unified `build.sh`

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- Copyright (c) 2026 Jens-Christian Merg DL4JC (fork improvements)
