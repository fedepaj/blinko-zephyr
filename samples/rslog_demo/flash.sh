#!/bin/sh
# Flash the Zephyr app to the Arduino Nano 33 BLE through its bootloader:
# 1200-baud touch (handled by our firmware) then bossac right away.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); MOD=$(cd "$HERE/../.." && pwd)
BIN=${BIN:-$MOD/build/zephyr/zephyr.bin}
PY=${PY:-python3}
BOSSAC=${BOSSAC:-$HOME/Library/Arduino15/packages/arduino/tools/bossac/1.9.1-arduino2/bossac}
PORT=${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
[ -n "$PORT" ] || { echo "no usbmodem port" >&2; exit 2; }
"$PY" - "$PORT" <<'PY'
import serial, sys, time
p = sys.argv[1]
try:
    s = serial.Serial(p, 1200, timeout=0.2, dsrdtr=True); s.dtr = True; time.sleep(0.2); s.dtr = False; s.close()
except Exception as e:
    print("touch:", e)
time.sleep(1.2)
PY
GIVEN=$PORT
for i in 1 2 3 4 5 6 7 8; do
    # the bootloader keeps the same port on this board; with several boards attached the
    # caller's PORT must win over the first usbmodem entry
    PORT=${GIVEN:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}
    if [ -n "$PORT" ] && [ -e "$PORT" ] && "$BOSSAC" -p "$PORT" -R -w -v -b "$BIN" 2>&1 | grep -qE "Verify successful"; then echo "flashed $BIN on $PORT"; exit 0; fi
    sleep 0.7
done
echo "flash failed (double-tap RESET and retry)" >&2; exit 1
