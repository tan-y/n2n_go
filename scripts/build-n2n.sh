#!/usr/bin/env bash
# Build libn2n.a for macOS from the n2n source tree.
#
# Usage: git submodule update --init third_party/n2n
#        ./scripts/build-n2n.sh [arm64|x86_64]
#        N2N_SRC=/path/to/n2n ./scripts/build-n2n.sh arm64
#
# Must be executed inside the Nix shell (autoconf/automake/pkg-config needed):
#   nix-shell shell.nix --run './scripts/build-n2n.sh arm64'

set -euo pipefail

ARCH="${1:-arm64}"
case "$ARCH" in
  arm64|x86_64) ;;
  *) echo "unsupported arch: $ARCH (expected arm64 or x86_64)" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
N2N_SRC="${N2N_SRC:-$ROOT/third_party/n2n}"
WORK="$ROOT/build/n2n-$ARCH"
LIB_DIR="$ROOT/third_party/lib/darwin/$ARCH"
INCLUDE_DIR="$ROOT/third_party/n2n-src/include"

if [ ! -d "$N2N_SRC" ]; then
  echo "n2n source not found: $N2N_SRC (set N2N_SRC)" >&2
  exit 2
fi

for tool in autoconf autoreconf automake pkg-config make patch rsync; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "missing build tool: $tool (run this inside 'nix-shell shell.nix')" >&2
    exit 2
  fi
done

echo "==> preparing $WORK from $N2N_SRC"
rm -rf "$WORK"
mkdir -p "$WORK"
rsync -a --exclude build --exclude bin --exclude .git --exclude '*.dSYM' "$N2N_SRC/" "$WORK/"

echo "==> applying utun adapter and keep_running patch"
cp "$ROOT/platform/macos/tuntap_osx.c" "$WORK/src/tuntap_osx.c"
( cd "$WORK" && patch -p1 --quiet -i "$ROOT/patches/n2n-keep-running.patch" )

echo "==> autogen + configure ($ARCH, Apple clang)"
# nix-shell injects its own apple-sdk via SDKROOT/NIX_CFLAGS_COMPILE, which would
# build the library against nixpkgs' SDK and its default deployment target.
# Drop those so /usr/bin/clang uses the Xcode Command Line Tools SDK, and pin the
# deployment target to the value Go uses for darwin/arm64.
unset SDKROOT NIX_CFLAGS_COMPILE NIX_CFLAGS_LINK NIX_LDFLAGS
export MACOSX_DEPLOYMENT_TARGET=11.0
(
  cd "$WORK"
  ./autogen.sh
  ./configure \
    CC=/usr/bin/clang \
    CFLAGS="-arch $ARCH -O2 -Wall -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -DSKIP_MULTICAST_PEERS_DISCOVERY" \
    LDFLAGS="-arch $ARCH"
)

echo "==> building libn2n.a"
make -C "$WORK" libn2n.a

echo "==> installing artifacts"
mkdir -p "$LIB_DIR" "$(dirname "$INCLUDE_DIR")"
cp "$WORK/libn2n.a" "$LIB_DIR/libn2n.a"
rm -rf "$INCLUDE_DIR"
mkdir -p "$INCLUDE_DIR"
rsync -a "$WORK/include/" "$INCLUDE_DIR/"

echo "==> done"
file "$LIB_DIR/libn2n.a"
