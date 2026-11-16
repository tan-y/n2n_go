//go:build linux && amd64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/linux/amd64/libn2n.a
import "C"
