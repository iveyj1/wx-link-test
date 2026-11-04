#!/usr/bin/env python3
"""Check the retained diagnostic trace in each build sits in .noinit after BSS (so startup
does not clear it) and ends before the heap. Run after ./build.sh."""
import glob, subprocess, sys
NM = (glob.glob('/home/user/.arduino15/packages/adafruit/tools/arm-none-eabi-gcc/*/bin/arm-none-eabi-nm') or ['arm-none-eabi-nm'])[0]
ok = True
for role in ('sensor', 'receiver'):
    syms = {}
    for line in subprocess.run([NM, '-S', f'build/{role}/link.ino.elf'], capture_output=True, text=True, check=True).stdout.split('\n'):
        f = line.split()
        if len(f) == 4: syms[f[3]] = (int(f[0], 16), int(f[1], 16))
        elif len(f) == 3: syms[f[2]] = (int(f[0], 16), 0)
    addr, size = syms['traceRecord']
    good = addr >= syms['__bss_end__'][0] and addr + size <= syms['end'][0] and size == 96
    ok &= good
    print(f"{'PASS' if good else 'FAIL'} {role}: retained trace at 0x{addr:08x}, {size} bytes, "
          f"{'outside BSS and before heap' if good else 'OVERLAPS BSS/heap or wrong size'}")
sys.exit(0 if ok else 1)
