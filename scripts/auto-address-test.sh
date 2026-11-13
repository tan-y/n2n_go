#!/usr/bin/env bash
# T2: verify -a auto (SN_ASSIGN) on macOS.
#
# Topology (all on this host): one supernode + two edges, both started with
# -a auto. Each edge opens its utun without an address, registers with the
# supernode, and applies the assigned address once the REGISTER_SUPER_ACK
# arrives. The script asserts that:
#
#   - both edges log an assigned address and the addresses differ;
#   - each utun really carries the assigned IPv4 address (ifconfig);
#   - each edge's peer table (management "edges") lists the other's address.
#
# Connectivity is not ping-tested: on a single host both assigned addresses are
# local, so ICMP never enters a utun (docs/macOS.md 6.2).
#
# Usage: sudo ./scripts/auto-address-test.sh
#        make auto-address
#
# Root is required to create utun devices.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="$(uname -m)"
case "$ARCH" in
  arm64)  GOARCH=arm64 ;;
  x86_64) GOARCH=amd64 ;;
  *) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

BIN="$ROOT/bin/n2n-darwin-$ARCH"
LIB="$ROOT/third_party/lib/darwin/$ARCH/libn2n.a"
WORK="$ROOT/build/n2n-$ARCH"
SN_BIN="$WORK/supernode"

SN_PORT="${SN_PORT:-$((7000 + RANDOM % 1000))}"
EDGE1_MGMT="${EDGE1_MGMT:-$((15000 + RANDOM % 300))}"
EDGE2_MGMT="${EDGE2_MGMT:-$((15300 + RANDOM % 300))}"
COMMUNITY="autoaddr"
KEY="autoaddr"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/n2n-auto.XXXXXX")"
SN_LOG="$TMP/supernode.log"
EDGE1_LOG="$TMP/edge1.log"
EDGE2_LOG="$TMP/edge2.log"

SN_PID=""
EDGE1_PID=""
EDGE2_PID=""
SUDO_PREFIX=""
LAST_PID=""

info() { printf '==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

process_alive() {
  $SUDO_PREFIX kill -0 "$1" 2>/dev/null
}

cleanup() {
  local rc=$? i alive pid
  if [ "${ROUTE_ADDED:-0}" -eq 1 ]; then
    $SUDO_PREFIX /sbin/route -n delete -host "${GROUP:-239.255.43.43}" >/dev/null 2>&1 || true
  fi
  for pid in "$EDGE1_PID" "$EDGE2_PID" "$SN_PID"; do
    [ -n "$pid" ] || continue
    $SUDO_PREFIX kill -INT "$pid" 2>/dev/null || kill -INT "$pid" 2>/dev/null || true
  done
  # The supernode checks keep_running only once per 10s select cycle.
  for i in $(seq 1 12); do
    alive=0
    for pid in "$EDGE1_PID" "$EDGE2_PID" "$SN_PID"; do
      [ -n "$pid" ] || continue
      process_alive "$pid" && alive=1
    done
    [ "$alive" -eq 0 ] && break
    sleep 1
  done
  for pid in "$EDGE1_PID" "$EDGE2_PID" "$SN_PID"; do
    [ -n "$pid" ] || continue
    process_alive "$pid" && $SUDO_PREFIX kill -KILL "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  if [ "$rc" -ne 0 ]; then
    for f in "$SN_LOG" "$EDGE1_LOG" "$EDGE2_LOG"; do
      [ -f "$f" ] || continue
      printf '\n--- %s ---\n' "$(basename "$f")" >&2
      tail -n 20 "$f" >&2 || true
    done
    printf '\nlogs kept in %s\n' "$TMP" >&2
  else
    rm -rf "$TMP"
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- prerequisites
if [ ! -f "$LIB" ]; then
  info "libn2n.a missing, building it (requires nix-shell)"
  N2N_SRC="${N2N_SRC:-$ROOT/third_party/n2n}" "$ROOT/scripts/build-n2n.sh" "$ARCH" ||
    fail "failed to build libn2n.a; run 'nix-shell shell.nix --run make build'"
fi

if [ ! -x "$BIN" ]; then
  info "building CLI: $BIN"
  ( cd "$ROOT" && CGO_ENABLED=1 GOARCH="$GOARCH" go build -o "$BIN" ./cmd/n2n ) ||
    fail "failed to build the CLI"
fi

if [ ! -x "$SN_BIN" ]; then
  info "building test supernode"
  make -C "$WORK" supernode >/dev/null 2>&1 || fail "failed to build the test supernode"
fi

SUDO_PREFIX=""
if [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null 2>&1 || fail "root is required to create utun devices"
  info "requesting sudo credentials for the edge processes"
  sudo -v || fail "sudo authentication failed"
  SUDO_PREFIX="sudo"
fi

info "stopping leftover test supernodes"
$SUDO_PREFIX pkill -f "$SN_BIN -f" >/dev/null 2>&1 || true
sleep 1

# ---------------------------------------------------------------- start supernode
info "starting supernode on port $SN_PORT"
"$SN_BIN" -f -p "$SN_PORT" >"$SN_LOG" 2>&1 &
SN_PID=$!
sleep 1
process_alive "$SN_PID" || fail "supernode exited immediately"

# ---------------------------------------------------------------- start edges
start_edge() {
  local name=$1 mgmt=$2 desc=$3 log=$4
  info "starting $name (-a auto, mgmt port $mgmt, description $desc)"
  $SUDO_PREFIX "$BIN" edge \
    -c "$COMMUNITY" -k "$KEY" \
    -a auto -I "$desc" \
    -l "127.0.0.1:$SN_PORT" \
    -d utun -t "$mgmt" -M 1290 >"$log" 2>&1 &
  LAST_PID=$!
}

wait_for() {
  local file=$1 pattern=$2 what=$3 pid=$4
  local i
  for i in $(seq 1 30); do
    if grep -q "$pattern" "$file" 2>/dev/null; then
      return 0
    fi
    process_alive "$pid" || fail "$what process exited early (see $file)"
    sleep 1
  done
  fail "timed out waiting for $what"
}

utun_name() {
  sed -n 's/.*Native \(utun[0-9]*\) up.*/\1/p' "$1" | head -n1
}

assigned_ip() {
  sed -n 's/.*supernode assigned address \([0-9.]*\)\/.*/\1/p' "$1" | tail -n1
}

mgmt_query() {
  local port=$1 cmd=$2
  printf '%s\n' "$cmd" | nc -u -w 1 127.0.0.1 "$port" 2>/dev/null || true
}

start_edge edge1 "$EDGE1_MGMT" edge1 "$EDGE1_LOG"
EDGE1_PID="$LAST_PID"
start_edge edge2 "$EDGE2_MGMT" edge2 "$EDGE2_LOG"
EDGE2_PID="$LAST_PID"

wait_for "$EDGE1_LOG" "Native utun" "edge1 utun creation" "$EDGE1_PID"
wait_for "$EDGE2_LOG" "Native utun" "edge2 utun creation" "$EDGE2_PID"
wait_for "$EDGE1_LOG" "supernode assigned address" "edge1 address assignment" "$EDGE1_PID"
wait_for "$EDGE2_LOG" "supernode assigned address" "edge2 address assignment" "$EDGE2_PID"

EDGE1_NAME="$(utun_name "$EDGE1_LOG")"
EDGE2_NAME="$(utun_name "$EDGE2_LOG")"
EDGE1_ADDR="$(assigned_ip "$EDGE1_LOG")"
EDGE2_ADDR="$(assigned_ip "$EDGE2_LOG")"

[ -n "$EDGE1_NAME" ] || fail "could not determine the utun device created by edge1"
[ -n "$EDGE2_NAME" ] || fail "could not determine the utun device created by edge2"
[ -n "$EDGE1_ADDR" ] || fail "edge1 did not report an assigned address"
[ -n "$EDGE2_ADDR" ] || fail "edge2 did not report an assigned address"
info "assigned: edge1=$EDGE1_ADDR ($EDGE1_NAME) edge2=$EDGE2_ADDR ($EDGE2_NAME)"

[ "$EDGE1_ADDR" != "$EDGE2_ADDR" ] ||
  fail "both edges received the same address $EDGE1_ADDR; the supernode hashes the edge description and does not record its own assignment, so simultaneous registrations share the proposal — give each edge a distinct -I description"

for pair in "$EDGE1_NAME:$EDGE1_ADDR" "$EDGE2_NAME:$EDGE2_ADDR"; do
  dev=${pair%%:*}
  addr=${pair##*:}
  ifconfig "$dev" 2>/dev/null | grep -q "inet $addr " ||
    fail "$dev does not carry the assigned address $addr"
done
info "both utun devices carry their assigned address"

# ---------------------------------------------------------------- connectivity
# A unicast or subnet-broadcast ping cannot validate the overlay here: both
# assigned addresses are local to this machine, so the packets are answered or
# absorbed by the local stack instead of entering a utun (docs/macOS.md 6.2).
# A multicast group with an explicit host route is not local and, as verified
# in T1, is relayed through the supernode. Peers in n2n are learned from such
# traffic (there is no ARP on this L3 shim), so afterwards each edge must list
# the other one, including its assigned address, in its peer table.
info "waiting for both edges to settle after re-registration"
sleep 3

GROUP="239.255.43.43"
GROUP_PORT=9999
ROUTE_ADDED=0

send_multicast() {
  local dev=$1 i
  $SUDO_PREFIX /sbin/route -n add -host "$GROUP" -interface "$dev" >/dev/null 2>&1 ||
    fail "failed to add the multicast route via $dev"
  ROUTE_ADDED=1
  info "sending 4 datagrams to $GROUP:$GROUP_PORT via $dev"
  for i in 1 2 3 4; do
    printf 'n2n-auto-%s\n' "$i" | nc -u -w 1 "$GROUP" "$GROUP_PORT" >/dev/null 2>&1 || true
  done
  $SUDO_PREFIX /sbin/route -n delete -host "$GROUP" >/dev/null 2>&1 || true
  ROUTE_ADDED=0
}

send_multicast "$EDGE1_NAME"

# Poll for traffic-driven peer discovery; the receiving edge registers the
# sender directly and both sides end up with the other's assigned address.
wait_for_peer() {
  local port=$1 want=$2 name=$3 out i
  for i in $(seq 1 12); do
    out="$(mgmt_query "$port" edges)"
    if printf '%s' "$out" | grep -q "$want"; then
      info "$name sees $want"
      return 0
    fi
    sleep 1
  done
  printf '\n--- %s peer table ---\n%s\n' "$name" "$out" >&2
  return 1
}

wait_for_peer "$EDGE1_MGMT" "$EDGE2_ADDR" edge1 ||
  fail "edge1 does not list edge2 ($EDGE2_ADDR) among its peers"
wait_for_peer "$EDGE2_MGMT" "$EDGE1_ADDR" edge2 ||
  fail "edge2 does not list edge1 ($EDGE1_ADDR) among its peers"
info "each edge discovered the other edge's assigned address"

info "stopping edges and checking exit status"
$SUDO_PREFIX kill -INT "$EDGE1_PID" "$EDGE2_PID" 2>/dev/null || true
EDGE1_RC=0
EDGE2_RC=0
wait "$EDGE1_PID" || EDGE1_RC=$?
wait "$EDGE2_PID" || EDGE2_RC=$?
EDGE1_PID=""
EDGE2_PID=""
[ "$EDGE1_RC" -eq 0 ] || fail "edge1 exited with status $EDGE1_RC"
[ "$EDGE2_RC" -eq 0 ] || fail "edge2 exited with status $EDGE2_RC"

info "checking utun cleanup"
for name in "$EDGE1_NAME" "$EDGE2_NAME"; do
  if ifconfig "$name" >/dev/null 2>&1; then
    fail "utun device $name still exists after shutdown"
  fi
done

info "auto-address test passed"
