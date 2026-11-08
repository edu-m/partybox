#!/bin/bash
# Build and run the libFuzzer targets (ASan + UBSan). Usage: test/host/fuzz.sh [SECONDS_PER_TARGET]
# Crashing inputs land in build/fuzz/artifacts/. Seeds come from the real binaries and the fixture ESP.
set -e
cd "$(dirname "$0")/../.."
SECS=${1:-60}
EDK2=${EDK2:-edk2}
O=build/fuzz
mkdir -p $O/artifacts $O/corp_pe $O/corp_conf $O/corp_dp
SAN=(-fsanitize=fuzzer,address,undefined -fno-sanitize=function -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
LIBSAN=(-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize=function -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
COM=(-g -O1 -fshort-wchar -I$EDK2/MdePkg/Include -I$EDK2/MdePkg/Include/X64 -Isrc -Wno-macro-redefined '-DPARTYBOX_VERSION=L"fuzz"')
REN=(-Dmemset=pb_memset -Dmemcpy=pb_memcpy -Dmemmove=pb_memmove -Dmemcmp=pb_memcmp)

for f in lib gfx scan; do clang "${COM[@]}" "${LIBSAN[@]}" "${REN[@]}" -c src/$f.c -o $O/$f.o & done; wait
clang "${COM[@]}" "${SAN[@]}" "${REN[@]}" test/host/fuzz_pe.c   $O/lib.o -o $O/fuzz_pe
clang "${COM[@]}" "${SAN[@]}" "${REN[@]}" test/host/fuzz_dp.c   $O/lib.o -o $O/fuzz_dp
clang "${COM[@]}" "${SAN[@]}" "${REN[@]}" test/host/fuzz_conf.c $O/lib.o $O/gfx.o $O/scan.o -o $O/fuzz_conf

# Seeds: real PE images plus the malformed ones from the fixture ESP.
cp -n build/*.efi build/test/*.efi $O/corp_pe/ 2>/dev/null || true
python3 test/host/mkesp.py $O/esp >/dev/null
cp -n $O/esp/EFI/partybox/PARTYBOX/*.efi $O/corp_pe/ 2>/dev/null || true
printf '\x00countdown=10\nselection=doom\\PlayDoom.efi\n' > $O/corp_conf/c1
printf '\x00\xff\xfec\x00o\x00u\x00n\x00t\x00d\x00o\x00w\x00n\x00=\x005\x00\n\x00' > $O/corp_conf/c2
printf '\x01timeout=5 foo' > $O/corp_conf/t1
printf '\x01t\x00i\x00m\x00e\x00o\x00u\x00t\x00=\x007\x00' > $O/corp_conf/t2
printf '\x04\x04\x24\x00\\\x00E\x00F\x00I\x00\\\x00x\x00.\x00e\x00f\x00i\x00\x00\x00' > $O/corp_dp/d1

rc=0
for t in pe conf dp; do
    echo "== fuzz_$t ($SECS s)"
    ASAN_OPTIONS=detect_leaks=0 $O/fuzz_$t -max_total_time=$SECS -timeout=10 -rss_limit_mb=2048 \
        -artifact_prefix=$O/artifacts/$t- $O/corp_$t 2>&1 | grep -E "ERROR|SUMMARY|runtime error|^Done " || rc=1
done
ls $O/artifacts | grep -q . && { echo "crashing inputs saved in $O/artifacts/"; rc=1; }
exit $rc
