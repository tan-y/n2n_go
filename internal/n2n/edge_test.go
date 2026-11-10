//go:build darwin

package n2n

import (
	"os"
	"testing"

	"n2n-go/internal/config"
)

// TestNewEdgeWithoutRootFailsCleanly exercises the create path far enough to
// fail at tuntap_open and verifies the C layer cleans up without crashing.
func TestNewEdgeWithoutRootFailsCleanly(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("must run as a normal user")
	}
	c := config.DefaultEdgeConfig()
	c.Community = "demo"
	c.Supernodes = []string{"127.0.0.1:7777"}
	if err := c.SetAddress("10.0.0.1/32"); err != nil {
		t.Fatal(err)
	}
	e, err := NewEdge(&c)
	if err == nil {
		e.Close()
		t.Fatal("expected utun creation to fail without root")
	}
	t.Logf("create failed as expected: %v", err)
}
