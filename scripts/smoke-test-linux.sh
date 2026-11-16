#!/usr/bin/env bash
# Linux smoke test: supernode + two edges, each in its own container.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=${ARCH:-arm64}
LIBC=${1:-${LIBC:-musl}}
COMPOSE="$ROOT/scripts/docker-compose.linux.yml"
IMAGE="n2n-go-linux:$ARCH-$LIBC"
export N2N_IMAGE="$IMAGE"

if [ ! -x "$ROOT/bin/n2n-linux-$ARCH-$LIBC" ] || \
   [ ! -x "$ROOT/bin/supernode-linux-$ARCH-$LIBC" ] || \
   ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  "$ROOT/scripts/build-linux.sh" "$ARCH" "$LIBC"
fi

logs() {
  for svc in supernode edge1 edge2; do
    printf '\n--- %s ---\n' "$svc" >&2
    docker compose -p n2n-linux-smoke -f "$COMPOSE" logs --no-color "$svc" >&2 || true
  done
}

cleanup() {
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    logs
  fi
  docker compose -p n2n-linux-smoke -f "$COMPOSE" down --remove-orphans >/dev/null 2>&1 || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_log() {
  local svc=$1 pattern=$2
  for _ in $(seq 1 40); do
    if docker compose -p n2n-linux-smoke -f "$COMPOSE" logs --no-color "$svc" 2>/dev/null | grep -F -q "$pattern"; then
      return 0
    fi
    sleep 1
  done
  echo "timed out waiting for '$pattern' in $svc logs" >&2
  return 1
}

echo "==> starting supernode and two edges"
docker compose -p n2n-linux-smoke -f "$COMPOSE" up -d

echo "==> waiting for edges to start"
wait_log edge1 "starting edge"
wait_log edge2 "starting edge"
docker compose -p n2n-linux-smoke -f "$COMPOSE" exec -T edge1 ip link show tap0 >/dev/null
docker compose -p n2n-linux-smoke -f "$COMPOSE" exec -T edge2 ip link show tap0 >/dev/null

echo "==> waiting for supernode registration"
wait_log edge1 "[OK] edge"
wait_log edge2 "[OK] edge"

echo "==> pinging edge2 from edge1"
ok=0
for _ in $(seq 1 10); do
  if docker compose -p n2n-linux-smoke -f "$COMPOSE" exec -T edge1 \
       ping -c 1 -W 1 10.0.0.2; then
    ok=1
    break
  fi
  sleep 1
done
[ "$ok" -eq 1 ] || { echo "ping over the n2n overlay failed" >&2; exit 1; }

echo "==> interface counters"
docker compose -p n2n-linux-smoke -f "$COMPOSE" exec -T edge1 ip -s link show tap0

echo "==> smoke test passed"
