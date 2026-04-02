# tcd — Hybrid Digital Voice Transcoder

Fork of [n7tae/tcd](https://github.com/n7tae/tcd) with bug fixes and usability improvements.

## Overview

tcd is a hybrid transcoder for the [URF reflector](https://github.com/jcmerg/urfd). It bridges between hardware-based vocoders (DVSI AMBE) and software-based vocoders (Codec2 for M17, IMBE for P25, md380 for DMR/YSF).

## Requirements

- Linux (systemd-based, Debian/Ubuntu recommended)
- DVSI AMBE hardware: USB-3000 / ThumbDV / DVstick-30 (1 channel) or USB-3003 / DVstick-33 (3 channels)
- [imbe_vocoder](https://github.com/jcmerg/imbe_vocoder) library
- [libftd2xx](https://ftdichip.com/drivers/d2xx-drivers/) FTDI driver
- [md380_vocoder](https://github.com/jcmerg/md380_vocoder) — always required (ARM native or x86_64 via dynarmic)

## Device Configurations

tcd auto-detects the connected devices and selects the appropriate mode:

### 1. Mixed Mode: DV3000 + DV3003 (recommended, 2 modules)

Use a DV3000 (1 D-Star channel) together with a DV3003 (1 D-Star + 2 DMR channels) for concurrent 2-module cross-mode transcoding. DMR audio levels are normalized via md380 software re-encode after AGC.

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

### 2. Single Device + md380 (1 module)

One DVSI device for D-Star, md380 software vocoder for DMR/YSF. Only supports 1 transcoded module. Works on ARM and x86_64.

```ini
Modules = S
DeviceSerial = DQ015SBR
```

### 3. Two Same-Type Devices (1-3 modules)

Two DV3000s (1 module) or two DV3003s (up to 3 modules). One device handles D-Star, the other DMR/YSF.

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
sudo bash build.sh
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
- `tcd.service` — already configured for `/usr/local/bin/tcd`

Build and install:

```bash
make -j$(nproc)
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

# Output Gain — post-AGC, per target codec, in dB (-40 to +40)
OutputGainDStar = 0             # D-Star + Codec2/M17 output
OutputGainDMR   = -16           # DMR/YSF + IMBE/P25 output
OutputGainUSRP  = 0             # USRP output

# DVSI Hardware Gains in dB (-40 to +40) — inside the DVSI chip
DStarGainIn     = 0
DStarGainOut    = 0
DmrGainIn       = 0
DmrGainOut      = 0

# USRP/SVX PCM gains in dB (-40 to +40)
UsrpGainIn      = 0
UsrpGainOut     = 0
DmrReencodeGain = 0             # legacy, use OutputGainDMR instead

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

Applied after AGC, independently per target codec. Use to balance level differences (e.g. DMR/YSF output is often too loud relative to D-Star).

| Parameter | Default | Applied to |
|-----------|---------|------------|
| `OutputGainDStar` | `0` | D-Star DVSI encode, Codec2/M17 |
| `OutputGainDMR` | `0` | DMR/YSF DVSI encode, md380 sw encode, IMBE/P25 |
| `OutputGainUSRP` | `0` | USRP output |

Gains are applied on local copies in each encode function and in the DVSI FeedDevice thread — the shared PCM buffer is never modified.

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

Note: `AGCMaxGain` (symmetric, old style) is still accepted for backwards compatibility — it sets both Up and Down to the same value.

Gain limits are asymmetric by design: attenuation (down) is safe, amplification (up) risks noise. Typical DMR input sits at -35 dBFS, so +20 dB up is needed to reach -16 target.

**AGC algorithm details:**
- **Sliding RMS window** (3 frames / 60ms): smooths consonant/vowel variation, tracks syllable-level energy
- **Per-stream long-term average gain**: tracks the typical gain needed for each speaker via slow EMA (~2s)
- **Gate with gain decay**: during silence, gain drifts toward the speaker's average gain (not unity), so the first syllable after a pause starts at the right level
- **Fast post-gate release**: first 5 frames after gate opening use 5x release speed for quicker recovery
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
- **Device status**: DVSI serial, type, role (dstar/dmr/mixed), vocoder slots used/total, active module letters, buffer depth
- **Reflector status**: Connected/disconnected, packet counters
- **Save to INI**: Persist current settings to tcd.ini

### ncurses Terminal Monitor (tcdmon)

For SSH access to the transcoder host:

```bash
tcdmon                      # connect to localhost:8081
tcdmon 172.16.20.20 8081    # connect to remote host
```

Shows VU bars, signal flow, AGC state, device status with slot usage and active modules, output gain summary. Press `q` to quit.

### Stats CSV Logging

When `StatsLog = true`, tcd writes a CSV file per stream to `StatsLogDir`:

```
F_20260402_003512_43521.csv
```

Format: `ms,module,codec,rms_in,peak_in,rms_out,peak_out,agc_gain,gate`

Files older than `StatsLogRetain` hours are automatically deleted. Useful for post-hoc AGC analysis with gnuplot, Excel, or Python.

## DV3003 Notes

The DV3003 (AMBE-3003F) requires special handling compared to the DV3000:

- **PKT_COMPAND=0**: D-Star channels on the DV3003 need companding explicitly disabled (the DV3000 defaults to companding off)
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

- **Mixed-mode DV3000+DV3003**: Concurrent 2-module transcoding with per-channel codec configuration
- **DV3003 D-Star support**: PKT_COMPAND fix, 350-byte flush, per-channel encoding
- **md380 always linked**: Runtime device detection instead of compile-time flags. DMR re-encode after AGC via `DmrReencodeGain` for correct output levels.
- **Performance**: FTDI event notification (`FT_SetEventNotification`) instead of busy-poll, condition variables for FeedDevice, cached DV3003 pointer. ~5% CPU on Raspberry Pi 3.
- **Per-codec output gains**: Independent post-AGC gain for D-Star, DMR/YSF, USRP outputs. Applied in encode functions and DVSI FeedDevice (never modifying shared packet buffer). Replaces the universal `DmrReencodeGain`.
- **AGC improvements**: Sliding RMS window (60ms), gate-with-decay (gain drifts to speaker avg during silence), fast post-gate release (5x speed for first 5 frames after pause), asymmetric gain limits (up/down, range to 40dB), configurable noise gate with hysteresis, peak limiter, per-stream tracking, live reconfiguration from web dashboard
- **Web dashboard**: Embedded mongoose HTTP+WebSocket server with signal flow visualization, VU meters, grouped gain sliders (output/DVSI/USRP/software), AGC controls, DVSI device status with vocoder slot tracking, save-to-INI
- **ncurses monitor**: `tcdmon` standalone SSH-friendly terminal tool with device slots, active modules, output gain summary
- **Stats CSV logging**: Per-stream AGC/level recording for post-hoc analysis with auto-cleanup
- **SVX codec path**: Separate `ECodecType::svx` for independent SVX audio handling, routed through all codec stages without touching USRP gain
- **md380 stream isolation**: Save/restore encoder state per stream and mutex around all md380 calls to prevent cross-stream crosstalk in multi-module setups
- **Thread safety**: Mutex around IMBE vocoder calls (race between C2 and IMBE threads), SIGPIPE ignored for clean reconnection after reflector restart
- **Error handling**: Retry limit with backoff in SendToReflector (was infinite loop), graceful queue overflow handling (was raise(SIGINT)), timeout recovery in ReadDevice
- **FTDI robustness**: Auto-unbind ftdi_sio at startup, device whitelist by serial number, FTDI buffer flush on overflow
- **Usability**: `--list-devices` CLI option, `DeviceSerial` config, improved error messages, unified `build.sh` with auto-dependency install

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- Copyright (c) 2026 Jens-Christian Merg DL4JC (fork improvements)
