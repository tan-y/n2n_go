package config

import "fmt"

// defaultDeviceName returns an empty name so the kernel allocates tapN.
func defaultDeviceName() string {
	return ""
}

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
