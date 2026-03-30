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
ServerAddress = 172.16.200.10    # IP of the urfd reflector
Modules = FS                      # must match urfd.ini [Transcoder] section

# Whitelist AMBE devices by serial (use 'tcd --list-devices' to find them)
# For mixed mode, list both serials. Order does not matter — tcd auto-detects types.
DeviceSerial = DQ015SBR
DeviceSerial = DKB7FXGE

# Static gain values in dB (-24 to +24)
# With AGC enabled, these only need coarse codec-level matching.
DStarGainIn   =  0
DStarGainOut  =  0
DmrYsfGainIn  =  0
DmrYsfGainOut =  0
UsrpTxGain    =  0
UsrpRxGain    =  0

# AGC (Automatic Gain Control)
AGC           = true
AGCTarget     = -16
AGCAttack     = 50
AGCRelease    = 500
AGCMaxGain    = 12
```

### Audio gain

All transcoding passes through PCM as intermediate format:

```
Source Codec --[GainIn]--> PCM --[AGC]--> normalized PCM --[GainOut]--> Target Codec
```

DMR input is always re-encoded from AGC-normalized PCM via the md380 software vocoder, ensuring correct audio levels on DMR/YSF output regardless of input source volume.

#### Static gain

Gain values (in dB, range -24 to +24) are applied at decode and encode stages:

| Parameter | Stage | Direction | Description |
|-----------|-------|-----------|-------------|
| `DStarGainIn` | Decode | D-Star AMBE → PCM | Boost quiet D-Star audio to normal PCM level |
| `DStarGainOut` | Encode | PCM → D-Star AMBE | Reduce PCM back to D-Star level |
| `DmrYsfGainIn` | Decode | DMR/YSF AMBE2+ → PCM | Attenuate hot DMR/YSF audio |
| `DmrYsfGainOut` | Encode | PCM → DMR/YSF AMBE2+ | Attenuate PCM for DMR/YSF encoding |
| `UsrpRxGain` | Receive | PCM from USRP → internal PCM | Attenuate incoming USRP audio |
| `UsrpTxGain` | Transmit | Internal PCM → USRP | Adjust PCM level sent to USRP |

**Note**: SvxReflector audio uses a separate codec path (`ECodecType::svx`) and is **not** affected by UsrpRxGain/UsrpTxGain. SVX gain is configured in urfd.ini (see urfd documentation).

#### AGC (Automatic Gain Control)

The AGC normalizes audio levels after decode and before encode. It tracks gain per stream (per transmission), so each user gets independent level adjustment.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `AGC` | `false` | Enable/disable AGC (`true` or `false`) |
| `AGCTarget` | `-16` | Target RMS level in dBFS. Lower = quieter output. |
| `AGCAttack` | `50` | Attack time in ms. How fast loud signals are dampened. |
| `AGCRelease` | `500` | Release time in ms. How fast quiet signals are raised. |
| `AGCMaxGain` | `12` | Maximum gain boost in dB. Limits noise amplification in quiet passages. |

**Without AGC**, the static gain values must compensate for all level differences between codecs and users. Tuning is tedious and every route needs individual attention.

**With AGC**, static gains only do coarse matching (D-Star is inherently quieter, so a small boost remains). The AGC automatically adjusts for different microphone levels, radio models, and codec characteristics.

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
- **md380 always linked**: Runtime device detection instead of compile-time flags. DMR re-encode after AGC for correct output levels.
- **Thread safety**: Mutex around IMBE vocoder calls (race between C2 and IMBE threads)
- **Error handling**: Retry limit with backoff in SendToReflector (was infinite loop), graceful queue overflow handling (was raise(SIGINT)), timeout recovery in ReadDevice
- **FTDI robustness**: Auto-unbind ftdi_sio at startup, device whitelist by serial number, FTDI buffer flush on overflow
- **Usability**: `--list-devices` CLI option, `DeviceSerial` config, improved error messages

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- Copyright (c) 2026 Jens-Christian Merg DL4JC (fork improvements)
