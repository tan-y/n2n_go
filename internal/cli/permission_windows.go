//go:build windows

package cli

import (
	"errors"
	"syscall"
)

// checkPrivileges verifies the process is elevated. Creating a wintun adapter
// and configuring its address both require Administrator rights.
func checkPrivileges() error {
	isUserAnAdmin := syscall.NewLazyDLL("shell32.dll").NewProc("IsUserAnAdmin")
	ok, _, _ := isUserAnAdmin.Call()
	if ok == 0 {
		return errors.New("creating a wintun adapter requires Administrator privileges; run this command from an elevated terminal")
	}
	return nil
}
