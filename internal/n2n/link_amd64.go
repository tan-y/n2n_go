//go:build darwin && amd64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/darwin/x86_64/libn2n.a
import "C"
