// Package n2n binds n2n 3.1.1's edge library through a small C adapter.
// One Edge maps to one utun device and one edge loop.
package n2n

/*
#cgo CFLAGS: -I${SRCDIR}/../../third_party/n2n-src/include -I${SRCDIR} -DSKIP_MULTICAST_PEERS_DISCOVERY
#include <stdlib.h>
#include "edge_glue.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"runtime/cgo"
	"sync"
	"unsafe"

	"n2n-go/internal/config"
)

// Edge is a running (or ready to run) n2n edge instance.
type Edge struct {
	handle *C.n2n_go_edge_t
	h      cgo.Handle

	started     chan struct{}
	startedOnce sync.Once
	closeOnce   sync.Once
}

// NewEdge initializes n2n and opens the utun device. It requires root on macOS.
func NewEdge(cfg *config.EdgeConfig) (*Edge, error) {
	if cfg == nil {
		return nil, errors.New("n2n: nil config")
	}
	if err := cfg.Validate(); err != nil {
		return nil, err
	}

	transopID, err := cfg.CipherID()
	if err != nil {
		return nil, err
	}
	compressionID, err := cfg.CompressionID()
	if err != nil {
		return nil, err
	}

	e := &Edge{started: make(chan struct{})}
	e.h = cgo.NewHandle(e)

	community := C.CString(cfg.Community)
	defer C.free(unsafe.Pointer(community))

	var key *C.char
	if cfg.Key != "" {
		key = C.CString(cfg.Key)
		defer C.free(unsafe.Pointer(key))
	}

	ip := C.CString(cfg.IP)
	defer C.free(unsafe.Pointer(ip))

	mask := C.CString(cfg.Mask)
	defer C.free(unsafe.Pointer(mask))

	device := C.CString(cfg.Device)
	defer C.free(unsafe.Pointer(device))

	var mac *C.char
	if cfg.MAC != "" {
		mac = C.CString(cfg.MAC)
		defer C.free(unsafe.Pointer(mac))
	}

	desc := C.CString(cfg.Description)
	defer C.free(unsafe.Pointer(desc))

	var fedPub, authPw *C.char
	if cfg.FederationPublicKey != "" {
		fedPub = C.CString(cfg.FederationPublicKey)
		defer C.free(unsafe.Pointer(fedPub))
	}
	if cfg.AuthPassword != "" {
		authPw = C.CString(cfg.AuthPassword)
		defer C.free(unsafe.Pointer(authPw))
	}

	// Validate() guarantees at least one supernode. The pointer array must live
	// in C memory: passing a Go slice of Go pointers to C is rejected by cgo.
	sn := make([]*C.char, len(cfg.Supernodes))
	for i, s := range cfg.Supernodes {
		sn[i] = C.CString(s)
	}
	defer func() {
		for _, p := range sn {
			C.free(unsafe.Pointer(p))
		}
	}()

	snArr := C.malloc(C.size_t(len(sn)) * C.size_t(unsafe.Sizeof(uintptr(0))))
	if snArr == nil {
		e.h.Delete()
		return nil, errors.New("n2n: out of memory allocating the supernode list")
	}
	defer C.free(snArr)
	snPtrs := unsafe.Slice((**C.char)(snArr), len(sn))
	for i := range sn {
		snPtrs[i] = sn[i]
	}

	var ccfg C.n2n_go_config_t
	ccfg.community = community
	ccfg.key = key
	ccfg.supernodes = (**C.char)(snArr)
	ccfg.sn_num = C.int(len(sn))
	ccfg.ip = ip
	ccfg.mask = mask
	if cfg.AutoAddress {
		ccfg.auto_address = 1
	}
	if cfg.IPv6 {
		ccfg.ipv6 = 1
	}
	ccfg.device = device
	ccfg.mac = mac
	ccfg.mtu = C.int(cfg.MTU)
	ccfg.transop_id = C.int(transopID)
	ccfg.compression = C.int(compressionID)
	ccfg.local_port = C.int(cfg.LocalPort)
	ccfg.mgmt_port = C.int(cfg.MgmtPort)
	ccfg.register_interval = C.int(cfg.RegisterInterval)
	ccfg.register_ttl = C.int(cfg.RegisterTTL)
	if cfg.AllowRouting {
		ccfg.allow_routing = 1
	}
	if cfg.DropMulticast {
		ccfg.drop_multicast = 1
	}
	if cfg.DisablePMTUIDiscovery {
		ccfg.disable_pmtu_discovery = 1
	}
	ccfg.tos = C.int(cfg.TOS)
	ccfg.description = desc
	ccfg.federation_public_key = fedPub
	ccfg.auth_password = authPw
	if cfg.HeaderEncryption {
		ccfg.header_encryption = 1
	}
	ccfg.started_handle = C.uintptr_t(e.h)

	var errBuf [512]C.char
	handle := C.n2n_go_edge_create(&ccfg, &errBuf[0], C.int(len(errBuf)))
	if handle == nil {
		e.h.Delete()
		msg := C.GoString(&errBuf[0])
		if msg == "" {
			msg = "edge_create failed"
		}
		return nil, errors.New(msg)
	}
	e.handle = handle
	return e, nil
}

// Run blocks until the edge loop stops. It performs the n2n cleanup internally,
// so Close must be called afterwards to release the handle.
func (e *Edge) Run() error {
	if e == nil || e.handle == nil {
		return errors.New("n2n: edge is closed")
	}
	if rc := C.n2n_go_edge_run(e.handle); rc != 0 {
		return fmt.Errorf("n2n: edge loop exited with code %d", int(rc))
	}
	return nil
}

// DeviceName returns the device name actually opened by the kernel (for Linux
// this may be an auto-assigned tapN), or "" if the edge is not open.
func (e *Edge) DeviceName() string {
	if e == nil || e.handle == nil {
		return ""
	}
	name := C.n2n_go_edge_device_name(e.handle)
	if name == nil {
		return ""
	}
	return C.GoString(name)
}

// Stop asks the edge loop to exit. Safe to call before Run.
func (e *Edge) Stop() {
	if e == nil || e.handle == nil {
		return
	}
	C.n2n_go_edge_stop(e.handle)
}

// ConfigureAuto applies the address assigned by the supernode to the utun
// device. It returns done=true once the device is configured (or when there is
// nothing left to configure because the edge loop has finished) and done=false
// while the assignment has not arrived yet. Call it repeatedly while Run is
// executing; it is a no-op when the edge uses a static address.
func (e *Edge) ConfigureAuto() (bool, error) {
	if e == nil || e.handle == nil {
		return false, errors.New("n2n: edge is closed")
	}
	switch rc := int(C.n2n_go_edge_configure_auto(e.handle)); rc {
	case 0:
		return false, nil
	case 1:
		return true, nil
	default:
		return false, errors.New("n2n: failed to apply the supernode-assigned address")
	}
}

// Started is closed once the C layer is about to enter run_edge_loop.
func (e *Edge) Started() <-chan struct{} {
	if e == nil {
		return nil
	}
	return e.started
}

// Close releases the C handle. Call it after Run has returned.
func (e *Edge) Close() {
	if e == nil {
		return
	}
	e.closeOnce.Do(func() {
		if e.handle != nil {
			C.n2n_go_edge_destroy(e.handle)
			e.handle = nil
		}
		e.h.Delete()
	})
}

// Version returns the n2n version the static library was built from.
func Version() string {
	return C.GoString(C.n2n_go_version())
}

//export n2nGoOnStarted
func n2nGoOnStarted(handle C.uintptr_t) {
	if v := cgo.Handle(handle).Value(); v != nil {
		if e, ok := v.(*Edge); ok {
			e.startedOnce.Do(func() { close(e.started) })
		}
	}
}
