package config

import (
	"fmt"
	"net"
	"strconv"
	"strings"
)

// defaultDeviceName returns the macOS default: utun, let the kernel assign utunN.
func defaultDeviceName() string {
	return "utun"
}

// deviceNameMax mirrors N2N_IFNAMSIZ (devstr_t).
func deviceNameMax() int { return 16 }

// supportsAutoAddress reports whether -a auto (supernode-assigned address) is
// implemented.
func supportsAutoAddress() bool { return true }

// platformDefaultMAC draws a random locally administered address.
func platformDefaultMAC() (string, error) {
	mac, err := randomMAC()
	if err != nil {
		return "", err
	}
	return mac.String(), nil
}

// validateMAC accepts any six-byte address on macOS.
func validateMAC(net.HardwareAddr) error { return nil }

// validateDeviceName enforces macOS's utun / utunN naming rule.
func validateDeviceName(name string) error {
	if !isUTUNName(name) {
		return fmt.Errorf("device name %q is not supported: macOS 首期仅支持 utun 或 utunN", name)
	}
	return nil
}

func isUTUNName(name string) bool {
	if name == "utun" {
		return true
	}
	if !strings.HasPrefix(name, "utun") {
		return false
	}
	_, err := strconv.Atoi(name[4:])
	return err == nil
}
