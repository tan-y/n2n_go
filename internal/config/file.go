package config

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
)

// FileConfig is the on-disk JSON schema for an edge instance. Every field is
// optional: omitted fields keep the caller's default, and explicitly given CLI
// flags still take precedence because they are applied after this file.
type FileConfig struct {
	Community  *string  `json:"community"`
	Supernodes []string `json:"supernodes"`
	Address    *string  `json:"address"`

	Key         *string `json:"key"`
	Cipher      *string `json:"cipher"`
	Compression *string `json:"compression"`

	HeaderEncryption    *bool   `json:"header_encryption"`
	FederationPublicKey *string `json:"federation_public_key"`
	AuthPassword        *string `json:"auth_password"`

	Device                *string `json:"device"`
	MAC                   *string `json:"mac"`
	MTU                   *int    `json:"mtu"`
	LocalPort             *int    `json:"local_port"`
	MgmtPort              *int    `json:"mgmt_port"`
	RegisterInterval      *int    `json:"register_interval"`
	RegisterTTL           *int    `json:"register_ttl"`
	AllowRouting          *bool   `json:"allow_routing"`
	DropMulticast         *bool   `json:"drop_multicast"`
	DisablePMTUIDiscovery *bool   `json:"disable_pmtu_discovery"`
	TOS                   *int    `json:"tos"`
	Description           *string `json:"description"`
	IPv6                  *bool   `json:"ipv6"`
}

// LoadFile reads and decodes a JSON config file. Unknown keys are rejected so
// typos do not silently fall back to defaults.
func LoadFile(path string) (FileConfig, error) {
	var fc FileConfig

	data, err := os.ReadFile(path)
	if err != nil {
		return fc, fmt.Errorf("read config %q: %w", path, err)
	}

	dec := json.NewDecoder(bytes.NewReader(data))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&fc); err != nil {
		return fc, fmt.Errorf("parse config %q: %w", path, err)
	}
	if len(fc.Supernodes) > MaxSupernodes {
		return fc, fmt.Errorf("config %q: too many supernodes: %d, max %d", path, len(fc.Supernodes), MaxSupernodes)
	}
	return fc, nil
}

// Apply copies the non-nil fields onto c. It never validates; call Validate
// afterwards so the caller gets one consistent error path.
func (f *FileConfig) Apply(c *EdgeConfig) {
	if f.Community != nil {
		c.Community = *f.Community
	}
	if len(f.Supernodes) > 0 {
		c.Supernodes = append([]string(nil), f.Supernodes...)
	}
	if f.Address != nil {
		c.CIDR = *f.Address
	}
	if f.Key != nil {
		c.Key = *f.Key
	}
	if f.Cipher != nil {
		c.Cipher = *f.Cipher
	}
	if f.Compression != nil {
		c.Compression = *f.Compression
	}
	if f.HeaderEncryption != nil {
		c.HeaderEncryption = *f.HeaderEncryption
	}
	if f.FederationPublicKey != nil {
		c.FederationPublicKey = *f.FederationPublicKey
	}
	if f.AuthPassword != nil {
		c.AuthPassword = *f.AuthPassword
	}
	if f.Device != nil {
		c.Device = *f.Device
	}
	if f.MAC != nil {
		c.MAC = *f.MAC
	}
	if f.MTU != nil {
		c.MTU = *f.MTU
	}
	if f.LocalPort != nil {
		c.LocalPort = *f.LocalPort
	}
	if f.MgmtPort != nil {
		c.MgmtPort = *f.MgmtPort
	}
	if f.RegisterInterval != nil {
		c.RegisterInterval = *f.RegisterInterval
	}
	if f.RegisterTTL != nil {
		c.RegisterTTL = *f.RegisterTTL
	}
	if f.AllowRouting != nil {
		c.AllowRouting = *f.AllowRouting
	}
	if f.DropMulticast != nil {
		c.DropMulticast = *f.DropMulticast
	}
	if f.DisablePMTUIDiscovery != nil {
		c.DisablePMTUIDiscovery = *f.DisablePMTUIDiscovery
	}
	if f.TOS != nil {
		c.TOS = *f.TOS
	}
	if f.Description != nil {
		c.Description = *f.Description
	}
	if f.IPv6 != nil {
		c.IPv6 = *f.IPv6
	}
}
