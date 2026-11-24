package config

import (
	"fmt"
	"net"
)

// deviceNameMax returns the wintun adapter name limit (127 ASCII characters).
func deviceNameMax() int { return 128 }

// defaultDeviceName returns the adapter created when -d is empty.
func defaultDeviceName() string {
	return "n2n0"
}

// platformDefaultMAC leaves the MAC empty on Windows: the wintun shim derives
// it deterministically from the adapter name and community.
func platformDefaultMAC() (string, error) {
	return "", nil
}

// supportsAutoAddress reports whether -a auto (supernode-assigned address) is
// implemented. The wintun device layer only configures a static IPv4 address.
func supportsAutoAddress() bool { return false }

// validateMAC only accepts a unicast, non-zero address because the shim uses it
// as the source MAC for every synthesized Ethernet frame.
func validateMAC(mac net.HardwareAddr) error {
	allZero := true
	for _, b := range mac {
		if b != 0 {
			allZero = false
			break
		}
	}
	if allZero {
		return fmt.Errorf("invalid MAC %q: must not be zero", mac)
	}
	if mac[0]&0x01 != 0 {
		return fmt.Errorf("invalid MAC %q: must be a unicast address", mac)
	}
	return nil
}

// validateDeviceName accepts 1-127 ASCII characters from [A-Za-z0-9_.-].
func validateDeviceName(name string) error {
	if name == "" {
		return nil
	}
	for _, r := range name {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') || r == '_' || r == '.' || r == '-' {
			continue
		}
		return fmt.Errorf("device name %q contains invalid character %q", name, r)
	}
	return nil
}
