//go:build darwin && arm64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/darwin/arm64/libn2n.a
import "C"
