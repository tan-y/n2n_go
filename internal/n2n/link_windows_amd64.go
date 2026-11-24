//go:build windows && amd64

package n2n

/*
#cgo CFLAGS: -D_WIN32_WINNT=0x0601 -I${SRCDIR}/../../platform/windows
#cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/windows/amd64/libn2n.a -lnetapi32 -lws2_32 -liphlpapi
*/
import "C"
