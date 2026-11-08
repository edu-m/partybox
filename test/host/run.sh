#!/bin/bash
# Host-side tests: partybox's real efi_main under ASan/UBSan against a fake firmware.
#   test/host/run.sh [OBJDIR]       (needs `make` + `make tests` first; see `make check`)
cd "$(dirname "$0")/../.."
OBJ=${1:-build/host}
F=$OBJ/fakefw
ESP=$OBJ/esp
fails=0; total=0
export ESP

run() { # name  [VAR=val ...]  -- KEYS comes from the environment list
    local name=$1; shift
    total=$((total+1))
    local out; out=$(env "$@" timeout 120 "$F" 2>&1); local rc=$?
    if [ $rc -ne 0 ]; then
        fails=$((fails+1))
        echo "FAIL  $name"
        echo "$out" | grep -E "ERROR: |SUMMARY|VIOLATION|LEAK|runtime error|^    #[0-6] |^FAIL" | head -${LINES_SHOWN:-9} | sed 's/^/        /'
    else
        [ -n "$VERBOSE" ] && echo "ok    $name  $(echo "$out" | tail -1)"
    fi
    return 0
}
mkesp() { python3 test/host/mkesp.py "$ESP" "$@" >/dev/null; }

mkesp
echo "== resolutions (navigation, rescan)"
for res in 3840x2160 2560x1440 2000x1400 1920x1080 1280x720 1024x768 800x600 640x480 480x320 320x200; do
    run "nav $res" W=${res%x*} H=${res#*x} KEYS="DOWN DOWN END PGUP HOME PGDN UP j k r F5 ESC"
done
echo "== tiny screens (heights < 200 px; no real GOP mode is this small)"
for res in 640x150 1920x150 200x100 160x120 100x60; do
    run "nav $res" W=${res%x*} H=${res#*x} KEYS="DOWN END HOME ESC"
done

echo "== launching every kind of child"
for app in hello exitdata binexit binerr longexit crashy warn modeswitch secfail denied blackjack vmlinuz-fake 'tools\nested' 'bulk\app07' 'unicode-é中\über'; do
    mkesp --conf "$(printf 'selection=%s\ncountdown=0\n' "$app")"
    run "launch $app" KEYS="ENTER | ESC"
    run "launch $app (BADCON, text console)" KEYS="ENTER | ESC" BADCON=1 NOGOP=1
    run "launch $app (child changes mode, restore fails)" KEYS="ENTER | ESC" SETMODE_FAIL=1
done
mkesp --conf $'selection=hello\ncountdown=2\n'
run "autoboot after 2 ticks" KEYS="T T | ESC"
run "autoboot cancelled by a key" KEYS="T DOWN ESC"
run "autoboot, tick then key then tick" KEYS="T j T ESC"
run "autoboot with timeout=0 in load options" LOADOPT="timeout=0" KEYS="ESC"
run "autoboot override timeout=1" LOADOPT="x timeout=1 y" KEYS="T | ESC"
run "load options: odd byte size" LOADOPT="timeout=7" LOADOPT_ODD=1 KEYS="ESC"
run "load options: huge number" LOADOPT="timeout=99999999999999999999" KEYS="ESC"
run "load options: just the key" LOADOPT="timeout=" KEYS="ESC"
run "load options: short" LOADOPT="a" KEYS="ESC"
run "SB on, ESC from nothing" SB=1 KEYS="ESC"

echo "== Secure Boot confirmation dialog"
mkesp --conf $'selection=hello\ncountdown=0\n'
run "SB: unsigned, cancel with n"        SB=1 KEYS="ENTER n | ESC"
run "SB: unsigned, cancel with default"  SB=1 KEYS="ENTER ENTER | ESC"
run "SB: unsigned, TAB then continue"    SB=1 KEYS="ENTER TAB ENTER | ESC"
run "SB: unsigned, y continues"          SB=1 KEYS="ENTER y | ESC"
run "SB: unsigned, esc cancels"          SB=1 KEYS="ENTER ESC ESC"
run "SB: text console dialog"            SB=1 NOGOP=1 KEYS="ENTER y | ESC"
mkesp --conf $'selection=signed\ncountdown=0\n'
run "SB: signed app, no dialog"          SB=1 KEYS="ENTER | ESC"
mkesp --conf $'selection=secfail\ncountdown=0\n'
run "SB: firmware rejects (SECURITY_VIOLATION)" SB=1 KEYS="ENTER y | ESC"

echo "== system actions"
mkesp
run "firmware settings"  KEYS="END UP UP ENTER"
run "reboot"             KEYS="END UP UP UP ENTER"
run "shutdown"           KEYS="END UP ENTER"
run "exit row"           KEYS="END ENTER"

echo "== firmware misbehaving"
mkesp --conf $'selection=hello\ncountdown=3\n'
run "SetTimer fails when autoboot starts"   SETTIMER_FAIL=1 KEYS="DOWN | ESC"
run "WaitForEvent keeps failing (no input)" WAIT_FAIL=1 EXPECT_EXHAUST=1 KEYS="j"

echo "== how partybox was loaded"
run "no DeviceHandle (LI=1)"  LI=1 KEYS="ESC"
run "no FilePath (LI=2)"      LI=2 KEYS="ESC"
run "no GOP at all"           NOGOP=1 KEYS="DOWN r ESC"

echo "== repeated rescans and launches (leak/stability)"
keys=""; for i in $(seq 1 200); do keys="$keys r"; done
run "200 rescans" KEYS="$keys ESC"
mkesp --conf $'selection=exitdata\ncountdown=0\n'
keys=""; for i in $(seq 1 60); do keys="$keys ENTER |"; done
run "60 launches of an app that returns exit data" KEYS="$keys ESC"
mkesp --conf $'selection=hello\ncountdown=3\n'
keys=""; for i in $(seq 1 100); do keys="$keys T DOWN UP"; done
run "countdown started and cancelled repeatedly" KEYS="$keys ESC"

echo "== configuration files"
conf() { mkesp --conf "$1"; }
conf $'\xef\xbb\xbfcountdown=1\nselection=hello\n';          run "UTF-8 BOM"          KEYS="T ESC"
printf 'countdown=1\r\nselection=hello\r\n' > "$OBJ/c1";     conf @"$OBJ/c1"; run "CRLF"  KEYS="T ESC"
python3 -c "import sys;sys.stdout.buffer.write('﻿countdown=1\nselection=hello\n'.encode('utf-16le'))" > "$OBJ/c2"; conf @"$OBJ/c2"; run "UTF-16LE BOM" KEYS="T ESC"
python3 -c "import sys;sys.stdout.buffer.write(b'\xff\xfe'+'countdown=1\nselection=hello'.encode('utf-16le')+b'\x00')" > "$OBJ/c3"; conf @"$OBJ/c3"; run "UTF-16LE odd tail" KEYS="T ESC"
head -c 100000 /dev/urandom > "$OBJ/c4";                      conf @"$OBJ/c4"; run "random 100 KB"  KEYS="ESC"
python3 -c "print('selection='+'a'*300+'\ncountdown=70000\nselection='+'b'*254)" > "$OBJ/c5"; conf @"$OBJ/c5"; run "overlong values" KEYS="ESC"
python3 -c "print('x'*100000)" > "$OBJ/c6";                  conf @"$OBJ/c6"; run "one 100 KB line" KEYS="ESC"
printf 'selection=nothing-like-this\n' > "$OBJ/c7";         conf @"$OBJ/c7"; run "selection that matches nothing" KEYS="ESC"
printf 'selection=\\\\\\\\\\\\\n=\n =x\ncountdown\n  COUNTDOWN =  3  \n' > "$OBJ/c8"; conf @"$OBJ/c8"; run "degenerate lines" KEYS="ESC"
head -c 70000 /dev/zero | tr '\0' 'a' > "$OBJ/c9";           conf @"$OBJ/c9"; run "70 KB file (over the limit)" KEYS="ESC"

echo
if [ $fails -eq 0 ]; then echo "all $total host tests passed"; else echo "$fails of $total host tests FAILED"; fi
exit $((fails != 0))
