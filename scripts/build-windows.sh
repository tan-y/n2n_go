#!/usr/bin/env bash
# Build the Windows/amd64 Go binary and stage wintun.dll next to it.
#
# Run this inside an MSYS2 MinGW-w64 shell (it also builds libn2n.a):
#   ./scripts/build-windows.sh

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *) echo "请在 Windows 的 MSYS2 MinGW x64 shell 中构建 (run this script inside an MSYS2 MinGW-w64 shell)" >&2; exit 2 ;;
esac

DLL="${WINTUN_DLL:-$ROOT/third_party/wintun/amd64/wintun.dll}"
if [ ! -f "$DLL" ]; then
  cat >&2 <<EOF
wintun.dll not found: $DLL

Download Wintun 0.14.1 (or newer) from https://www.wintun.net/ and copy
amd64/wintun.dll to third_party/wintun/amd64/, or set WINTUN_DLL=/path/to/wintun.dll.
EOF
  exit 2
fi

if [ "${SKIP_N2N_LIB:-0}" != "1" ]; then
  bash "$ROOT/scripts/build-n2n-win.sh"
fi

# The requireAdministrator manifest is opt-in (EMBED_MANIFEST=1): embedding it
# makes every invocation, including `n2n version`, trigger a UAC prompt.
if [ "${EMBED_MANIFEST:-0}" = "1" ]; then
  # Go only picks up *.syso files in the package directory, so generate it
  # there and remove it afterwards.
  SYSO="$ROOT/cmd/n2n/rsrc_windows_amd64.syso"
  trap 'rm -f "$SYSO"' EXIT
  windres -I "$ROOT/platform/windows" "$ROOT/platform/windows/n2n.rc" -O coff -o "$SYSO"
fi

echo "==> building n2n-windows-amd64.exe"
cd "$ROOT"
export CGO_ENABLED=1 GOOS=windows GOARCH=amd64
GO_LDFLAGS='-linkmode external -extldflags "-static"'
go build -a -trimpath -buildvcs=false \
  -ldflags "$GO_LDFLAGS" \
  -o bin/n2n-windows-amd64.exe ./cmd/n2n

cp "$DLL" bin/wintun.dll
ls -l bin/n2n-windows-amd64.exe bin/wintun.dll
