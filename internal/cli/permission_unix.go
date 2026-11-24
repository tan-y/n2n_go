//go:build !windows

package cli

import (
	"errors"
	"os"
)

// checkPrivileges verifies the process may create a TUN/TAP device.
func checkPrivileges() error {
	if os.Geteuid() != 0 {
		return errors.New("creating a TUN/TAP device requires root or CAP_NET_ADMIN; run this command with sudo")
	}
	return nil
}
