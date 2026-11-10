package config

import (
	"os"
	"path/filepath"
	"testing"
)

func writeTempConfig(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "edge.json")
	if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
		t.Fatalf("write config: %v", err)
	}
	return path
}

func TestLoadFileAppliesFields(t *testing.T) {
	path := writeTempConfig(t, `{
		"community": "spirit",
		"supernodes": ["wg.example.com:5356"],
		"address": "10.0.3.13",
		"key": "secret",
		"mtu": 1280,
		"mgmt_port": 5645,
		"drop_multicast": false
	}`)

	fc, err := LoadFile(path)
	if err != nil {
		t.Fatalf("LoadFile: %v", err)
	}

	c := DefaultEdgeConfig()
	fc.Apply(&c)

	if c.Community != "spirit" {
		t.Errorf("Community = %q", c.Community)
	}
	if len(c.Supernodes) != 1 || c.Supernodes[0] != "wg.example.com:5356" {
		t.Errorf("Supernodes = %v", c.Supernodes)
	}
	if c.CIDR != "10.0.3.13" {
		t.Errorf("CIDR = %q", c.CIDR)
	}
	if c.Key != "secret" {
		t.Errorf("Key = %q", c.Key)
	}
	if c.MTU != 1280 {
		t.Errorf("MTU = %d", c.MTU)
	}
	if c.MgmtPort != 5645 {
		t.Errorf("MgmtPort = %d", c.MgmtPort)
	}
	if c.DropMulticast {
		t.Error("DropMulticast should be disabled by the file")
	}
	if c.Device != "utun" {
		t.Errorf("Device = %q, omitted fields must keep their default", c.Device)
	}

	if err := c.Validate(); err != nil {
		t.Fatalf("Validate: %v", err)
	}
	if c.IP != "10.0.3.13" || c.Mask != "255.255.255.0" {
		t.Errorf("resolved address = %s/%s", c.IP, c.Mask)
	}
}

func TestLoadFileRejectsUnknownKey(t *testing.T) {
	path := writeTempConfig(t, `{"community": "spirit", "supernode": ["x:1"]}`)
	if _, err := LoadFile(path); err == nil {
		t.Fatal("unknown key must be rejected")
	}
}
