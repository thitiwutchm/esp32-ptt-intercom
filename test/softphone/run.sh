#!/bin/sh
# tools/sip_phone.py against the device's SIP stack (fake_device.c) on
# localhost, both directions, checking the test tone comes back.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/../.."
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
VOIP="$ROOT/components/voip_core"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$VOIP/include" \
   "$HERE/fake_device.c" "$VOIP"/*.c -lm -o "$WORK/fake_device"
PHONE="python3 $ROOT/tools/sip_phone.py --port 5090 --tone"
fail=0

echo "== laptop calls the device, laptop hangs up"
"$WORK/fake_device" answer 5090 5092 > "$WORK/dev1.log" 2>&1 &
DEV=$!
sleep 0.5
$PHONE --record "$WORK/a.wav" call 127.0.0.1:5092 > "$WORK/phone1.log" 2>&1 &
PH=$!
sleep 4
kill $PH
wait $PH || { echo "phone exited with an error"; fail=1; }
wait $DEV || fail=1
cat "$WORK/phone1.log" "$WORK/dev1.log"
python3 "$HERE/check_tone.py" "$WORK/a.wav" || fail=1

echo "== device calls the laptop, device hangs up"
$PHONE --record "$WORK/b.wav" answer > "$WORK/phone2.log" 2>&1 &
PH=$!
sleep 0.5
"$WORK/fake_device" call 5090 5092 > "$WORK/dev2.log" 2>&1 || fail=1
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 $PH 2>/dev/null || break; sleep 0.2; done
if kill -0 $PH 2>/dev/null; then
    echo "phone did not exit after the device hung up"
    kill $PH
    fail=1
fi
wait $PH || fail=1
cat "$WORK/phone2.log" "$WORK/dev2.log"
python3 "$HERE/check_tone.py" "$WORK/b.wav" || fail=1

[ $fail = 0 ] && echo PASSED || echo FAILED
exit $fail
