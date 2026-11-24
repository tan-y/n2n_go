GO        ?= go
N2N_SRC   ?= $(CURDIR)/third_party/n2n
NIX_SHELL ?= /nix/var/nix/profiles/default/bin/nix-shell

.PHONY: all n2n-lib n2n-lib-amd64 build build-amd64 build-all test vet check smoke multicast auto-address ipv6 interop linux-arm64 linux-arm64-glibc linux-amd64 linux-amd64-glibc linux-smoke linux-smoke-glibc linux-clean n2n-lib-win win win-smoke win-clean clean shell

all: build

n2n-lib:
	N2N_SRC=$(N2N_SRC) ./scripts/build-n2n.sh arm64

n2n-lib-amd64:
	N2N_SRC=$(N2N_SRC) ./scripts/build-n2n.sh x86_64

# -a forces a re-link: cgo does not hash the contents of libn2n.a, so a rebuilt
# static library would otherwise be ignored while the Go build cache stays warm.
build: n2n-lib
	CGO_ENABLED=1 $(GO) build -a -buildvcs=false -o bin/n2n-darwin-arm64 ./cmd/n2n

build-amd64: n2n-lib-amd64
	GOARCH=amd64 CGO_ENABLED=1 MACOSX_DEPLOYMENT_TARGET=11.0 \
	  $(GO) build -a -buildvcs=false -o bin/n2n-darwin-amd64 ./cmd/n2n

build-all: build build-amd64

test:
	CGO_ENABLED=1 $(GO) test ./...

vet:
	CGO_ENABLED=1 $(GO) vet ./...

check:
	@test -z "$(shell gofmt -l internal cmd)" || { echo "gofmt needed: $(shell gofmt -l internal cmd)"; exit 1; }
	CGO_ENABLED=1 $(GO) vet ./...
	CGO_ENABLED=1 $(GO) test ./...

smoke: build
	./scripts/smoke-test.sh

multicast:
	./scripts/multicast-test.sh

auto-address:
	./scripts/auto-address-test.sh

ipv6:
	./scripts/ipv6-test.sh

interop:
	./scripts/interop-test.sh

linux-arm64:
	./scripts/build-linux.sh arm64 musl

linux-arm64-glibc:
	./scripts/build-linux.sh arm64 glibc

linux-amd64:
	./scripts/build-linux.sh amd64 musl

linux-amd64-glibc:
	./scripts/build-linux.sh amd64 glibc

linux-smoke:
	./scripts/smoke-test-linux.sh

linux-smoke-glibc:
	./scripts/smoke-test-linux.sh glibc

linux-clean:
	rm -rf build/n2n-linux-* third_party/lib/linux bin/n2n-linux-* bin/supernode-linux-*

# Windows targets must run inside an MSYS2 MinGW-w64 shell (see docs/Windows.md).
n2n-lib-win:
	N2N_SRC=$(N2N_SRC) bash ./scripts/build-n2n-win.sh

win: n2n-lib-win
	SKIP_N2N_LIB=1 bash ./scripts/build-windows.sh

win-smoke:
	powershell -ExecutionPolicy Bypass -File scripts/smoke-test-windows.ps1

win-clean:
	rm -rf build/n2n-windows-* third_party/lib/windows bin/n2n-windows-*.exe bin/wintun.dll

clean:
	rm -rf build bin third_party/lib third_party/n2n-src

shell:
	$(NIX_SHELL) shell.nix
