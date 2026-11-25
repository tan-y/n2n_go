// Package config holds the edge configuration and its validation rules.
// The numeric constants mirror n2n 3.1.1 so the cgo layer can pass them through.
package config

import (
	"crypto/rand"
	"fmt"
	"net"
	"strconv"
	"strings"
)

// Limits mirrored from include/n2n_define.h (n2n 3.1.1).
// Community occupies a 20-byte array (N2N_COMMUNITY_SIZE), so at most 19 bytes
// are usable; the description array (N2N_DESC_SIZE) leaves 15 usable bytes.
const (
	CommunityMax = 19 // N2N_COMMUNITY_SIZE - 1
	DescMax      = 15 // N2N_DESC_SIZE - 1
	MACMax       = 18 // N2N_MACNAMSIZ

	DefaultMTU      = 1290 // DEFAULT_MTU
	MinMTU          = 68
	MaxMTU          = 1500
	DefaultMgmtPort = 5644 // N2N_EDGE_MGMT_PORT

	DefaultRegisterInterval = 20 // REGISTER_SUPER_INTERVAL_DFL
	DefaultRegisterTTL      = 2  // N2N_DEFAULT_TTL

	MaxSupernodes = 8
)

// Transform IDs mirrored from n2n_typedefs.h.
const (
	TransformNull     = 1
	TransformTwofish  = 2
	TransformAES      = 3
	TransformChaCha20 = 4
	TransformSpeck    = 5
)

// Compression IDs mirrored from n2n_define.h.
const (
	CompressionNone = 1
	CompressionLZO  = 2
)

// EdgeConfig describes a single edge instance.
type EdgeConfig struct {
	Community  string
	Supernodes []string

	CIDR string // user supplied value, e.g. 10.0.0.1/24 or auto
	IP   string // dotted IPv4 address, filled by SetAddress
	Mask string // dotted IPv4 netmask, filled by SetAddress

	// AutoAddress is set when the address is assigned by the supernode
	// (SN_ASSIGN); IP and Mask stay empty until the assignment arrives.
	AutoAddress bool

	// IPv6 enables carrying IPv6 over the tunnel (--ipv6). The interface keeps
	// the link-local address macOS assigns; no global IPv6 address is set yet.
	IPv6 bool

	Key         string
	Cipher      string // none | aes | cc20 | speck | tf
	Compression string // none | lzo

	HeaderEncryption    bool   // -H: encrypt packet headers, requires Key
	FederationPublicKey string // -P: federation public key (ASCII, user-password auth)
	AuthPassword        string // -J: password for user-password authentication

	Device                string
	MAC                   string
	MTU                   int
	LocalPort             int
	MgmtPort              int
	RegisterInterval      int
	RegisterTTL           int
	AllowRouting          bool
	DropMulticast         bool
	DisablePMTUIDiscovery bool
	TOS                   int
	Description           string
}

// DefaultEdgeConfig returns the same defaults n2n's edge_init_conf_defaults uses
// for the fields exposed by the CLI.
func DefaultEdgeConfig() EdgeConfig {
	return EdgeConfig{
		Compression:           "none",
		Device:                defaultDeviceName(),
		MTU:                   DefaultMTU,
		MgmtPort:              DefaultMgmtPort,
		RegisterInterval:      DefaultRegisterInterval,
		RegisterTTL:           DefaultRegisterTTL,
		DropMulticast:         true,
		DisablePMTUIDiscovery: true,
	}
}

// SetAddress parses an IPv4 CIDR and fills IP and Mask. A bare IPv4 address
// defaults to /24, matching n2n's behavior when the prefix is omitted.
func (c *EdgeConfig) SetAddress(cidr string) error {
	cidr = strings.TrimSpace(cidr)
	if !strings.Contains(cidr, "/") {
		if ip := net.ParseIP(cidr); ip != nil && ip.To4() != nil {
			cidr += "/24"
		}
	}
	ip, ipnet, err := net.ParseCIDR(cidr)
	if err != nil {
		return fmt.Errorf("invalid address %q, expected IPv4 or IPv4/CIDR (e.g. 10.0.0.1/24): %w", cidr, err)
	}
	ip4 := ip.To4()
	if ip4 == nil {
		return fmt.Errorf("invalid address %q: only IPv4 is supported", cidr)
	}
	c.CIDR = cidr
	c.IP = ip4.String()
	c.Mask = net.IP(ipnet.Mask).String()
	return nil
}

// CipherID maps the cipher name to the n2n transform id.
func (c EdgeConfig) CipherID() (int, error) {
	switch strings.ToLower(c.Cipher) {
	case "none":
		return TransformNull, nil
	case "aes":
		return TransformAES, nil
	case "cc20", "chacha20":
		return TransformChaCha20, nil
	case "speck":
		return TransformSpeck, nil
	case "tf", "twofish":
		return TransformTwofish, nil
	default:
		return 0, fmt.Errorf("unknown cipher %q (supported: none, aes, cc20, speck, tf)", c.Cipher)
	}
}

// CompressionID maps the compression name to the n2n compression id.
func (c EdgeConfig) CompressionID() (int, error) {
	switch strings.ToLower(c.Compression) {
	case "none":
		return CompressionNone, nil
	case "lzo":
		return CompressionLZO, nil
	default:
		return 0, fmt.Errorf("unknown compression %q (supported: none, lzo)", c.Compression)
	}
}

// Validate normalizes defaults and checks constraints that n2n's C layer cannot
// report clearly. It must be called before building the C config.
func (c *EdgeConfig) Validate() error {
	if c.Community == "" {
		return fmt.Errorf("community is required (-c)")
	}
	if len(c.Community) > CommunityMax {
		return fmt.Errorf("community %q is too long: %d bytes, max %d", c.Community, len(c.Community), CommunityMax)
	}
	if len(c.Supernodes) == 0 {
		return fmt.Errorf("at least one supernode is required (-l host:port)")
	}
	if len(c.Supernodes) > MaxSupernodes {
		return fmt.Errorf("too many supernodes: %d, max %d", len(c.Supernodes), MaxSupernodes)
	}
	for _, sn := range c.Supernodes {
		host, port, err := net.SplitHostPort(sn)
		if err != nil {
			return fmt.Errorf("invalid supernode %q, expected host:port: %w", sn, err)
		}
		if host == "" {
			return fmt.Errorf("invalid supernode %q: empty host", sn)
		}
		p, err := strconv.Atoi(port)
		if err != nil || p < 1 || p > 65535 {
			return fmt.Errorf("invalid supernode %q: port must be 1-65535", sn)
		}
	}
	cidr := strings.TrimSpace(c.CIDR)
	if strings.EqualFold(cidr, "auto") {
		c.AutoAddress = true
		c.CIDR = "auto"
		c.IP = ""
		c.Mask = ""
	} else {
		c.AutoAddress = false
		if c.IP == "" || c.Mask == "" {
			if cidr == "" {
				return fmt.Errorf("address is required (-a IPv4/CIDR or -a auto)")
			}
			if err := c.SetAddress(cidr); err != nil {
				return err
			}
		}
	}
	if c.AutoAddress && !supportsAutoAddress() {
		return fmt.Errorf("address mode auto is not supported on this platform; use a static IPv4/CIDR (-a)")
	}

	if c.Device == "" {
		c.Device = defaultDeviceName()
	}
	if max := deviceNameMax(); len(c.Device) >= max {
		return fmt.Errorf("device name %q is too long: max %d bytes", c.Device, max-1)
	}
	if err := validateDeviceName(c.Device); err != nil {
		return err
	}

	if c.MTU == 0 {
		c.MTU = DefaultMTU
	}
	if c.MTU < MinMTU || c.MTU > MaxMTU {
		return fmt.Errorf("invalid MTU %d: must be %d-%d", c.MTU, MinMTU, MaxMTU)
	}

	if c.MAC == "" {
		// n2n seeds its PRNG from time()+clock(), which is nearly identical for
		// processes started in the same second on macOS. Two edges launched
		// together would then generate the same MAC and the supernode would
		// treat them as one node, so the MAC is drawn here instead. Windows
		// leaves it empty: the wintun shim derives it deterministically.
		mac, err := platformDefaultMAC()
		if err != nil {
			return fmt.Errorf("failed to generate a default MAC: %w", err)
		}
		c.MAC = mac
	}
	if c.MAC != "" {
		mac, err := net.ParseMAC(c.MAC)
		if err != nil {
			return fmt.Errorf("invalid MAC %q: %w", c.MAC, err)
		}
		if len(mac) != 6 {
			return fmt.Errorf("invalid MAC %q: must have 6 bytes", c.MAC)
		}
		if err := validateMAC(mac); err != nil {
			return err
		}
	}

	if c.LocalPort < 0 || c.LocalPort > 65535 {
		return fmt.Errorf("invalid local port %d", c.LocalPort)
	}
	if c.MgmtPort < 0 || c.MgmtPort > 65535 {
		return fmt.Errorf("invalid management port %d", c.MgmtPort)
	}
	if c.RegisterInterval < 1 {
		return fmt.Errorf("invalid registration interval %d: must be >= 1", c.RegisterInterval)
	}
	if c.RegisterTTL < 0 || c.RegisterTTL > 255 {
		return fmt.Errorf("invalid registration TTL %d: must be 0-255", c.RegisterTTL)
	}
	if c.TOS < 0 || c.TOS > 255 {
		return fmt.Errorf("invalid TOS %d: must be 0-255", c.TOS)
	}
	if len(c.Description) > DescMax {
		return fmt.Errorf("description %q is too long: %d bytes, max %d", c.Description, len(c.Description), DescMax)
	}

	if c.Cipher == "" {
		if c.Key != "" {
			c.Cipher = "aes"
		} else {
			c.Cipher = "none"
		}
	}
	cipherID, err := c.CipherID()
	if err != nil {
		return err
	}
	if c.Key == "" && cipherID != TransformNull {
		return fmt.Errorf("cipher %q requires -k/--key", c.Cipher)
	}
	if c.Key != "" && cipherID == TransformNull {
		return fmt.Errorf("cipher none does not accept -k/--key")
	}
	if c.HeaderEncryption && (c.Key == "") {
		return fmt.Errorf("header encryption (-H) requires -k/--key")
	}
	if c.AuthPassword != "" {
		// n2n requires a stream cipher for user-password authentication and
		// dereferences the payload key unconditionally, so both -k and
		// -A cc20|speck are mandatory.
		if c.Key == "" {
			return fmt.Errorf("user-password authentication (-J) requires -k/--key")
		}
		switch strings.ToLower(c.Cipher) {
		case "cc20", "chacha20", "speck":
		default:
			return fmt.Errorf("user-password authentication (-J) requires cipher cc20 or speck, got %q", c.Cipher)
		}
	}
	if c.FederationPublicKey != "" {
		// n2n-keygen emits 43 characters; n2n's edge.c rejects anything that
		// would not fit its 44-byte buffer.
		if len(c.FederationPublicKey) >= 44 {
			return fmt.Errorf("federation public key is too long: %d characters, max 43", len(c.FederationPublicKey))
		}
	}

	if c.Compression == "" {
		c.Compression = "none"
	}
	if _, err := c.CompressionID(); err != nil {
		return err
	}

	if c.IPv6 {
		// IPv6 neighbour discovery rides on 33:33:... multicast, which n2n
		// filters unless multicast is accepted; -E is therefore implied.
		c.DropMulticast = false
	}
	return nil
}

// randomMAC returns a random unicast, locally administered MAC address.
func randomMAC() (net.HardwareAddr, error) {
	b := make([]byte, 6)
	if _, err := rand.Read(b); err != nil {
		return nil, err
	}
	b[0] &= 0xFE // clear the multicast bit
	b[0] |= 0x02 // set the locally administered bit
	return net.HardwareAddr(b), nil
}
