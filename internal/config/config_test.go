package config

import (
	"strings"
	"testing"
)

func validConfig(t *testing.T) EdgeConfig {
	t.Helper()
	c := DefaultEdgeConfig()
	c.Community = "demo"
	c.Supernodes = []string{"127.0.0.1:7777"}
	if err := c.SetAddress("10.0.0.1/32"); err != nil {
		t.Fatalf("SetAddress: %v", err)
	}
	return c
}

func TestValidateOK(t *testing.T) {
	c := validConfig(t)
	if err := c.Validate(); err != nil {
		t.Fatalf("Validate: %v", err)
	}
	if c.Cipher != "none" {
		t.Fatalf("cipher default = %q, want none", c.Cipher)
	}
	if id, _ := c.CipherID(); id != TransformNull {
		t.Fatalf("CipherID = %d, want %d", id, TransformNull)
	}
}

func TestSetAddress(t *testing.T) {
	c := DefaultEdgeConfig()
	if err := c.SetAddress("10.0.0.1/24"); err != nil {
		t.Fatalf("SetAddress: %v", err)
	}
	if c.IP != "10.0.0.1" || c.Mask != "255.255.255.0" {
		t.Fatalf("got ip=%q mask=%q", c.IP, c.Mask)
	}
	if err := c.SetAddress("10.0.0.1/32"); err != nil {
		t.Fatalf("SetAddress /32: %v", err)
	}
	if c.Mask != "255.255.255.255" {
		t.Fatalf("mask = %q, want 255.255.255.255", c.Mask)
	}
}

func TestSetAddressRejectsIPv6AndGarbage(t *testing.T) {
	c := DefaultEdgeConfig()
	if err := c.SetAddress("[::1]/128"); err == nil {
		t.Fatal("expected IPv6 to be rejected")
	}
	if err := c.SetAddress("nonsense"); err == nil {
		t.Fatal("expected garbage address to be rejected")
	}
}

func TestSetAddressBareIPDefaultsToSlash24(t *testing.T) {
	c := DefaultEdgeConfig()
	if err := c.SetAddress("10.0.3.13"); err != nil {
		t.Fatalf("SetAddress: %v", err)
	}
	if c.IP != "10.0.3.13" || c.Mask != "255.255.255.0" || c.CIDR != "10.0.3.13/24" {
		t.Fatalf("got ip=%q mask=%q cidr=%q", c.IP, c.Mask, c.CIDR)
	}
}

func TestHeaderEncryptionRequiresKey(t *testing.T) {
	c := DefaultEdgeConfig()
	c.Community = "demo"
	c.Supernodes = []string{"127.0.0.1:7777"}
	c.CIDR = "10.0.0.1/24"
	c.HeaderEncryption = true

	if err := c.Validate(); err == nil {
		t.Fatal("header encryption without a key must be rejected")
	}

	c.Key = "secret"
	c.Cipher = "" // let Validate resolve it again after the first failed pass
	if err := c.Validate(); err != nil {
		t.Fatalf("header encryption with a key: %v", err)
	}
}

func TestValidateCommunityLength(t *testing.T) {
	c := validConfig(t)
	c.Community = strings.Repeat("a", CommunityMax+1)
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "too long") {
		t.Fatalf("expected community length error, got %v", err)
	}
	c.Community = strings.Repeat("a", CommunityMax)
	if err := c.Validate(); err != nil {
		t.Fatalf("%d-byte community should be valid: %v", CommunityMax, err)
	}
}

func TestValidateKeyAndCipher(t *testing.T) {
	c := validConfig(t)
	c.Key = "secret"
	if err := c.Validate(); err != nil {
		t.Fatalf("key without cipher should default to aes: %v", err)
	}
	if c.Cipher != "aes" {
		t.Fatalf("cipher = %q, want aes", c.Cipher)
	}

	c = validConfig(t)
	c.Cipher = "aes"
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "requires") {
		t.Fatalf("expected missing key error, got %v", err)
	}

	c = validConfig(t)
	c.Key = "secret"
	c.Cipher = "none"
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "does not accept") {
		t.Fatalf("expected cipher none + key error, got %v", err)
	}

	c = validConfig(t)
	c.Key = "secret"
	c.Cipher = "cc20"
	if err := c.Validate(); err != nil {
		t.Fatalf("cc20 with key: %v", err)
	}
	if id, _ := c.CipherID(); id != TransformChaCha20 {
		t.Fatalf("cc20 id = %d", id)
	}
}

func TestValidateSupernodeAndMTU(t *testing.T) {
	c := validConfig(t)
	c.Supernodes = []string{"127.0.0.1"}
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "host:port") {
		t.Fatalf("expected supernode error, got %v", err)
	}

	c = validConfig(t)
	c.MTU = 1600
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "MTU") {
		t.Fatalf("expected MTU error, got %v", err)
	}
}

func TestValidateDevice(t *testing.T) {
	c := validConfig(t)
	c.Device = "tap0"
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "utun") {
		t.Fatalf("expected utun-only error, got %v", err)
	}
	c.Device = "utun7"
	if err := c.Validate(); err != nil {
		t.Fatalf("utun7 should be valid: %v", err)
	}
}

func TestValidateDescription(t *testing.T) {
	c := validConfig(t)
	c.Description = strings.Repeat("d", DescMax+1)
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "description") {
		t.Fatalf("expected description error, got %v", err)
	}
}

func TestValidateAutoAddress(t *testing.T) {
	for _, value := range []string{"auto", "AUTO", " Auto "} {
		c := DefaultEdgeConfig()
		c.Community = "demo"
		c.Supernodes = []string{"127.0.0.1:7777"}
		c.CIDR = value
		if err := c.Validate(); err != nil {
			t.Fatalf("Validate(%q): %v", value, err)
		}
		if !c.AutoAddress {
			t.Fatalf("Validate(%q): AutoAddress = false, want true", value)
		}
		if c.IP != "" || c.Mask != "" {
			t.Fatalf("Validate(%q): ip=%q mask=%q, want empty", value, c.IP, c.Mask)
		}
		if c.CIDR != "auto" {
			t.Fatalf("Validate(%q): CIDR = %q, want auto", value, c.CIDR)
		}
	}
}

// A config file may carry a static address that -a auto overrides; the parsed
// IP and mask must not leak into SN_ASSIGN mode.
func TestValidateAutoAddressClearsStatic(t *testing.T) {
	c := validConfig(t)
	c.CIDR = "auto"
	if err := c.Validate(); err != nil {
		t.Fatalf("Validate: %v", err)
	}
	if !c.AutoAddress || c.IP != "" || c.Mask != "" {
		t.Fatalf("got auto=%v ip=%q mask=%q", c.AutoAddress, c.IP, c.Mask)
	}
}

func TestValidateAddressRequiredMentionsAuto(t *testing.T) {
	c := DefaultEdgeConfig()
	c.Community = "demo"
	c.Supernodes = []string{"127.0.0.1:7777"}
	if err := c.Validate(); err == nil || !strings.Contains(err.Error(), "auto") {
		t.Fatalf("expected an address error mentioning auto, got %v", err)
	}
}

// IPv6 neighbour discovery uses 33:33:... multicast, which n2n drops unless
// multicast is accepted, so --ipv6 must imply -E.
func TestValidateIPv6AcceptsMulticast(t *testing.T) {
	c := validConfig(t)
	c.IPv6 = true
	c.DropMulticast = true
	if err := c.Validate(); err != nil {
		t.Fatalf("Validate: %v", err)
	}
	if c.DropMulticast {
		t.Fatal("IPv6 must force DropMulticast to false")
	}
}

func TestValidateWithoutIPv6KeepsDropMulticast(t *testing.T) {
	c := validConfig(t)
	c.DropMulticast = true
	if err := c.Validate(); err != nil {
		t.Fatalf("Validate: %v", err)
	}
	if !c.DropMulticast {
		t.Fatal("DropMulticast must stay true when IPv6 is off")
	}
}
