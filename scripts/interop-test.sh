#!/usr/bin/env bash
# Interop test: our macOS edge (utun shim) against a native n2n edge running in
# a Linux container (TAP). Verifies registration, peer discovery, IPv4 unicast
# in both directions and the ARP responder the shim added for native peers.
#
# Needs root (utun creation) and a working docker daemon (OrbStack is fine).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="$(uname -m)"
BIN="$ROOT/bin/n2n-darwin-$ARCH"
WORK="$ROOT/build/n2n-$ARCH"
N2N_SRC="${N2N_SRC:-$ROOT/third_party/n2n}"
NATIVE_DIR="$ROOT/build/linux-$ARCH"
NATIVE_EDGE="$NATIVE_DIR/edge"

COMMUNITY="${COMMUNITY:-interop}"
KEY="${KEY:-interop-key}"
SN_PORT="${SN_PORT:-$((20000 + RANDOM % 20000))}"
SN_MGMT_PORT="${SN_MGMT_PORT:-$((SN_PORT + 1))}"
MGMT_PORT="${MGMT_PORT:-$((SN_PORT + 2))}"
OVERLAY_NET="${OVERLAY_NET:-10.44.0}"
HOST_IP="10.44.0.1"
NATIVE_IP="10.44.0.2"
CONTAINER="n2n-interop-native-$$"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/n2n-interop.XXXXXX")"
SN_LOG="$TMP/supernode.log"
EDGE_LOG="$TMP/edge.log"
NATIVE_LOG="$TMP/native.log"

SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO="sudo"

info() { printf '==> %s\n' "$*"; }
fail() {
  printf 'ERROR: %s\n' "$*" >&2
  for f in "$SN_LOG" "$EDGE_LOG" "$NATIVE_LOG"; do
    [ -f "$f" ] || continue
    printf '\n--- %s ---\n' "$(basename "$f")" >&2
    tail -n 20 "$f" >&2 || true
  done
  printf '\nlogs kept in %s\n' "$TMP" >&2
  exit 1
}

cleanup() {
  set +e
  docker rm -f "$CONTAINER" >/dev/null 2>&1
  if [ -n "${EDGE_PID:-}" ]; then kill "$EDGE_PID" 2>/dev/null; wait "$EDGE_PID" 2>/dev/null; fi
  if [ -n "${SN_PID:-}" ]; then kill "$SN_PID" 2>/dev/null; wait "$SN_PID" 2>/dev/null; fi
}
trap cleanup EXIT

wait_for() {
  local what=$1 file=$2 pattern=$3 tries=${4:-60} i
  for i in $(seq 1 "$tries"); do
    grep -q "$pattern" "$file" 2>/dev/null && return 0
    sleep 1
  done
  fail "timed out waiting for $what (pattern: $pattern)"
}

DOCKER="$(command -v docker || true)"
[ -x "$DOCKER" ] || fail "docker not found; the native peer runs in a Linux container"
"$DOCKER" info >/dev/null 2>&1 || fail "docker daemon not reachable (try without sudo, or start OrbStack/Docker Desktop)"

# ---------------------------------------------------------------- build
[ -x "$BIN" ] || fail "$BIN missing; run make build first"
if [ ! -x "$WORK/supernode" ]; then
  info "building test supernode"
  make -C "$WORK" supernode >/dev/null 2>&1 || fail "failed to build the test supernode"
fi

if [ ! -x "$NATIVE_EDGE" ]; then
  info "building native n2n edge for linux/$ARCH (one-off, cached in build/linux-$ARCH)"
  mkdir -p "$NATIVE_DIR"
  "$DOCKER" run --rm --platform "linux/$ARCH" \
    -v "$N2N_SRC:/src:ro" -v "$NATIVE_DIR:/out" alpine:3.20 \
    sh -c 'set -e
      apk add --no-cache build-base autoconf automake linux-headers >/dev/null
      cp -a /src /work && cd /work
      ./autogen.sh >/dev/null
      ./configure >/dev/null
      make -j"$(nproc)" LDFLAGS="-static -L." edge >/dev/null
      cp edge /out/edge' >"$TMP/build.log" 2>&1 ||
    { tail -n 20 "$TMP/build.log" >&2; fail "native edge build failed"; }
  chmod +x "$NATIVE_EDGE"
fi

if ! file "$NATIVE_EDGE" | grep -q "statically linked"; then
  info "note: $NATIVE_EDGE is not static; the alpine container may fail to load it"
fi

# ---------------------------------------------------------------- run
SN_BIN="$WORK/supernode"
info "stopping leftover test supernodes"
pkill -f "$SN_BIN -c $COMMUNITY" 2>/dev/null || true

info "starting supernode on port $SN_PORT"
# -t: default management port 5645 collides with leftovers from other tests.
"$SN_BIN" -c "$COMMUNITY" -p "$SN_PORT" -t "$SN_MGMT_PORT" -f >"$SN_LOG" 2>&1 &
SN_PID=$!
wait_for "supernode" "$SN_LOG" "supernode started"

LAN_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || true)"
[ -n "$LAN_IP" ] || fail "no LAN IPv4 address; the container must reach the supernode via the host"
info "host LAN address: $LAN_IP"

info "starting our edge ($HOST_IP/24, device utun, -E --ipv6)"
$SUDO "$BIN" edge -c "$COMMUNITY" -k "$KEY" -a "$HOST_IP/24" \
  -l "$LAN_IP:$SN_PORT" -d utun -t "$MGMT_PORT" -M 1290 -E --ipv6 -I our-edge >"$EDGE_LOG" 2>&1 &
EDGE_PID=$!
wait_for "our edge registration" "$EDGE_LOG" ">>> supernode"

info "starting native edge in container ($NATIVE_IP/24, device tap0, -E)"
"$DOCKER" run -d --name "$CONTAINER" --platform "linux/$ARCH" \
  --cap-add=NET_ADMIN --device /dev/net/tun \
  -v "$NATIVE_EDGE:/edge:ro" alpine:3.20 \
  /edge -f -c "$COMMUNITY" -k "$KEY" -a "$NATIVE_IP/24" \
  -l "host.docker.internal:$SN_PORT" -d tap0 -M 1290 -E >/dev/null 2>"$NATIVE_LOG"

sleep 2
# docker logs must be re-read every round: the file does not grow by itself.
for _ in $(seq 1 60); do
  "$DOCKER" logs "$CONTAINER" >"$NATIVE_LOG" 2>&1 || true
  grep -q ">>> supernode" "$NATIVE_LOG" && break
  sleep 1
done
grep -q ">>> supernode" "$NATIVE_LOG" || fail "native edge never registered with the supernode"

# ---------------------------------------------------------------- verify
# Broadcast first: the peer table is traffic driven, and a broadcast frame is
# accepted by the native TAP even before its MAC is known to us.
info "broadcast ping to ${OVERLAY_NET}.255 to trigger peer discovery"
ping -c 2 -t 2 -b "${OVERLAY_NET}.255" >"$TMP/bcast.log" 2>&1 || true
sleep 2

info "unicast ping from host to native peer ($NATIVE_IP)"
if ! ping -c 3 -t 5 "$NATIVE_IP" >"$TMP/ping-host.log" 2>&1; then
  cat "$TMP/ping-host.log" >&2
  fail "host -> native peer failed; ARP or peer discovery is broken"
fi
info "$(grep -o '[0-9][0-9]* packets received' "$TMP/ping-host.log" | head -1) (host -> native)"

info "unicast ping from native peer to host ($HOST_IP)"
if ! "$DOCKER" exec "$CONTAINER" ping -c 3 -W 2 "$HOST_IP" >"$TMP/ping-native.log" 2>&1; then
  if ! "$DOCKER" exec "$CONTAINER" ping -c 3 "$HOST_IP" >"$TMP/ping-native.log" 2>&1; then
    cat "$TMP/ping-native.log" >&2
    fail "native peer -> host failed; the shim did not answer ARP"
  fi
fi
info "$(grep -o '[0-9][0-9]* packets received' "$TMP/ping-native.log" | head -1) (native -> host)"

info "checking peer tables"
for name_mgmt in "our-edge:$MGMT_PORT"; do
  name=${name_mgmt%%:*}
  mgmt=${name_mgmt##*:}
  table="$(printf 'edges\n' | nc -u -w 2 127.0.0.1 "$mgmt" 2>/dev/null || true)"
  printf '%s\n' "$table"
  printf '%s' "$table" | grep -q "$NATIVE_IP" ||
    fail "$name does not list the native peer $NATIVE_IP"
done

# IPv6: utun is layer 3, so the shim answers neighbour solicitations for its
# link-local address on the peer's behalf.
NATIVE_LL="$("$DOCKER" exec "$CONTAINER" ip -6 addr show dev tap0 scope link 2>/dev/null |
  awk '/inet6 fe80/ {sub("/.*", "", $2); print $2; exit}')"
EDGE_DEV="$(sed -n 's/.*Native \(utun[0-9][0-9]*\) up and running.*/\1/p' "$EDGE_LOG" | head -1)"
if [ -z "$NATIVE_LL" ] || [ -z "$EDGE_DEV" ]; then
  info "note: skipping the IPv6 phase (native link-local '$NATIVE_LL', device '$EDGE_DEV')"
else
  info "IPv6 ping to native link-local $NATIVE_LL via $EDGE_DEV"
  if ! ping6 -c 3 -I "$EDGE_DEV" "$NATIVE_LL" >"$TMP/ping6.log" 2>&1; then
    cat "$TMP/ping6.log" >&2
    fail "IPv6 ping to the native peer failed; neighbour discovery or checksums are broken"
  fi
  info "$(grep -o '[0-9][0-9]* packets received' "$TMP/ping6.log" | head -1) (host -> native, IPv6)"
fi

info "interop test passed"
