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

# Gain values in dB (-24 to +24)
DStarGainIn   =  16
DStarGainOut  = -16
DmrYsfGainIn  =  -6
DmrYsfGainOut =  -6
UsrpTxGain    = -8
UsrpRxGain    = -16
```

### Audio gain

All transcoding passes through PCM as intermediate format. Gain values (in dB) are applied at the decode and encode stages:

```
Source Codec --[GainIn]--> PCM --[GainOut]--> Target Codec
```

| Parameter | Stage | Direction | Description |
|-----------|-------|-----------|-------------|
| `DStarGainIn` | Decode | D-Star AMBE → PCM | Boost quiet D-Star audio to normal PCM level |
| `DStarGainOut` | Encode | PCM → D-Star AMBE | Reduce PCM back to D-Star level |
| `DmrYsfGainIn` | Decode | DMR/YSF AMBE2+ → PCM | Attenuate hot DMR/YSF audio |
| `DmrYsfGainOut` | Encode | PCM → DMR/YSF AMBE2+ | Attenuate PCM for DMR/YSF encoding |
| `UsrpRxGain` | Receive | PCM from USRP/SvxLink → internal PCM | Attenuate incoming PCM sources |
| `UsrpTxGain` | Transmit | Internal PCM → USRP/SvxLink | Adjust PCM level sent to USRP |

The net gain for a transcoding path is the sum of the source GainIn and the target GainOut:

| Route | Gain path | Net |
|-------|-----------|-----|
| D-Star → DMR/YSF | DStarGainIn + DmrYsfGainOut | +10 dB |
| DMR/YSF → D-Star | DmrYsfGainIn + DStarGainOut | -22 dB |
| D-Star → D-Star | DStarGainIn + DStarGainOut | 0 dB |
| DMR/YSF → DMR/YSF | DmrYsfGainIn + DmrYsfGainOut | -12 dB |
| SvxLink → D-Star | UsrpRxGain + DStarGainOut | -32 dB |
| SvxLink → DMR/YSF | UsrpRxGain + DmrYsfGainOut | -22 dB |
| D-Star → SvxLink | DStarGainIn + UsrpTxGain | +8 dB |
| DMR/YSF → SvxLink | DmrYsfGainIn + UsrpTxGain | -14 dB |

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
