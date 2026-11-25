# N2N Go CLI 需求与设计文档

## 1. 原始需求

> 参考仓库：
> - `/Users/xtao/Work/n2n/omniedge-cli`
> - `/Users/xtao/Work/n2n/n2n`
>
> 期望参考 omniedge-cli，用 Go 包一下 n2n 的命令行，实现多平台的 n2n 命令行。

## 2. 背景与目标

n2n 是一套 C 实现的 P2P 二层 VPN（edge + supernode + community），原生通过 autotools 编译，命令行参数繁多，且各平台（Linux / macOS / Windows / FreeBSD / OpenWrt）需要自行编译和适配 TUN/TAP。

omniedge-cli 展示了“用 Go 包装 n2n”的可行路径：CGO 直接调用 `libn2n.a`，用 cobra 提供统一 CLI，并针对多平台交叉编译与 TUN/TAP 差异做补丁。

本项目目标：

1. 用 Go 封装 n2n，提供统一的 `n2n` 命令行入口；
2. 覆盖 n2n 核心能力：edge 节点、supernode、密钥生成、配置与状态查看；
3. 一次代码，多平台构建：macOS、Linux（含 arm/mips）、Windows、FreeBSD、OpenWrt；
4. 保留 n2n 原生能力的完整性与兼容性，同时提供更友好的参数和运行方式。

**非目标（初版）**：不实现 omniedge 的 SaaS 账号体系（登录、网络编排、心跳上报）；如后续需要，再作为独立模块叠加。

## 3. 参考实现分析

### 3.1 omniedge-cli 的包装方式

| 层次 | 做法 | 位置 |
| --- | --- | --- |
| n2n 源码引入 | `go:generate` 拉取 n2n 源码，打补丁后用 `configure && make` 产出 `libn2n.a` | `internal/coren2n/edge.go`、`internal/coren2n/make_n2n` |
| CGO 绑定 | `#cgo LDFLAGS: -ln2n -L${SRCDIR}/n2n`，暴露 `edge_configure` / `tuntap_open` / `edge_start` | `internal/coren2n/edge.c`、`edge.h` |
| 生命周期 | 用 `int *keep_running` 指针控制 edge 主循环，`Stop()` 置 0 实现优雅退出 | `internal/coren2n/edge.go` |
| 平台适配 | macOS 用 `utun` 补丁替换 tuntaposx；OpenWrt 按架构切换 musl 工具链 | `patches/`、`make_n2n_openwrt` |
| CLI | cobra + viper，命令：`login/register/join/start/scan/version` | `cmd/edgecli/cmd/` |
| 提权 | 独立 helper 进程承载需要 root 的操作 | `cmd/omniedge-helper`、`pkg/bridge` |
| 构建 | Makefile 按 `GOOS/GOARCH` 设置 `CGO_ENABLED=1`，先 `go generate` 再 build | `Makefile` |

### 3.2 n2n 原生能力

- 版本：3.1.1（见 `/Users/xtao/Work/n2n/n2n/VERSION`）
- 产物：`libn2n.a`、`edge`、`supernode`、`tools/n2n-keygen` 等
- 设备层：`tuntap_linux.c` / `tuntap_osx.c` / `tuntap_freebsd.c` / `tuntap_netbsd.c` / `win32`
- 加密与压缩：AES、ChaCha20(cc20)、Speck、LZO、zstd
- 关键 edge 参数：`-c` 社区、`-l` supernode、`-a` 虚拟 IP、`-k` 密钥、`-A` 加密算法、`-z` 压缩、`-H` 头部加密、`-r` 转发、`-E` 组播、`-p` 本地端口、`-i/-L` 注册间隔/TTL、`-t` 管理端口、`-d` 设备名、`-m` MAC、`-M` MTU、`-f` 前台运行
- 管理能力：`management.c` 提供本地管理端口接口，可用于查询状态

## 4. 方案选型

### 方案 A：CGO 静态链接 libn2n（推荐）

Go 直接调用 n2n C 接口，单二进制分发。

- 优点：性能与原生一致；单文件交付；可精细控制生命周期；可裁剪特性。
- 缺点：各平台需对应 C 工具链（Windows 需在 MSYS2/mingw-w64 上原生构建）；升级 n2n 需重新编译；Windows 需随包分发 `wintun.dll` 并实现 L3↔L2 shim（见 [Windows.md](Windows.md)）。

### 方案 B：Go 进程封装 n2n 二进制

Go 负责参数解析、配置生成、进程管理与日志，实际运行调用现成 `edge`/`supernode` 可执行文件。

- 优点：实现简单，可任意替换 n2n 版本；不依赖 CGO。
- 缺点：需要随包分发多平台二进制；进程控制与状态获取较弱；无法直接扩展 C 层能力。

### 方案 C：纯 Go 重写 n2n 协议

不推荐作为首版：工作量与兼容性风险极高。

**结论**：主路径采用方案 A，构建与分发层借鉴方案 B（各平台预编译 C 库或统一 CI 交叉编译）。

## 5. 技术架构

```
n2n_go/
├── cmd/n2n/                # CLI 入口（cobra）
├── internal/n2n/           # CGO 绑定层（edge / supernode / tuntap）
│   ├── edge.go             # Configure / OpenTunTap / Start / Stop
│   ├── edge.c / edge.h     # C 适配函数
│   └── supernode.go        # supernode 封装（后续）
├── internal/config/        # 参数、配置文件（edge.conf 兼容）校验
├── internal/service/       # 生命周期、信号处理、daemon/服务化
├── internal/mgmt/          # 本地管理端口查询（状态、统计）
├── third_party/n2n/        # n2n 源码（Git submodule，固定 3.1.1）
├── third_party/lib/        # 各平台预编译静态库（构建产物）
├── scripts/                # 按平台编译 n2n 的脚本
└── docs/
```

首次克隆后执行 `git submodule update --init --recursive` 获取 n2n 源码；submodule 固定在上游 3.1.1（`a6f9456`），升级 n2n 时需同步更新 submodule 指针并回归补丁。

关键设计点：

1. **配置模型**：Go struct ↔ `n2n_edge_conf_t` 字段一一映射；保留原生 `edge.conf` 格式兼容（`-k=v` 每行一项）。
2. **生命周期**：沿用 `keep_running` 指针方案，`SIGINT/SIGTERM` 触发 `Stop()`，保证 TUN/TAP 清理。
3. **参数校验**：社区名长度（16 字节截断/补零）、CIDR、端口、密钥与加密算法组合在 Go 层前置校验。
4. **提权**：TUN/TAP 创建需要 root/管理员权限；CLI 可检测权限并给出提示，必要时引入 helper 模式。
5. **构建隔离**：`go generate` 负责拉取/编译 n2n，`CGO_ENABLED=1` 按目标平台设置编译器和 CFLAGS（含 `-DSKIP_MULTICAST_PEERS_DISCOVERY` 以适配受限网络）。

## 6. 命令行设计（初版）

```
n2n edge        # 启动 edge 节点（别名 up/start）
n2n supernode   # 启动 supernode
n2n keygen      # 生成 n2n 密钥
n2n status      # 通过管理端口查看运行状态（后续）
n2n version     # 版本信息
```

参数映射示例（Go flag → n2n 原生参数）：

| CLI 参数 | n2n 对应 | 说明 |
| --- | --- | --- |
| `--community, -c` | `-c` | 社区名 |
| `--supernode, -l` | `-l` | supernode 地址，可多次指定 |
| `--ip, -a` | `-a` | 虚拟 IP / CIDR |
| `--key, -k` | `-k` | 加密密钥 |
| `--cipher, -A` | `-A` | 加密算法（aes/cc20/speck…） |
| `--compress, -z` | `-z` | 压缩算法（lzo/zstd…） |
| `--header-enc, -H` | `-H` | 头部加密 |
| `--routing, -r` | `-r` | 允许跨节点转发 |
| `--device, -d` | `-d` | TUN/TAP 设备名 |
| `--mac, -m` / `--mtu, -M` | `-m` / `-M` | 设备 MAC / MTU |
| `--local-port, -p` | `-p` | 本地 UDP 端口 |
| `--reg-interval, -i` / `--reg-ttl, -L` | `-i` / `-L` | 注册间隔 / TTL |
| `--mgmt-port, -t` | `-t` | 本地管理端口 |
| `--foreground, -f` | `-f` | 前台运行 |
| `--config` | `<config file>` | 兼容 `edge.conf` |

设计原则：短参数与 n2n 原生保持一致，便于迁移；长参数提供更自解释的命名；配置文件优先于默认值、命令行优先于配置文件。

## 7. 多平台构建

| 平台 | 目标架构 | 关键点 |
| --- | --- | --- |
| macOS | arm64 / amd64 | 使用原生 `utun`（需移植补丁，见 7.1）；`clang -arch`；可做 universal 包 |
| Linux | amd64 / arm64 / arm / 386 | 静态链接（musl/zig cc）；注意 glibc 兼容 |
| OpenWrt | mips / mipsle / arm / arm64 / amd64 | 交叉工具链 + `-DSKIP_MULTICAST_PEERS_DISCOVERY` |
| Windows | amd64 | wintun（用户态 DLL，随包分发）+ L3↔L2 shim；MSYS2/mingw-w64 原生构建；需管理员权限 |
| FreeBSD | amd64 | `gmake`，n2n 已有 `tuntap_freebsd.c` |

建议用 CI 矩阵（GitHub Actions）逐平台执行：`go generate` → 编译 n2n → `go build` → 产出压缩包/安装包。

### 7.1 macOS 兼容性（utun）

**现状判断**

- n2n 3.1.1 原生 [tuntap_osx.c](file:///Users/xtao/Work/n2n/n2n/src/tuntap_osx.c) 走 `/dev/tapN`（tuntaposx 第三方 kext）。macOS 10.15 起 Apple 弃用网络 kext，Apple Silicon 上加载 kext 还需降低安全级别，该路径已不可用。
- omniedge-cli 的 [tuntap_osx_utun.c](file:///Users/xtao/Work/n2n/omniedge-cli/patches/tuntap_osx_utun.c) 改用系统原生 `utun`（`PF_SYSTEM` + `SYSPROTO_CONTROL` + `com.apple.net.utun_control`），方向正确，但该补丁基于 `omniedgeio/n2n` 的 2.6 分支，**不能直接套到 3.1.1**：
  - `tuntap_open` 在 3.1.1 是 8 个参数（多一个 `int metric`），补丁为 7 个；
  - 3.1.1 的 edge 注册 supernode 时用 `device->device_mask` 计算 `net_bitlen`（[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L1321)），补丁只写 `ip_addr`，未设置 `device_mask`；
  - `dev_name` 类型为 `devstr_t`、`mtu` 为 `uint16_t`，字段需对齐 [n2n_typedefs.h](file:///Users/xtao/Work/n2n/n2n/include/n2n_typedefs.h#L235-L243)。
- 补丁用 `system("ifconfig ...")` 拼命令，存在命令注入风险且依赖 shell，封装时应改用参数数组 exec 或 ioctl。

**系统版本兼容性**

| macOS 版本 | utun 可用性 | 说明 |
| --- | --- | --- |
| 10.13–10.14 | 可用（需 root） | tuntaposx kext 尚可用的过渡期 |
| 10.15 Catalina | 可用（需 root） | 32 位/未签名 kext 失效，**必须改用 utun** |
| 11 Big Sur–12 Monterey | 可用（需 root） | Apple Silicon 下 kext 受限，utun 是推荐可行路径 |
| 13 Ventura–14 Sonoma | 可用（需 root） | API 行为无变化 |
| 15 Sequoia | 可用（需 root） | 新增「本地网络」隐私授权，影响局域网发现/直连 |
| 26 Tahoe | 可用（需 root） | 本地网络权限进一步收紧，首次访问需授权 |

**关键约束**

1. **权限**：utun 控制 socket 注册了 `CTL_FLAG_PRIVILEGED`，只能由特权进程创建，必须 root 或特权 helper 运行。
2. **API 定位**：Apple 仍将 utun 视为“受支持”的接口，但已明确不推荐用于新的临时 VPN 客户端，官方方向是 NetworkExtension（system extension）。作为非 App Store 分发的 root CLI，短期内不受影响，但需持续关注后续系统版本。
3. **L3→L2 语义损失**：utun 是三层设备，补丁用伪以太网头桥接，导致仅支持 IPv4；出站帧目的 MAC 恒为广播（退化为全员广播、MAC 学习不完整）；ARP 直接丢弃；IPv6、组播/广播（`-E`）、DHCP 地址模式均不支持。
4. **本地网络隐私**：macOS 15/26 对访问局域网的应用要求「本地网络」授权，n2n 的局域网组播发现与直连会受影响；可通过 `-DSKIP_MULTICAST_PEERS_DISCOVERY` 规避发现流程，但网络权限仍需用户或 MDM 授予。

**工程建议**：`sc_unit = 0` 让系统动态分配（避免 `EBUSY`）；补丁中补充 `device_mask`、`if_idx`、`mtu` 字段赋值；按需添加 `route add`；在 CLI 层检测 root 权限并给出明确提示。

## 8. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 交叉编译 CGO 依赖工具链 | 构建失败/耗时长 | 统一 Docker 镜像或 `zig cc`；CI 矩阵预构建 |
| TUN/TAP 需 root/管理员 | 普通用户无法启动 | 提权检测 + helper 模式 + 服务化安装 |
| Windows 设备层 | wintun 为 L3，与 n2n 的 L2 语义不匹配 | 设备层实现 L3↔L2 shim、ARP 应答与 MAC 学习；`wintun.dll` 随包分发并校验版本 |
| macOS utun 权限/隐私策略 | 需 root；局域网访问需授权 | 原生 `utun` + 权限检测 + helper；关注 NetworkExtension 政策变化 |
| n2n 源码升级成本 | 补丁失效 | 固定 submodule 版本，补丁脚本化、CI 校验 |
| 参数语义差异 | 迁移成本 | 短参数保持兼容，文档同步维护 |
| 静态链接与 glibc | 跨发行版运行异常 | 默认 musl 静态构建，另发 glibc 版本 |

## 9. 里程碑

- **M1 PoC**：macOS arm64 下 CGO 链接 libn2n，两个节点可 ping 通。
- **M2 CLI 骨架**：cobra 命令 + 配置解析 + 版本信息；完成 macOS/Linux 构建。
- **M3 多平台**：Linux amd64/arm64/arm、OpenWrt、FreeBSD、Windows 构建与冒烟测试。
- **M4 运维能力**：`supernode` 命令、状态查询、信号/daemon 管理。
- **M5 发布**：CI 产物矩阵、安装脚本、使用文档。

## 10. 待确认问题

1. CLI 是否需要兼容 n2n 原生短参数与 `edge.conf` 格式（默认按兼容设计）？
2. Windows 是否必须与 Linux/macOS TAP 双向互通（wintun 已定为默认方案）；IPv6/NDP 已按 `--ipv6` 实现，DHCP 仍待定，见 [Windows.md](Windows.md)。
3. 是否需要内置 supernode 管理（部署、监控），还是仅作为独立命令？
4. 是否需要 omniedge 式的账号/网络编排能力，还是定位为纯 n2n CLI？
5. 分发包形态：单二进制、压缩包、系统包（deb/rpm/brew）还是安装脚本？
6. macOS 是否接受 `utun` 方案的限制（仅 IPv4、广播语义、需 root）？若要求 IPv6 或 App Store 分发，则需改用 NetworkExtension。
