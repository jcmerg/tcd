# This is where the executable will be installed `make install`
BINDIR = /usr/local/bin

# set to true to build a debug binary (includes gdb symbols)
debug = false

# MD380 software vocoder for DMR/YSF (default: false)
# Set to true to enable the md380_vocoder library.
# Requires libmd380_vocoder installed (ARM native or x86_64/aarch64 via dynarmic).
#
# With md380=true:
#   - A single DVSI device (DV3000 or DV3003) is sufficient; MD380 handles DMR/YSF
#   - DMR re-encode after AGC is available (set DMRReEncode = true in tcd.ini)
#
# Without md380=true (default):
#   - Minimum hardware: 2x AMBE3000 (DV3000), 1x AMBE3003 (DV3003), or DV3000+DV3003
#   - DMRReEncode is ignored (re-encode requires md380)
md380 = false
