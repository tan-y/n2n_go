#!/usr/bin/env bash
# Install (or remove) the n2n edge as a macOS LaunchDaemon.
#
# Usage (must run as root):
#   sudo ./scripts/install-service.sh --config /path/to/edge.json
#   sudo ./scripts/install-service.sh --uninstall
#
# Optional flags:
#   --binary <path>   edge binary to install (default: bin/n2n-darwin-<arch>)
#   --label <name>    launchd label (default: com.n2n-go.edge)
#
# The daemon runs as root in the background and is restarted on crash. Edge
# arguments live in the JSON config; command-line flags are not used here.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="$(uname -m)"
case "$ARCH" in
  arm64)  BIN="$ROOT/bin/n2n-darwin-arm64" ;;
  x86_64) BIN="$ROOT/bin/n2n-darwin-amd64" ;;
  *) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

LABEL="com.n2n-go.edge"
CONFIG=""
UNINSTALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --config)   CONFIG="$2"; shift 2 ;;
    --binary)   BIN="$2"; shift 2 ;;
    --label)    LABEL="$2"; shift 2 ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" -ne 0 ]; then
  echo "this script must run as root: sudo $0 $*" >&2
  exit 1
fi

PLIST="/Library/LaunchDaemons/$LABEL.plist"
INSTALL_BIN="/usr/local/bin/n2n-go-edge"
INSTALL_CONFIG_DIR="/Library/Application Support/n2n-go"
INSTALL_CONFIG="$INSTALL_CONFIG_DIR/edge.json"
LOG="/var/log/n2n-go-edge.log"

stop_daemon() {
  launchctl bootout "system/$LABEL" >/dev/null 2>&1 || true
}

if [ "$UNINSTALL" -eq 1 ]; then
  stop_daemon
  rm -f "$PLIST" "$INSTALL_BIN"
  echo "removed $LABEL (config kept at $INSTALL_CONFIG, log at $LOG)"
  exit 0
fi

if [ -z "$CONFIG" ]; then
  echo "--config is required (a JSON edge config, see docs/macOS.md)" >&2
  exit 2
fi
[ -f "$CONFIG" ] || { echo "config not found: $CONFIG" >&2; exit 2; }
[ -x "$BIN" ] || { echo "binary not found: $BIN (run make build first)" >&2; exit 2; }

mkdir -p "$(dirname "$INSTALL_BIN")" "$INSTALL_CONFIG_DIR"
cp "$BIN" "$INSTALL_BIN"
cp "$CONFIG" "$INSTALL_CONFIG"
chmod 755 "$INSTALL_BIN"
chmod 644 "$INSTALL_CONFIG"

stop_daemon

cat >"$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$INSTALL_BIN</string>
    <string>edge</string>
    <string>-config</string>
    <string>$INSTALL_CONFIG</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>ThrottleInterval</key>
  <integer>10</integer>
  <key>ProcessType</key>
  <string>Background</string>
  <key>StandardOutPath</key>
  <string>$LOG</string>
  <key>StandardErrorPath</key>
  <string>$LOG</string>
</dict>
</plist>
EOF

chown root:wheel "$PLIST"
chmod 644 "$PLIST"

launchctl bootstrap system "$PLIST"
echo "installed $LABEL"
echo "  binary : $INSTALL_BIN"
echo "  config : $INSTALL_CONFIG"
echo "  log    : $LOG"
echo "  status : sudo launchctl print system/$LABEL | head -n 20"
