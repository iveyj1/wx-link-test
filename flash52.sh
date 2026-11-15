#!/usr/bin/env bash
# Flash the nRF52840 node: ./flash52.sh [build/node52/node52.ino.zip]
# Asks the running firmware to reset into the bootloader ("dfu" command, probe builds and later);
# if no bootloader port appears, double-tap RST to GND and it continues within 5 minutes.
set -euo pipefail
zip=${1:-build/node52/node52.ino.zip}
app=$(ls /dev/serial/by-id/ 2>/dev/null | grep -i nRF52840_node || true)
if [ -n "$app" ]; then
  python3 -c "import serial,time; s=serial.Serial('/dev/serial/by-id/$app',115200); time.sleep(0.3); s.write(b'dfu\n'); time.sleep(0.3)" || true
fi
echo "waiting for the bootloader (double-tap RST to GND if nothing happens)..."
timeout 300 bash -c 'until ls /dev/serial/by-id/ 2>/dev/null | grep -qi nice_nano; do sleep 0.3; done'
sleep 0.5
adafruit-nrfutil dfu serial -pkg "$zip" -p "$(readlink -f /dev/serial/by-id/*nice_nano*)" -b 115200 --singlebank
timeout 20 bash -c 'until ls /dev/serial/by-id/ 2>/dev/null | grep -qi nRF52840_node; do sleep 0.3; done' && echo "running: $(ls /dev/serial/by-id/ | grep -i nRF52840_node)"
