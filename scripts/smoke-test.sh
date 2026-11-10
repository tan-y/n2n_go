#!/usr/bin/env bash
# Local smoke test: build the artifacts, start a supernode, start two edges and
# check that both register and can reach each other.
#
# Usage: ./scripts/smoke-test.sh
#        make smoke
#
# The edge processes need root to create utun devices. When this script is not
# run as root it prefixes the edge commands with sudo (and asks for a password
# once). The build steps still run as the invoking user.

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
EDGE1_IP="10.0.0.1"
EDGE2_IP="10.0.0.2"
# Random mgmt ports keep repeated runs from colliding with leftovers, a running
# production edge, or the supernode's own management port.
EDGE1_MGMT="${EDGE1_MGMT:-$((15000 + RANDOM % 500))}"
EDGE2_MGMT="${EDGE2_MGMT:-$((15600 + RANDOM % 300))}"
COMMUNITY="smoke"
KEY="smoke"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/n2n-smoke.XXXXXX")"
SN_LOG="$TMP/supernode.log"
EDGE1_LOG="$TMP/edge1.log"
EDGE2_LOG="$TMP/edge2.log"
PING_LOG="$TMP/ping.log"

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
    printf '\n--- supernode log ---\n' >&2
    tail -n 20 "$SN_LOG" 2>/dev/null >&2 || true
    printf '\n--- edge1 log ---\n' >&2
    tail -n 20 "$EDGE1_LOG" 2>/dev/null >&2 || true
    printf '\n--- edge2 log ---\n' >&2
    tail -n 20 "$EDGE2_LOG" 2>/dev/null >&2 || true
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
    fail "failed to build libn2n.a; run 'nix-shell shell.nix --run make smoke'"
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

# A leftover edge from an interrupted run keeps its virtual address registered
# on the supernode of that run. Ports are randomly chosen per run, so old edges
# cannot join this run's supernode; only stale test supernodes need cleaning.
info "stopping leftover test supernodes"
$SUDO_PREFIX pkill -f "$SN_BIN -f" >/dev/null 2>&1 || true
sleep 1

# ---------------------------------------------------------------- start supernode
info "starting supernode on port $SN_PORT"
# -f keeps the supernode in the foreground; without it n2n forks a daemon and
# the parent exits immediately, which would make this script think it crashed.
"$SN_BIN" -f -p "$SN_PORT" >"$SN_LOG" 2>&1 &
SN_PID=$!
sleep 1
process_alive "$SN_PID" || fail "supernode exited immediately"

# ---------------------------------------------------------------- start edges
start_edge() {
  local name=$1 ip=$2 mgmt=$3 log=$4
  info "starting $name ($ip/32, mgmt port $mgmt)"
  $SUDO_PREFIX "$BIN" edge \
    -c "$COMMUNITY" -k "$KEY" \
    -a "$ip/32" \
    -l "127.0.0.1:$SN_PORT" \
    -d utun -t "$mgmt" -M 1290 >"$log" 2>&1 &
  LAST_PID=$!
}

start_edge edge1 "$EDGE1_IP" "$EDGE1_MGMT" "$EDGE1_LOG"
EDGE1_PID="$LAST_PID"
start_edge edge2 "$EDGE2_IP" "$EDGE2_MGMT" "$EDGE2_LOG"
EDGE2_PID="$LAST_PID"

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

wait_for "$EDGE1_LOG" "Native utun" "edge1 utun creation" "$EDGE1_PID"
wait_for "$EDGE2_LOG" "Native utun" "edge2 utun creation" "$EDGE2_PID"
wait_for "$EDGE1_LOG" ">>> supernode" "edge1 supernode registration" "$EDGE1_PID"
wait_for "$EDGE2_LOG" ">>> supernode" "edge2 supernode registration" "$EDGE2_PID"

# ---------------------------------------------------------------- connectivity
EDGE1_NAME="$(sed -n 's/.*Native \(utun[0-9]*\) up.*/\1/p' "$EDGE1_LOG" | head -n1)"
EDGE2_NAME="$(sed -n 's/.*Native \(utun[0-9]*\) up.*/\1/p' "$EDGE2_LOG" | head -n1)"
[ -n "$EDGE1_NAME" ] || fail "could not determine the utun device created by edge1"
[ -n "$EDGE2_NAME" ] || fail "could not determine the utun device created by edge2"

# Received packets on an interface: the "inet" line is the one whose network
# column carries an address/prefix value.
ipkts() {
  netstat -ib -I "$1" 2>/dev/null | awk '$3 ~ /\// {print $5; exit}'
}

EDGE1_IPKTS_BEFORE="$(ipkts "$EDGE1_NAME")"
EDGE2_IPKTS_BEFORE="$(ipkts "$EDGE2_NAME")"

# Both edges run on this host, so the peer address is also a local address and
# macOS does not answer the ICMP request (see docs/macOS.md 6.2). The ping is
# therefore best effort; what is asserted below is that the packets still
# travelled over the n2n overlay.
info "pinging $EDGE1_IP and $EDGE2_IP (ICMP replies are not expected on one host)"
ping -c 3 -t 5 "$EDGE1_IP" >"$PING_LOG" 2>&1 || true
ping -c 3 -t 5 "$EDGE2_IP" >>"$PING_LOG" 2>&1 || true
grep -E "packet loss" "$PING_LOG" || true
sleep 1

EDGE1_IPKTS_AFTER="$(ipkts "$EDGE1_NAME")"
EDGE2_IPKTS_AFTER="$(ipkts "$EDGE2_NAME")"
info "overlay packets received: $EDGE1_NAME $EDGE1_IPKTS_BEFORE -> $EDGE1_IPKTS_AFTER, $EDGE2_NAME $EDGE2_IPKTS_BEFORE -> $EDGE2_IPKTS_AFTER"

# Each utun must have received packets that the other edge carried over the
# overlay. If both edges registered with the same MAC, the supernode treats
# them as one node and neither counter moves.
[ "${EDGE1_IPKTS_AFTER:-0}" -gt "${EDGE1_IPKTS_BEFORE:-0}" ] ||
  fail "$EDGE1_NAME received no traffic over the n2n overlay"
[ "${EDGE2_IPKTS_AFTER:-0}" -gt "${EDGE2_IPKTS_BEFORE:-0}" ] ||
  fail "$EDGE2_NAME received no traffic over the n2n overlay"

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
  [ -n "$name" ] || continue
  if ifconfig "$name" >/dev/null 2>&1; then
    fail "utun device $name still exists after shutdown"
  fi
done

info "smoke test passed"
