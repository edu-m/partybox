#!/usr/bin/env bash
# Build apps/uefidoom (Cacodemon345/uefidoom) on Linux with edk2's GCC
# toolchain and write build/doom.efi.
#
# Everything lands in build/doomws: pinned clones of edk2-libc and AudioPkg,
# a patched copy of the DOOM sources, edk2's Conf/ and Build/. The edk2
# submodule only gets its BaseTools compiled (ignored by its .gitignore).
#
# Needs: gcc, make, nasm, python3, libuuid headers, git (network on first run).
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
HERE=$ROOT/apps/uefidoom-build
EDK2=${EDK2:-$ROOT/edk2}
DOOM=$ROOT/apps/uefidoom
W=$ROOT/build/doomws
TARGET=${TARGET:-RELEASE}

LIBC_URL=https://github.com/tianocore/edk2-libc.git
LIBC_REV=73056078b60c9378af08dd122a29913e25e84e1c
AUDIO_URL=https://github.com/FangJiaZhen/AudioPkg.git
AUDIO_REV=e6ee2a317e0c9229ebad238a29cf37fd0aa9c422

# Shallow-fetch exactly one commit into $1, unless it's already checked out.
fetch() {
    local dir=$1 url=$2 rev=$3
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    if [ "$(git -C "$dir" rev-parse -q --verify HEAD 2>/dev/null || true)" != "$rev" ]; then
        echo ">> fetching $url @ ${rev:0:12}"
        git -C "$dir" fetch -q --depth 1 origin "$rev"
        git -C "$dir" checkout -q --detach FETCH_HEAD
    fi
}

[ -f "$DOOM/uefidoom/doom.inf" ] || {
    echo "apps/uefidoom is missing; run: git submodule update --init apps/uefidoom" >&2
    exit 1
}
[ -f "$EDK2/edksetup.sh" ] || {
    echo "edk2 at $EDK2 is missing; run: git submodule update --init edk2 (a full checkout is needed here)" >&2
    exit 1
}

if [ ! -x "$EDK2/BaseTools/Source/C/bin/GenFw" ]; then
    echo ">> building edk2 BaseTools"
    make -C "$EDK2/BaseTools" -j"$(nproc)" >/dev/null
fi

mkdir -p "$W/audio" "$W/src" "$W/Conf"
fetch "$W/edk2-libc" "$LIBC_URL" "$LIBC_REV"
fetch "$W/audio/AudioPkg" "$AUDIO_URL" "$AUDIO_REV"

# Fresh copy of the port with our patches (patches/NNNN-*.patch, in order);
# the submodule stays pristine.
rm -rf "$W/src/uefidoom"
mkdir -p "$W/src/uefidoom"
cp -r "$DOOM/uefidoom" "$W/src/uefidoom/"
for p in "$HERE"/patches/*.patch; do
    patch -s --batch -d "$W/src/uefidoom" -p1 < "$p"
done
cp "$HERE/DoomBuild.dsc" "$W/DoomBuild.dsc"

echo ">> building doom.efi ($TARGET)"
(
    export WORKSPACE=$W EDK_TOOLS_PATH=$EDK2/BaseTools CONF_PATH=$W/Conf
    export PACKAGES_PATH=$EDK2:$W/edk2-libc:$W/audio:$W/src
    cd "$EDK2"
    # edksetup.sh trips over `set -u`
    set +u
    . ./edksetup.sh BaseTools >/dev/null
    cd "$W"
    build -q -a X64 -t GCC -b "$TARGET" -n "$(nproc)" -p DoomBuild.dsc
)

cp "$W/Build/DoomPkg/${TARGET}_GCC/X64/doom.efi" "$ROOT/build/doom.efi"
echo ">> build/doom.efi"
