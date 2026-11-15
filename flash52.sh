#!/usr/bin/env bash
# Flash the nRF52840 node: ./flash52.sh [build/node52/node52.ino.zip]
# Software resets into the bootloader do not work with this board's bootloader (0.6.0, nice!nano build):
# the 1200-baud touch and GPREGRET 0x4E/0x57 resets all restart the app. Double-tap RST to GND.
# if no bootloader port appears, double-tap RST to GND and it continues within 5 minutes.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"   # adafruit-nrfutil (pipx)
zip=${1:-build/node52/node52.ino.zip}
echo "waiting for the bootloader (double-tap RST to GND now)..."
timeout 300 bash -c 'until ls /dev/serial/by-id/ 2>/dev/null | grep -qi nice_nano; do sleep 0.3; done'
sleep 0.5
adafruit-nrfutil dfu serial -pkg "$zip" -p "$(readlink -f /dev/serial/by-id/*nice_nano*)" -b 115200 --singlebank
timeout 20 bash -c 'until ls /dev/serial/by-id/ 2>/dev/null | grep -qi nRF52840_node; do sleep 0.3; done' && echo "running: $(ls /dev/serial/by-id/ | grep -i nRF52840_node)"
