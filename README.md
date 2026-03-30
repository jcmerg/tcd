# tcd — Hybrid Digital Voice Transcoder

Fork of [n7tae/tcd](https://github.com/n7tae/tcd) with bug fixes and usability improvements.

## Overview

tcd is a hybrid transcoder for the [URF reflector](https://github.com/jcmerg/urfd). It bridges between hardware-based vocoders (DVSI AMBE) and software-based vocoders (Codec2 for M17, IMBE for P25, md380 for DMR/YSF).

## Requirements

- Linux (systemd-based, Debian/Ubuntu recommended)
- DVSI AMBE hardware: USB-3000 (1 channel) or USB-3003 (3 channels)
- [imbe_vocoder](https://github.com/jcmerg/imbe_vocoder) library
- [libftd2xx](https://ftdichip.com/drivers/d2xx-drivers/) FTDI driver
- Optional: [md380_vocoder](https://github.com/jcmerg/md380_vocoder) for software DMR/YSF vocoding (ARM native)

With md380_vocoder enabled, only **one** DVSI device is needed — D-Star uses the hardware vocoder, DMR/YSF runs in software.

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
sudo bash build.sh --swambe2    # with md380 software vocoder
# or: sudo bash build.sh        # without (needs two DVSI devices)
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
- `tcd.mk` — enable md380 software vocoder if desired (`swambe2 = true`)
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
Modules = S                       # must match urfd.ini [Transcoder] section

# Whitelist AMBE device by serial (use 'tcd --list-devices' to find it)
DeviceSerial = DQ015SBR

# Static gain values in dB (-24 to +24)
# With AGC enabled, these only need coarse codec-level matching.
DStarGainIn   =  8
DStarGainOut  = -8
DmrYsfGainIn  =  0
DmrYsfGainOut =  0
UsrpTxGain    =  0
UsrpRxGain    = -8

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

The net gain for a transcoding path is the sum of the source GainIn and the target GainOut. With AGC enabled, the static gains only need to do coarse codec-level matching — the AGC handles user-to-user variation.

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

## Managing tcd

```bash
sudo systemctl start tcd
sudo systemctl stop tcd
sudo systemctl status tcd
sudo journalctl -u tcd -f          # follow logs
```

## Changes from upstream

- **Thread safety**: Mutex around IMBE vocoder calls (race between C2 and IMBE threads)
- **Error handling**: Retry limit with backoff in SendToReflector (was infinite loop), graceful queue overflow handling (was raise(SIGINT))
- **FTDI robustness**: Auto-unbind ftdi_sio at startup, device whitelist by serial number, FTDI buffer flush on overflow
- **Usability**: `--list-devices` CLI option, `DeviceSerial` config, improved error messages
- **Misc**: Typo fix, version bump to 0.1.1

## Copyright

- Copyright (c) 2021-2023 Thomas A. Early N7TAE, Doug McLain AD8DP
- Copyright (c) 2026 Jens-Christian Merg DL4JC (fork improvements)
