GO        ?= go
N2N_SRC   ?= $(CURDIR)/third_party/n2n
NIX_SHELL ?= /nix/var/nix/profiles/default/bin/nix-shell

.PHONY: all n2n-lib n2n-lib-amd64 build build-amd64 build-all test vet check smoke multicast auto-address ipv6 interop clean shell

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

clean:
	rm -rf build bin third_party/lib third_party/n2n-src

shell:
	$(NIX_SHELL) shell.nix
