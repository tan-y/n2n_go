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

echo "==> lifecycle: auto tapN, signal exit code, TAP cleanup"
for sig in TERM INT; do
  echo "--- SIG$sig ---"
  docker run --rm --platform "linux/$ARCH" \
    --cap-add NET_ADMIN --device-cgroup-rule 'c 10:200 rwm' \
    -e SIG="$sig" "$IMAGE" sh -c '
      mkdir -p /dev/net
      [ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200
      /usr/local/bin/n2n edge -c lifecycle -k lifecycle -a 10.9.0.1/24 \
        -l 127.0.0.1:7777 >/tmp/edge.log 2>&1 &
      pid=$!
      for _ in $(seq 1 30); do
        grep -q "device=tap" /tmp/edge.log && break
        sleep 0.2
      done
      cat /tmp/edge.log
      dev=$(ip -o link show type tun | awk -F": " "NR==1 {print \$2}")
      if [ -z "$dev" ]; then
        echo "FAIL: no auto-assigned tap device"
        exit 3
      fi
      kill -"$SIG" "$pid"
      wait "$pid"
      rc=$?
      sleep 1
      if ip link show "$dev" >/dev/null 2>&1; then
        echo "FAIL: $dev still exists after SIG$SIG"
        exit 4
      fi
      if [ "$rc" -ne 0 ]; then
        echo "FAIL: edge exit code $rc after SIG$SIG"
        exit 5
      fi
      echo "PASS: $dev removed, exit code 0"
    '
done

echo "==> smoke test passed"
