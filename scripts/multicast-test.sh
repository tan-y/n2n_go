#!/usr/bin/env bash
# T1: verify IPv4 multicast/broadcast delivery over an n2n community.
#
# Topology (all on this host): one supernode + three edges, each edge creates a
# utun device. The script sends UDP datagrams to 239.255.42.42 through one
# edge's utun and asserts that the peer utun received them:
#
#   Phase A: edge1 <-> edge2, both started with -E (accept multicast).
#   Phase B: edge3 is started with the default drop setting. The macOS shim
#            emits a broadcast MAC for destinations it has no peer MAC for, and
#            n2n's drop_multicast only matches 01:00:5E/33:33 MACs, so the
#            frame is expected to arrive anyway. This documents the current
#            shim behavior, see docs/macOS-p5.md.
#
# Usage: sudo ./scripts/multicast-test.sh
#        make multicast
#
# Root is required to create utun devices and to modify the routing table.
# When not run as root the script prefixes privileged commands with sudo.

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
NC="/usr/bin/nc"
NETSTAT="$(command -v netstat || echo /usr/sbin/netstat)"

SN_PORT="${SN_PORT:-$((7000 + RANDOM % 1000))}"
EDGE1_IP="10.0.0.1"
EDGE2_IP="10.0.0.2"
EDGE3_IP="10.0.0.3"
EDGE1_MGMT="${EDGE1_MGMT:-$((15000 + RANDOM % 300))}"
EDGE2_MGMT="${EDGE2_MGMT:-$((15300 + RANDOM % 300))}"
EDGE3_MGMT="${EDGE3_MGMT:-$((15600 + RANDOM % 300))}"
COMMUNITY="multicast"
KEY="multicast"

GROUP="239.255.42.42"
GROUP_NET="239.255.42.0"
GROUP_MASK="255.255.255.0"
GROUP_PORT="${GROUP_PORT:-9999}"
MCAST_COUNT="${MCAST_COUNT:-4}"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/n2n-multicast.XXXXXX")"
SN_LOG="$TMP/supernode.log"
EDGE1_LOG="$TMP/edge1.log"
EDGE2_LOG="$TMP/edge2.log"
EDGE3_LOG="$TMP/edge3.log"

SN_PID=""
EDGE1_PID=""
EDGE2_PID=""
EDGE3_PID=""
SUDO_PREFIX=""
ROUTE_DEV=""
LAST_PID=""

info() { printf '==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

process_alive() {
  $SUDO_PREFIX kill -0 "$1" 2>/dev/null
}

del_route() {
  [ -n "$ROUTE_DEV" ] || return 0
  $SUDO_PREFIX /sbin/route -n delete -net "$GROUP_NET" -netmask "$GROUP_MASK" >/dev/null 2>&1 || true
  ROUTE_DEV=""
}

cleanup() {
  local rc=$? i alive pid
  del_route
  for pid in "$EDGE1_PID" "$EDGE2_PID" "$EDGE3_PID" "$SN_PID"; do
    [ -n "$pid" ] || continue
    $SUDO_PREFIX kill -INT "$pid" 2>/dev/null || kill -INT "$pid" 2>/dev/null || true
  done
  # The supernode checks keep_running only once per 10s select cycle.
  for i in $(seq 1 12); do
    alive=0
    for pid in "$EDGE1_PID" "$EDGE2_PID" "$EDGE3_PID" "$SN_PID"; do
      [ -n "$pid" ] || continue
      process_alive "$pid" && alive=1
    done
    [ "$alive" -eq 0 ] && break
    sleep 1
  done
  for pid in "$EDGE1_PID" "$EDGE2_PID" "$EDGE3_PID" "$SN_PID"; do
    [ -n "$pid" ] || continue
    process_alive "$pid" && $SUDO_PREFIX kill -KILL "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  if [ "$rc" -ne 0 ]; then
    for f in "$SN_LOG" "$EDGE1_LOG" "$EDGE2_LOG" "$EDGE3_LOG"; do
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
[ -x "$NC" ] || fail "$NC not found"

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
  local name=$1 ip=$2 mgmt=$3 log=$4
  shift 4
  info "starting $name ($ip/32, mgmt port $mgmt, extra args: $*)"
  $SUDO_PREFIX "$BIN" edge \
    -c "$COMMUNITY" -k "$KEY" \
    -a "$ip/32" \
    -l "127.0.0.1:$SN_PORT" \
    -d utun -t "$mgmt" -M 1290 "$@" >"$log" 2>&1 &
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

# Received packets on an interface. Row layout depends on whether the Address
# column is empty: with an address Ipkts is field 5, without it field 4.
ipkts() {
  "$NETSTAT" -ib -I "$1" 2>/dev/null |
    awk 'NR > 1 { if (NF >= 11) { print $5; exit } if (NF >= 10) { print $4; exit } }'
}

add_route() {
  local dev=$1
  if "$NETSTAT" -rn -f inet 2>/dev/null | awk '{print $1}' | grep -q "^239\.255\.42"; then
    fail "route $GROUP_NET/$GROUP_MASK already exists; refusing to modify it"
  fi
  $SUDO_PREFIX /sbin/route -n add -net "$GROUP_NET" -netmask "$GROUP_MASK" -interface "$dev" >/dev/null 2>&1 ||
    fail "failed to add multicast route via $dev"
  ROUTE_DEV="$dev"
}

send_group() {
  local dev=$1 i sent=0
  add_route "$dev"
  if ! /sbin/ifconfig "$dev" 2>/dev/null | grep -q MULTICAST; then
    info "warning: $dev does not report IFF_MULTICAST; multicast egress may fail"
  fi
  info "sending $MCAST_COUNT datagrams to $GROUP:$GROUP_PORT via $dev"
  for i in $(seq 1 "$MCAST_COUNT"); do
    if printf 'n2n-multicast-%s\n' "$i" | "$NC" -u -w 1 "$GROUP" "$GROUP_PORT" >/dev/null 2>&1; then
      sent=$((sent + 1))
    fi
  done
  [ "$sent" -gt 0 ] || fail "failed to send any multicast datagram via $dev"
  sleep 1
  del_route
}

assert_grew() {
  local name=$1 before=$2 after=$3 what=$4
  info "$name packets received: ${before:-0} -> ${after:-0} ($what)"
  [ "${after:-0}" -gt "${before:-0}" ] || fail "$name received no multicast traffic ($what)"
}

assert_unchanged() {
  local name=$1 before=$2 after=$3 what=$4
  info "$name packets received: ${before:-0} -> ${after:-0} ($what)"
  [ "${after:-0}" -eq "${before:-0}" ] || fail "$name unexpectedly received multicast traffic ($what)"
}

start_edge edge1 "$EDGE1_IP" "$EDGE1_MGMT" "$EDGE1_LOG" -E
EDGE1_PID="$LAST_PID"
start_edge edge2 "$EDGE2_IP" "$EDGE2_MGMT" "$EDGE2_LOG" -E
EDGE2_PID="$LAST_PID"
start_edge edge3 "$EDGE3_IP" "$EDGE3_MGMT" "$EDGE3_LOG" --drop-multicast
EDGE3_PID="$LAST_PID"

wait_for "$EDGE1_LOG" "Native utun" "edge1 utun creation" "$EDGE1_PID"
wait_for "$EDGE2_LOG" "Native utun" "edge2 utun creation" "$EDGE2_PID"
wait_for "$EDGE3_LOG" "Native utun" "edge3 utun creation" "$EDGE3_PID"
wait_for "$EDGE1_LOG" ">>> supernode" "edge1 supernode registration" "$EDGE1_PID"
wait_for "$EDGE2_LOG" ">>> supernode" "edge2 supernode registration" "$EDGE2_PID"
wait_for "$EDGE3_LOG" ">>> supernode" "edge3 supernode registration" "$EDGE3_PID"

EDGE1_NAME="$(utun_name "$EDGE1_LOG")"
EDGE2_NAME="$(utun_name "$EDGE2_LOG")"
EDGE3_NAME="$(utun_name "$EDGE3_LOG")"
[ -n "$EDGE1_NAME" ] || fail "could not determine the utun device created by edge1"
[ -n "$EDGE2_NAME" ] || fail "could not determine the utun device created by edge2"
[ -n "$EDGE3_NAME" ] || fail "could not determine the utun device created by edge3"
info "devices: edge1=$EDGE1_NAME edge2=$EDGE2_NAME edge3=$EDGE3_NAME"

# ---------------------------------------------------------------- phase A
E1_BEFORE="$(ipkts "$EDGE1_NAME")"
E2_BEFORE="$(ipkts "$EDGE2_NAME")"
E3_BEFORE="$(ipkts "$EDGE3_NAME")"

send_group "$EDGE1_NAME"

E1_AFTER="$(ipkts "$EDGE1_NAME")"
E2_AFTER="$(ipkts "$EDGE2_NAME")"
E3_AFTER="$(ipkts "$EDGE3_NAME")"

assert_grew "$EDGE2_NAME" "$E2_BEFORE" "$E2_AFTER" "edge1 -> edge2, both -E"

# The shim sends IPv4 multicast with a broadcast MAC, which n2n's
# drop_multicast filter (01:00:5E / 33:33) does not match.
assert_grew "$EDGE3_NAME" "$E3_BEFORE" "$E3_AFTER" "edge1 -> edge3 with default --drop-multicast"

# ---------------------------------------------------------------- phase B
E1_BEFORE="$(ipkts "$EDGE1_NAME")"
send_group "$EDGE2_NAME"
E1_AFTER="$(ipkts "$EDGE1_NAME")"
assert_grew "$EDGE1_NAME" "$E1_BEFORE" "$E1_AFTER" "edge2 -> edge1, both -E"

info "multicast test passed"
info "note: with the current broadcast-MAC shim, -E does not gate IPv4 multicast;"
info "      it only matters for real 01:00:5E/33:33 destination MACs (native peers, IPv6 ND)."
