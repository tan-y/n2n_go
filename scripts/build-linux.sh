#!/usr/bin/env bash
# Host entry point: builds an arm64/amd64 Linux binary in a matching container.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=${1:-arm64}
LIBC=${2:-musl}
N2N_SRC_HOST=${N2N_SRC_HOST:-$ROOT/third_party/n2n}

case "$ARCH" in
  amd64|arm64) ;;
  *) echo "usage: $0 <amd64|arm64> [musl]" >&2; exit 2 ;;
esac
case "$LIBC" in
  musl|glibc) ;;
  *) echo "unsupported libc: $LIBC" >&2; exit 2 ;;
esac

if [ ! -d "$N2N_SRC_HOST" ]; then
  echo "n2n source not found: $N2N_SRC_HOST" >&2
  echo "run: git submodule update --init third_party/n2n" >&2
  exit 2
fi

docker build --platform "linux/$ARCH" \
  -f "$ROOT/docker/linux/Dockerfile.$LIBC" \
  -t "n2n-go-build:linux-$ARCH-$LIBC" "$ROOT/docker/linux"

docker run --rm --platform "linux/$ARCH" \
  -e ARCH="$ARCH" -e LIBC="$LIBC" -e N2N_SRC=/n2n-src \
  -v "$ROOT":/work -w /work \
  -v "$N2N_SRC_HOST":/n2n-src:ro \
  "n2n-go-build:linux-$ARCH-$LIBC" \
  ./scripts/build-n2n-linux.sh

if [ "$LIBC" = musl ]; then
  RUNTIME_DOCKERFILE="Dockerfile.runtime"
else
  RUNTIME_DOCKERFILE="Dockerfile.runtime.glibc"
fi
docker build --platform "linux/$ARCH" \
  -f "$ROOT/docker/linux/$RUNTIME_DOCKERFILE" \
  --build-arg ARCH="$ARCH" \
  -t "n2n-go-linux:$ARCH-$LIBC" "$ROOT/bin"
