#!/usr/bin/env bash
# T3: verify the IPv6 read/write path of the macOS utun shim.
#
# Topology (all on this host): one supernode + three edges, each edge creates a
# utun device.
#
#   edge1, edge2: --ipv6 (which also forces -E, see config.Validate)
#   edge3:       -E but no --ipv6, so the shim itself must drop IPv6 frames
#
# Traffic is generated with an ICMPv6 echo to the all-nodes multicast group
# ff02::1, because a unicast IPv6 ping between two local addresses never enters
# a utun on a single host (docs/macOS.md 6.2). The group maps to 33:33:00:00:00:01
# per RFC 2464, which is what the shim must produce:
#
#   Phase A: edge1 -> edge2, both --ipv6, expect utun2 to receive the frames.
#   Phase B: edge1 -> edge3, IPv6 disabled, expect utun3 to stay unchanged.
#
# ICMPv6 checksums are completed in T4; the receiving kernel may discard the
# echo, but the packet still has to be written to the utun for Ipkts to grow.
#
# Usage: sudo ./scripts/ipv6-test.sh
#        make ipv6
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
NETSTAT="$(command -v netstat || echo /usr/sbin/netstat)"
PING6="$(command -v ping6 || echo /sbin/ping6)"

SN_PORT="${SN_PORT:-$((7000 + RANDOM % 1000))}"
EDGE1_IP="10.0.0.1"
EDGE2_IP="10.0.0.2"
EDGE3_IP="10.0.0.3"
EDGE1_MGMT="${EDGE1_MGMT:-$((15000 + RANDOM % 300))}"
EDGE2_MGMT="${EDGE2_MGMT:-$((15300 + RANDOM % 300))}"
EDGE3_MGMT="${EDGE3_MGMT:-$((15600 + RANDOM % 300))}"
COMMUNITY="ipv6"
KEY="ipv6"

ALL_NODES="ff02::1"
PING_COUNT="${PING_COUNT:-3}"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/n2n-ipv6.XXXXXX")"
SN_LOG="$TMP/supernode.log"
EDGE1_LOG="$TMP/edge1.log"
EDGE2_LOG="$TMP/edge2.log"
EDGE3_LOG="$TMP/edge3.log"

SN_PID=""
EDGE1_PID=""
EDGE2_PID=""
EDGE3_PID=""
SUDO_PREFIX=""
LAST_PID=""

info() { printf '==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

process_alive() {
  $SUDO_PREFIX kill -0 "$1" 2>/dev/null
}

cleanup() {
  local rc=$? i alive pid
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
[ -x "$PING6" ] || fail "$PING6 not found"

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

link_local() {
  /sbin/ifconfig "$1" 2>/dev/null | awk '/inet6 fe80/ { sub(/%.*/, "", $2); print $2; exit }'
}

# macOS adds its own host-wide link-local (same value on every interface), so
# prefer the EUI-64 address the shim reports; fall back to the first one.
shim_link_local() {
  sed -n 's/.*IPv6 link-local \([^ ]*\) on .*/\1/p' "$1" 2>/dev/null | head -n1
}

send_all_nodes() {
  local dev=$1
  info "sending $PING_COUNT ICMPv6 echoes to $ALL_NODES%$dev"
  "$PING6" -c "$PING_COUNT" "$ALL_NODES%$dev" >/dev/null 2>&1 || true
  sleep 1
}

start_edge edge1 "$EDGE1_IP" "$EDGE1_MGMT" "$EDGE1_LOG" --ipv6
EDGE1_PID="$LAST_PID"
start_edge edge2 "$EDGE2_IP" "$EDGE2_MGMT" "$EDGE2_LOG" --ipv6
EDGE2_PID="$LAST_PID"
start_edge edge3 "$EDGE3_IP" "$EDGE3_MGMT" "$EDGE3_LOG" -E
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

sleep 2 # allow duplicate address detection to settle on the link-local

EDGE1_LL="$(shim_link_local "$EDGE1_LOG")"
EDGE2_LL="$(shim_link_local "$EDGE2_LOG")"
if [ -z "$EDGE1_LL" ]; then EDGE1_LL="$(link_local "$EDGE1_NAME")"; fi
if [ -z "$EDGE2_LL" ]; then EDGE2_LL="$(link_local "$EDGE2_NAME")"; fi
[ -n "$EDGE1_LL" ] || fail "$EDGE1_NAME has no link-local IPv6 address; IPv6 is not up"
[ -n "$EDGE2_LL" ] || fail "$EDGE2_NAME has no link-local IPv6 address; IPv6 is not up"
[ "$EDGE1_LL" != "$EDGE2_LL" ] ||
  fail "both edges ended up with $EDGE1_LL; the shim must derive it from the device MAC"

/sbin/ifconfig "$EDGE1_NAME" 2>/dev/null | grep inet6 || true
/sbin/ifconfig "$EDGE2_NAME" 2>/dev/null | grep inet6 || true
info "link-local: edge1=$EDGE1_LL edge2=$EDGE2_LL"

# ---------------------------------------------------------------- phase A
E2_BEFORE="$(ipkts "$EDGE2_NAME")"
E3_BEFORE="$(ipkts "$EDGE3_NAME")"

send_all_nodes "$EDGE1_NAME"

E2_AFTER="$(ipkts "$EDGE2_NAME")"
E3_AFTER="$(ipkts "$EDGE3_NAME")"

info "$EDGE2_NAME packets received: ${E2_BEFORE:-0} -> ${E2_AFTER:-0} (edge1 -> edge2, both --ipv6)"
[ "${E2_AFTER:-0}" -gt "${E2_BEFORE:-0}" ] ||
  fail "$EDGE2_NAME received no IPv6 traffic from edge1"

# ---------------------------------------------------------------- phase B
info "$EDGE3_NAME packets received: ${E3_BEFORE:-0} -> ${E3_AFTER:-0} (edge3 has IPv6 disabled)"
[ "${E3_AFTER:-0}" -eq "${E3_BEFORE:-0}" ] ||
  fail "$EDGE3_NAME accepted IPv6 frames although --ipv6 is off"

grep -q "dropping IPv6 frame from the overlay" "$EDGE3_LOG" ||
  info "note: edge3 did not log the IPv6 drop (log level may hide it)"

info "IPv6 shim test passed"

# ---------------------------------------------------------------- phase C
# End-to-end check: the echo request must survive edge2's kernel, which only
# happens when the transport checksum has been completed by the shim.
info "pinging edge2 link-local ${EDGE2_LL} via ${EDGE1_NAME}"
if "$PING6" -c "$PING_COUNT" -I "$EDGE1_NAME" "$EDGE2_LL" >"$TMP/ping6.log" 2>&1; then
  info "ICMPv6 echo: $(grep -o '[0-9][0-9]* packets received' "$TMP/ping6.log" | head -1)"
  info "IPv6 end-to-end test passed"
else
  cat "$TMP/ping6.log"
  fail "no ICMPv6 echo reply from ${EDGE2_LL}; transport checksums are still broken"
fi
