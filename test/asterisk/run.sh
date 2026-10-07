#!/bin/sh
# Start a private Asterisk with etc/ from this directory, run the SIP interop
# test against it, stop Asterisk. Needs the asterisk package (apt install asterisk).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
stop() {
    asterisk -C "$WORK/etc/asterisk.conf" -rx "core stop now" >/dev/null 2>&1 || true
    [ -f "$WORK/run/asterisk.pid" ] && kill -9 "$(cat "$WORK/run/asterisk.pid")" 2>/dev/null || true
    rm -rf "$WORK"
}
trap stop EXIT
cp -r "$HERE/etc" "$WORK/etc"
mkdir -p "$WORK/run"
sed -i "s|@DIR@|$WORK|g" "$WORK/etc/asterisk.conf"
ARCH_MODS=$(dirname "$(find /usr/lib -name chan_pjsip.so 2>/dev/null | head -1)")
sed -i "s|^astmoddir => .*|astmoddir => $ARCH_MODS|" "$WORK/etc/asterisk.conf"

VOIP="$HERE/../../components/voip_core"
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$VOIP/include" \
   "$HERE/sip_interop.c" "$VOIP"/*.c -lm -o "$WORK/sip_interop"

asterisk -C "$WORK/etc/asterisk.conf"
for i in $(seq 1 50); do
    asterisk -C "$WORK/etc/asterisk.conf" -rx "pjsip show transports" 2>/dev/null | grep -q transport-udp && break
    sleep 0.2
done
"$WORK/sip_interop" "$WORK/etc/asterisk.conf"
