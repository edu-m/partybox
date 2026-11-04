#!/usr/bin/env bash
# Drive a headless QEMU/OVMF VM for scripted testing: boot an ESP image,
# send keys, take screenshots, talk to the monitor.
#
#   tools/qemu-headless.sh start [IMG]       boot IMG (default build/esp.img)
#   tools/qemu-headless.sh wait-partybox [S] wait up to S s for partybox's menu
#   tools/qemu-headless.sh key K [K ...]     send keys (QEMU names: ret, down, a, esc...)
#   tools/qemu-headless.sh shot NAME         screenshot to $STATE/NAME.png
#   tools/qemu-headless.sh mon CMD...        run a monitor command
#   tools/qemu-headless.sh stop
#
# Environment: STATE (work dir, default build/qemu-headless), MEM (default
# 1024), SB=1 (Secure Boot OVMF with build/sb/OVMF_VARS.fd), EXTRA (more
# QEMU args, e.g. "-enable-kvm -cpu host -serial file:serial.log").
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STATE=${STATE:-$ROOT/build/qemu-headless}
OVMF_DIR=${OVMF_DIR:-/usr/share/edk2/x64}
mkdir -p "$STATE"
MON=$STATE/mon.sock PID=$STATE/qemu.pid

mon() { printf '%s\n' "$*" | socat - "unix-connect:$MON"; }

shot() { # NAME -> $STATE/NAME.png
    mon "screendump $STATE/$1.ppm" >/dev/null
    for _ in $(seq 1 20); do [ -s "$STATE/$1.ppm" ] && break; sleep 0.1; done
    sleep 0.2
    magick "$STATE/$1.ppm" "$STATE/$1.png" && rm -f "$STATE/$1.ppm"
    echo "$STATE/$1.png"
}

stop() {
    [ -f "$PID" ] && kill "$(cat "$PID")" 2>/dev/null || true
    for _ in $(seq 1 20); do pgrep -F "$PID" >/dev/null 2>&1 || break; sleep 0.25; done
    rm -f "$PID"
}

case "${1:-}" in
start)
    stop
    img=${2:-$ROOT/build/esp.img}
    if [ -n "${SB:-}" ]; then
        cp "$ROOT/build/sb/OVMF_VARS.fd" "$STATE/vars.fd"
        fw=(-machine q35,smm=on -global driver=cfi.pflash01,property=secure,value=on
            -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_DIR/OVMF_CODE.secboot.4m.fd")
    else
        cp "$OVMF_DIR/OVMF_VARS.4m.fd" "$STATE/vars.fd"
        fw=(-machine q35 -drive "if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_DIR/OVMF_CODE.4m.fd")
    fi
    rm -f "$MON"
    # shellcheck disable=SC2086
    qemu-system-x86_64 "${fw[@]}" -m "${MEM:-1024}" -net none -display none \
        -drive "if=pflash,format=raw,unit=1,file=$STATE/vars.fd" \
        -drive "format=raw,file=$img" ${EXTRA:-} \
        -monitor "unix:$MON,server,nowait" -daemonize -pidfile "$PID"
    ;;
wait-partybox)
    # partybox's background colour at (20,20) is #1a16xx/#1b17xx.
    for _ in $(seq 1 $(( ${2:-30} * 2 ))); do
        mon "screendump $STATE/w.ppm" >/dev/null 2>&1 || true
        sleep 0.3
        c=$(magick "$STATE/w.ppm" -format "%[hex:p{20,20}]" info: 2>/dev/null || true)
        case "$c" in 1A16*|1B17*|1A17*|1B16*) exit 0 ;; esac
        sleep 0.2
    done
    echo "timeout waiting for partybox" >&2
    exit 1
    ;;
key) shift; for k in "$@"; do mon "sendkey $k" >/dev/null; sleep 0.3; done ;;
shot) shot "$2" ;;
mon) shift; mon "$*" ;;
stop) stop ;;
*) sed -n '2,15p' "$0"; exit 2 ;;
esac
