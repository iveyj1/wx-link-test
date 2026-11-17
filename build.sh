#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CLI=${ARDUINO_CLI:-/home/user/pnp/Arduino/arduino-cl/bin/arduino-cli}
# Python with pyserial: $PYTHON, else the venv, else whatever python3 is on PATH.
PY=${PYTHON:-$( [[ -x ~/.local/bin/venv/bin/python3 ]] && echo ~/.local/bin/venv/bin/python3 || command -v python3)}
for role in sensor receiver; do
  flag=1; [[ $role == receiver ]] && flag=0
  "$CLI" compile --fqbn adafruit:samd:adafruit_feather_m0 \
    --build-property "compiler.cpp.extra_flags=-DSENSOR=$flag" \
    --build-property "compiler.c.elf.extra_flags=-Wl,-T,$PWD/retained.ld" \
    --output-dir "build/$role" link
done
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
g++ -std=c++11 -Wall -Wextra test_sht31.cpp -o build/test_sht31
build/test_sht31
g++ -std=c++11 -Wall -Wextra test_softi2c.cpp -o build/test_softi2c
build/test_softi2c
"$PY" -m unittest test_monitor test_web test_reference test_spindown
node test_display.js
"$PY" test_trace_layout.py
