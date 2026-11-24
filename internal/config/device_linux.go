package config

import (
	"fmt"
	"net"
)

// defaultDeviceName returns an empty name so the kernel allocates tapN.
func defaultDeviceName() string {
	return ""
}

// deviceNameMax mirrors IFNAMSIZ (devstr_t).
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

// validateMAC accepts any six-byte address on Linux.
func validateMAC(net.HardwareAddr) error { return nil }

// validateDeviceName accepts any Linux interface name up to IFNAMSIZ-1 bytes
// using the portable character set. An empty name means auto-allocation.
func validateDeviceName(name string) error {
	if name == "" {
		return nil
	}
	for _, r := range name {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') || r == '_' || r == '-' || r == '.' {
			continue
		}
		return fmt.Errorf("device name %q contains invalid character %q", name, r)
	}
	return nil
}
