#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CLI=${ARDUINO_CLI:-/home/user/pnp/Arduino/arduino-cl/bin/arduino-cli}
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
python -m unittest test_monitor
python test_trace_layout.py
