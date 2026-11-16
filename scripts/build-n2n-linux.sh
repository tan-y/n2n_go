#!/usr/bin/env bash
# Builds libn2n.a and the Go binary inside a Linux container.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=${ARCH:-arm64}
LIBC=${LIBC:-musl}
N2N_SRC=${N2N_SRC:-/n2n-src}

case "$ARCH" in
  amd64|arm64) ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 2 ;;
esac
case "$LIBC" in
  musl|glibc) ;;
  *) echo "unsupported libc: $LIBC" >&2; exit 2 ;;
esac

if [ ! -d "$N2N_SRC" ]; then
  echo "n2n source not found: $N2N_SRC (run: git submodule update --init third_party/n2n)" >&2
  exit 2
fi

WORK="$ROOT/build/n2n-linux-$ARCH-$LIBC"
LIB="$ROOT/third_party/lib/linux/$ARCH-$LIBC"
INCLUDE_DIR="$ROOT/third_party/n2n-src/include"

rm -rf "$WORK"
mkdir -p "$WORK"
rsync -a --exclude build --exclude bin --exclude .git "$N2N_SRC/" "$WORK/"

cd "$WORK"
patch -p1 --quiet -i "$ROOT/patches/n2n-keep-running.patch"
patch -p1 --quiet -i "$ROOT/patches/n2n-tuntap-linux.patch"

./autogen.sh
./configure CFLAGS="-O2 -Wall -DSKIP_MULTICAST_PEERS_DISCOVERY"
make libn2n.a
make supernode

mkdir -p "$LIB" "$ROOT/third_party/lib/linux/$ARCH" "$ROOT/bin"
cp libn2n.a "$LIB/libn2n.a"
cp libn2n.a "$ROOT/third_party/lib/linux/$ARCH/libn2n.a"
cp supernode "$ROOT/bin/supernode-linux-$ARCH-$LIBC"

rm -rf "$INCLUDE_DIR"
mkdir -p "$INCLUDE_DIR"
rsync -a include/ "$INCLUDE_DIR/"

cd "$ROOT"
export CGO_ENABLED=1 GOOS=linux GOARCH="$ARCH"
case "$LIBC" in
  musl) GO_LDFLAGS='-linkmode external -extldflags "-static"' ;;
  glibc) GO_LDFLAGS='-linkmode external' ;;
esac
go build -a -trimpath -buildvcs=false \
  -ldflags "$GO_LDFLAGS" \
  -o "$ROOT/bin/n2n-linux-$ARCH-$LIBC" ./cmd/n2n
