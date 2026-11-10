GO        ?= go
N2N_SRC   ?= $(CURDIR)/third_party/n2n
NIX_SHELL ?= /nix/var/nix/profiles/default/bin/nix-shell

.PHONY: all n2n-lib n2n-lib-amd64 build build-amd64 build-all test vet check smoke clean shell

all: build

n2n-lib:
	N2N_SRC=$(N2N_SRC) ./scripts/build-n2n.sh arm64

n2n-lib-amd64:
	N2N_SRC=$(N2N_SRC) ./scripts/build-n2n.sh x86_64

build: n2n-lib
	CGO_ENABLED=1 $(GO) build -buildvcs=false -o bin/n2n-darwin-arm64 ./cmd/n2n

build-amd64: n2n-lib-amd64
	GOARCH=amd64 CGO_ENABLED=1 MACOSX_DEPLOYMENT_TARGET=11.0 \
	  $(GO) build -buildvcs=false -o bin/n2n-darwin-amd64 ./cmd/n2n

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

clean:
	rm -rf build bin third_party/lib third_party/n2n-src

shell:
	$(NIX_SHELL) shell.nix
