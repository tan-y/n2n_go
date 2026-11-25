package cli

import (
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	"n2n-go/internal/config"
	"n2n-go/internal/n2n"
)

// stringList collects the repeated -l/--supernode flags.
type stringList []string

func (s *stringList) String() string { return strings.Join(*s, ",") }

func (s *stringList) Set(v string) error {
	if v == "" {
		return errors.New("empty supernode")
	}
	*s = append(*s, v)
	return nil
}

// acceptMulticastFlag maps -E to n2n's drop_multicast field with inverted
// polarity: -E (true) accepts multicast, -E=false drops it. This matches the
// native n2n CLI, where -E means "accept multicast MAC addresses".
type acceptMulticastFlag struct {
	drop *bool
}

func (f acceptMulticastFlag) String() string {
	if f.drop == nil || *f.drop {
		return "false"
	}
	return "true"
}

func (f acceptMulticastFlag) Set(v string) error {
	accept, err := strconv.ParseBool(v)
	if err != nil {
		return fmt.Errorf("invalid boolean value %q for -E: %w", v, err)
	}
	*f.drop = !accept
	return nil
}

func (acceptMulticastFlag) IsBoolFlag() bool { return true }

// registerMulticastFlags wires -E (accept multicast, native n2n semantics) and
// --drop-multicast (explicitly drop, the default).
func registerMulticastFlags(fs *flag.FlagSet, drop *bool) {
	fs.Var(acceptMulticastFlag{drop: drop}, "E", "接受组播（等价原生 n2n -E；默认丢弃）")
	fs.BoolVar(drop, "drop-multicast", *drop, "丢弃组播（默认开启）")
}

// configPathFromArgs finds -config/--config before flag parsing, because the
// file has to be loaded before the flags are registered with file defaults.
func configPathFromArgs(args []string) string {
	for i, a := range args {
		for _, name := range []string{"-config", "--config"} {
			if a == name && i+1 < len(args) {
				return args[i+1]
			}
			if strings.HasPrefix(a, name+"=") {
				return strings.TrimPrefix(a, name+"=")
			}
		}
	}
	return ""
}

func runEdge(args []string) int {
	logger := log.New(os.Stderr, "", log.LstdFlags)

	// The config file is the base layer: CLI flags are parsed afterwards with
	// the file values as their defaults, so explicit flags still win.
	configPath := configPathFromArgs(args)
	cfg := config.DefaultEdgeConfig()
	if configPath != "" {
		fc, err := config.LoadFile(configPath)
		if err != nil {
			logger.Printf("invalid configuration: %v", err)
			return 2
		}
		fc.Apply(&cfg)
	}

	var supernodes stringList

	fs := flag.NewFlagSet("edge", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	fs.Usage = func() { fmt.Fprint(os.Stderr, edgeUsageText) }

	fs.StringVar(&configPath, "config", configPath, "JSON 配置文件路径（命令行参数优先于文件）")
	fs.StringVar(&cfg.Community, "c", cfg.Community, "社区名（必填，最长 19 字节）")
	fs.StringVar(&cfg.Community, "community", cfg.Community, "社区名（必填，最长 19 字节）")
	fs.Var(&supernodes, "l", "supernode 地址 host:port（可重复）")
	fs.Var(&supernodes, "supernode", "supernode 地址 host:port（可重复）")
	fs.StringVar(&cfg.CIDR, "a", cfg.CIDR, "静态 IPv4/CIDR（例如 10.0.0.1/24）或 auto（由 supernode 分配）")
	fs.StringVar(&cfg.CIDR, "ip", cfg.CIDR, "静态 IPv4/CIDR（例如 10.0.0.1/24）或 auto（由 supernode 分配）")
	fs.StringVar(&cfg.Key, "k", cfg.Key, "加密 key（使用非 none 加密算法时必填）")
	fs.StringVar(&cfg.Key, "key", cfg.Key, "加密 key（使用非 none 加密算法时必填）")
	fs.BoolVar(&cfg.HeaderEncryption, "H", cfg.HeaderEncryption, "头部加密（需配合 -k/--key）")
	fs.BoolVar(&cfg.HeaderEncryption, "header-encryption", cfg.HeaderEncryption, "头部加密（需配合 -k/--key）")
	fs.StringVar(&cfg.FederationPublicKey, "P", cfg.FederationPublicKey, "联邦公钥（user-password 认证，ASCII）")
	fs.StringVar(&cfg.FederationPublicKey, "federation-public-key", cfg.FederationPublicKey, "联邦公钥（user-password 认证，ASCII）")
	fs.StringVar(&cfg.AuthPassword, "J", cfg.AuthPassword, "user-password 认证密码（需 -k 且 -A cc20|speck，-I 作为用户名）")
	fs.StringVar(&cfg.AuthPassword, "auth-password", cfg.AuthPassword, "user-password 认证密码（需 -k 且 -A cc20|speck，-I 作为用户名）")
	fs.StringVar(&cfg.Cipher, "A", cfg.Cipher, "加密算法：none | aes | cc20 | speck | tf（默认 none）")
	fs.StringVar(&cfg.Cipher, "cipher", cfg.Cipher, "加密算法：none | aes | cc20 | speck | tf（默认 none）")
	fs.StringVar(&cfg.Compression, "z", cfg.Compression, "压缩算法：none | lzo（默认 none）")
	fs.StringVar(&cfg.Compression, "compress", cfg.Compression, "压缩算法：none | lzo（默认 none）")
	fs.StringVar(&cfg.Device, "d", cfg.Device, "设备名（macOS: utun/utunN；Linux: 留空自动分配 tapN；Windows: wintun 适配器名，留空为 n2n0）")
	fs.StringVar(&cfg.Device, "device", cfg.Device, "设备名（macOS: utun/utunN；Linux: 留空自动分配 tapN；Windows: wintun 适配器名，留空为 n2n0）")
	fs.StringVar(&cfg.MAC, "m", cfg.MAC, "设备 MAC（默认自动生成或派生）")
	fs.StringVar(&cfg.MAC, "mac", cfg.MAC, "设备 MAC（默认自动生成或派生）")
	fs.IntVar(&cfg.MTU, "M", cfg.MTU, "MTU（68-1500，默认 1290）")
	fs.IntVar(&cfg.MTU, "mtu", cfg.MTU, "MTU（68-1500，默认 1290）")
	fs.IntVar(&cfg.LocalPort, "p", cfg.LocalPort, "本地 UDP 端口（0 表示自动）")
	fs.IntVar(&cfg.LocalPort, "local-port", cfg.LocalPort, "本地 UDP 端口（0 表示自动）")
	fs.IntVar(&cfg.MgmtPort, "t", cfg.MgmtPort, "管理端口（默认 5644）")
	fs.IntVar(&cfg.MgmtPort, "mgmt-port", cfg.MgmtPort, "管理端口（默认 5644）")
	fs.IntVar(&cfg.RegisterInterval, "i", cfg.RegisterInterval, "注册间隔秒数（默认 20）")
	fs.IntVar(&cfg.RegisterInterval, "register-interval", cfg.RegisterInterval, "注册间隔秒数（默认 20）")
	fs.IntVar(&cfg.RegisterTTL, "L", cfg.RegisterTTL, "注册 TTL（0-255，默认 2）")
	fs.IntVar(&cfg.RegisterTTL, "register-ttl", cfg.RegisterTTL, "注册 TTL（0-255，默认 2）")
	fs.BoolVar(&cfg.AllowRouting, "r", cfg.AllowRouting, "允许路由")
	fs.BoolVar(&cfg.AllowRouting, "routing", cfg.AllowRouting, "允许路由")
	registerMulticastFlags(fs, &cfg.DropMulticast)
	fs.IntVar(&cfg.TOS, "T", cfg.TOS, "TOS（0-255，默认 0）")
	fs.IntVar(&cfg.TOS, "tos", cfg.TOS, "TOS（0-255，默认 0）")
	fs.StringVar(&cfg.Description, "I", cfg.Description, "设备描述（最长 15 字节）")
	fs.StringVar(&cfg.Description, "description", cfg.Description, "设备描述（最长 15 字节）")
	fs.BoolVar(&cfg.IPv6, "ipv6", cfg.IPv6, "承载 IPv6（自动接受组播，需两端都开启）")
	// 兼容原生 n2n 的 -f：本 CLI 始终前台运行，该参数被接受但不产生效果。
	fs.Bool("f", false, "前台运行（本 CLI 始终前台，兼容保留）")
	fs.Bool("foreground", false, "前台运行（本 CLI 始终前台，兼容保留）")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if fs.NArg() > 0 {
		logger.Printf("unexpected arguments: %s", strings.Join(fs.Args(), " "))
		return 2
	}

	// Command-line supernodes replace the file's list when any are given.
	if len(supernodes) > 0 {
		cfg.Supernodes = supernodes
	}
	if err := cfg.Validate(); err != nil {
		logger.Printf("invalid configuration: %v", err)
		return 2
	}

	if err := checkPrivileges(); err != nil {
		logger.Print(err)
		return 1
	}

	edge, err := n2n.NewEdge(&cfg)
	if err != nil {
		logger.Printf("failed to start edge: %v", err)
		return 1
	}
	defer edge.Close()

	addr := fmt.Sprintf("%s/%s", cfg.IP, cfg.Mask)
	if cfg.AutoAddress {
		addr = "auto（等待 supernode 分配）"
	}
	deviceName := edge.DeviceName()
	if deviceName == "" {
		deviceName = cfg.Device
	}
	logger.Printf("starting edge: community=%s supernodes=%s ip=%s device=%s cipher=%s ipv6=%t",
		cfg.Community, strings.Join(cfg.Supernodes, ","), addr, deviceName, cfg.Cipher, cfg.IPv6)
	if cfg.IPv6 {
		logger.Printf("IPv6 enabled: multicast is accepted regardless of -E (neighbour discovery)")
	}

	runErr := make(chan error, 1)
	go func() { runErr <- edge.Run() }()

	// In SN_ASSIGN mode the device stays unconfigured until the supernode
	// answers; apply the address as soon as it arrives.
	var autoErr chan error
	if cfg.AutoAddress {
		autoErr = make(chan error, 1)
		go func() { autoErr <- waitForAssignedAddress(edge, 15*time.Second) }()
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	defer signal.Stop(sig)

	for {
		select {
		case err := <-runErr:
			if err != nil {
				logger.Printf("edge stopped with error: %v", err)
				return 1
			}
			return 0
		case err := <-autoErr:
			if err != nil {
				logger.Printf("automatic address assignment failed: %v", err)
				edge.Stop()
				<-runErr
				return 1
			}
			autoErr = nil
		case s := <-sig:
			logger.Printf("received %s, shutting down", s)
			// 与 macOS.md §4.2 保持一致：等 C 层进入 edge loop 后再请求停止。
			select {
			case <-edge.Started():
				edge.Stop()
			case err := <-runErr:
				if err != nil {
					logger.Printf("edge stopped with error: %v", err)
					return 1
				}
				logger.Print("edge stopped, device released")
				return 0
			}
			if err := <-runErr; err != nil {
				logger.Printf("edge stopped with error: %v", err)
				return 1
			}
			logger.Print("edge stopped, device released")
			return 0
		}
	}
}

// waitForAssignedAddress polls the C layer until the supernode-assigned
// address has been applied to the device. The register interval governs
// how fast the first REGISTER_SUPER_ACK arrives; a failure here is reported to
// the caller so the process can exit with a non-zero status instead of looking
// connected while no address is configured.
func waitForAssignedAddress(edge *n2n.Edge, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		done, err := edge.ConfigureAuto()
		if err != nil {
			return err
		}
		if done {
			return nil
		}
		if time.Now().After(deadline) {
			return fmt.Errorf("no address assigned after %s; the supernode must support SN_ASSIGN", timeout)
		}
		time.Sleep(200 * time.Millisecond)
	}
}

const edgeUsageText = `Usage: n2n edge [options]

Required:
  -c, --community string   社区名（最长 19 字节）
  -l, --supernode host:port
                           supernode 地址（可重复，最多 8 个）
  -a, --ip IPv4[/CIDR]|auto
                            静态地址（裸 IP 默认 /24），或 auto 由 supernode 分配

Optional:
  -config, --config path    JSON 配置文件（命令行参数优先于文件）
  -k, --key string          加密 key
  -H, --header-encryption   头部加密（需配合 -k）
  -P, --federation-public-key string
                            联邦公钥（user-password 认证）
  -J, --auth-password string
                            user-password 认证密码
  -A, --cipher string       加密算法 none|aes|cc20|speck|tf（默认 none）
  -z, --compress string     压缩算法 none|lzo（默认 none）
  -d, --device string       macOS: utun/utunN；Linux: 空自动 tapN；Windows: wintun 适配器名
  -m, --mac string          设备 MAC（Windows 留空由 wintun shim 派生）
  -M, --mtu int             MTU 68-1500（默认 1290）
  -p, --local-port int      本地端口（0 自动）
  -t, --mgmt-port int       管理端口（默认 5644）
  -i, --register-interval int
                            注册间隔秒数（默认 20）
  -L, --register-ttl int    注册 TTL（默认 2）
  -r, --routing             允许路由
  -E                        接受组播（等价原生 n2n -E；默认丢弃）
  --drop-multicast          丢弃组播（默认开启）
  -T, --tos int             TOS 0-255（默认 0）
  -I, --description string  设备描述（最长 15 字节）
  --ipv6                    承载 IPv6（自动接受组播，需社区内所有节点都开启）
  -f, --foreground          兼容原生 n2n，本 CLI 始终前台运行（忽略）

macOS 首期限制：仅 IPv4 地址配置；不支持 DHCP、全局 IPv6 地址。IPv4 组播/广播已用
scripts/multicast-test.sh 验证（当前广播 MAC 实现下不受 -E 约束，见 docs/macOS-p5.md）。
--ipv6 只承载 IPv6 报文，接口使用 macOS 自动生成的链路本地地址（T3，见 docs/macOS-p5.md）。
-a auto 由 supernode 分配地址；多个 edge 同时启动时请给每个节点不同的 -I 描述，否则
可能拿到相同地址。需要 root 权限。

Windows 需要管理员权限，wintun.dll（0.14+）必须与 exe 同目录；支持 IPv4 与 --ipv6
（含 NDP 应答）；暂不支持 -a auto 与 DHCP。
`
