#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export PATH="$HOME/.local/bin:$PATH"   # adafruit-nrfutil (pipx), used to package nRF52 builds
CLI=${ARDUINO_CLI:-/home/user/pnp/Arduino/arduino-cl/bin/arduino-cli}
for role in sensor receiver; do
  flag=1; [[ $role == receiver ]] && flag=0
  "$CLI" compile --fqbn adafruit:samd:adafruit_feather_m0 \
    --build-property "compiler.cpp.extra_flags=-DSENSOR=$flag" \
    --build-property "compiler.c.elf.extra_flags=-Wl,-T,$PWD/retained.ld" \
    --output-dir "build/$role" link
done
# nRF52840 node (Adafruit nRF52 core 1.7.0 + the board definition in hardware/wx/nrf52).
ln -sfn "$HOME/.arduino15/packages/adafruit/hardware/nrf52/1.7.0/libraries" hardware/wx/nrf52/libraries
ARDUINO_DIRECTORIES_USER=$PWD "$CLI" compile --fqbn wx:nrf52:promicro \
  --build-property "compiler.cpp.extra_flags=-I$PWD/link" --output-dir build/node52 node52
g++ -std=c++11 -Wall -Wextra test_pulses.cpp -o build/test_pulses
build/test_pulses
g++ -std=c++11 -Wall -Wextra test_reports.cpp -o build/test_reports
build/test_reports
g++ -std=c++11 -Wall -Wextra test_revolutions.cpp -o build/test_revolutions
build/test_revolutions
g++ -std=c++11 -Wall -Wextra test_tips.cpp -o build/test_tips
build/test_tips
g++ -std=c++11 -Wall -Wextra test_bmp180.cpp -o build/test_bmp180
build/test_bmp180
python -m unittest test_monitor
python test_trace_layout.py
