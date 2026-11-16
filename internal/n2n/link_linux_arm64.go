//go:build linux && arm64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/linux/arm64/libn2n.a
import "C"
