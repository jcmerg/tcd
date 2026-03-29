# This is where the executable will be installed `make install`
BINDIR = /usr/local/bin

# set to true to build a transcoder that supports gdb
debug = false

# set to true to use the md-380 software vocoder for DMR/YSF
# requires libmd380_vocoder installed (ARM native or x86_64 via dynarmic)
# with this enabled, only one DVSI device is needed (for D-Star)
swambe2 = false
