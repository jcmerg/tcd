#!/bin/bash
set -e

CONFIG="${TCD_CONFIG:-/etc/tcd/tcd.ini}"

if [ ! -f "$CONFIG" ]; then
    echo "ERROR: Config file not found: $CONFIG"
    exit 1
fi

# Unbind AMBE DVSI devices from ftdi_sio without unloading the module.
# This allows other FTDI devices (USB serial converters) to keep working.
# Common FTDI product IDs for DVSI devices:
#   0403:6001 - ThumbDV, DVstick-30, USB-3000 (AMBE3000)
#   0403:6010 - USB-3003, USB-3012 (AMBE3003)
#   0403:6015 - FT-X series (some AMBE3000 variants)
unbind_ambe_devices() {
    local found=0
    for devpath in /sys/bus/usb/devices/*/; do
        [ -f "$devpath/idVendor" ] || continue
        local vid=$(cat "$devpath/idVendor" 2>/dev/null)
        local pid=$(cat "$devpath/idProduct" 2>/dev/null)
        local product=$(cat "$devpath/product" 2>/dev/null || echo "")
        local serial=$(cat "$devpath/serial" 2>/dev/null || echo "")

        # Only target FTDI devices (vendor 0403)
        [ "$vid" = "0403" ] || continue

        # Check if this looks like a DVSI/AMBE device by product string
        local is_ambe=0
        case "$product" in
            *ThumbDV*|*DVstick*|*USB-300*|*USB-301*|*AMBE*|*DF2ET*)
                is_ambe=1
                ;;
        esac

        # Also match known PIDs for dual-channel FTDI (USB-3003/3012)
        if [ "$pid" = "6010" ]; then
            is_ambe=1
        fi

        [ "$is_ambe" = "1" ] || continue

        echo "Found AMBE device: $product (${vid}:${pid}) serial=$serial at $(basename $devpath)"

        # Unbind all interfaces of this device from ftdi_sio
        for iface in "$devpath"/*/driver; do
            local driver=$(readlink -f "$iface" 2>/dev/null || true)
            if echo "$driver" | grep -q "ftdi_sio"; then
                local ifname=$(basename $(dirname "$iface"))
                echo "  Unbinding $ifname from ftdi_sio..."
                echo "$ifname" > /sys/bus/usb/drivers/ftdi_sio/unbind 2>/dev/null && \
                    echo "  OK: $ifname unbound" || \
                    echo "  WARN: Could not unbind $ifname (may already be unbound)"
                found=$((found + 1))
            fi
        done
    done

    if [ "$found" -eq 0 ]; then
        echo "WARN: No AMBE devices found bound to ftdi_sio (may already be unbound or not plugged in)"
    fi
}

echo "Checking for AMBE USB devices..."
unbind_ambe_devices
echo "---"

echo "Starting tcd with config: $CONFIG"
echo "---"
cat "$CONFIG"
echo "---"

exec /usr/local/bin/tcd "$CONFIG"
