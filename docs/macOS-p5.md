# macOS p5 设计：IPv6、地址自动分配、组播

> 目标：在现有 macOS utun（L3）实现上扩展网络能力。本文只做设计，不含代码改动。
> 现状基线：`docs/macOS.md` 的 IPv4 静态地址方案已跑通（ping/TCP/SSH）。

## 1. 结论摘要

| 能力 | 结论 | 原因 |
| --- | --- | --- |
| 组播/广播社区 | **可做，n2n 内核无需改动** | supernode 会把非单播目的 MAC 的帧广播给社区内其它 edge。注意本项目 shim 用广播 MAC，`drop_multicast` 只匹配 `01:00:5E`/`33:33`，因此本项目之间不依赖 `-E`；与原生 TAP 对端互通时需要双方接受组播（见 3.1） |
| 地址自动分配 | **可做，用 supernode 分配（SN_ASSIGN）替代 DHCP** | n2n 3.1.1 的 DHCP 只在 Windows（`win32/wintap.c`）实现；macOS/Linux 的 `tuntap_open` 都忽略 `address_mode`。supernode 3.1.1 默认就会分配（默认网段 `10.128.0.0–10.255.255.0/24`），但本项目启动顺序与上游不同，需二次配置（3.2） |
| DHCP | **不做**，对齐上游能力 | 要支持就得自己实现 DHCP 客户端（DISCOVER/OFFER/REQUEST/ACK + 广播收发），收益低、风险高 |
| IPv6 | **可做，属于本项目的适配层工作** | utun 能收到 AF_INET6；难点在伪以太目的 MAC、IPv6 传输层校验和、以及对端 `-E` 的配置要求 |

建议实施顺序：`-E` 语义修正（已完成）→ 组播开关 → 地址自动分配 → IPv6。

## 2. 代码事实（设计依据）

- `-E` / `drop_multicast` 双向都会丢弃组播：
  - 发送侧：`src/edge_utils.c:2194-2197`
  - 接收侧：`src/edge_utils.c:1769-1773`
  - IPv6 发现帧（目的 MAC `33:33:...`）也算组播：`src/edge_utils.c:199-213`
- supernode 的转发规则：
  - 目的 MAC 是单播 → `try_forward`；否则 → `try_broadcast`：`src/sn_utils.c:1744`、`src/sn_utils.c:1793-1797`
  - 也就是说，只要发送端没丢，组播帧会被 supernode 广播到社区内所有其它 edge。
- `-E` 语义（**T0 已修正，与原生一致**）：
  - 原生 `-E` 表示接受组播（`conf->drop_multicast = 0`）：`src/edge.c:486-490`；
  - 本项目 `-E` 同样表示接受组播，`--drop-multicast` 显式恢复丢弃，默认仍丢弃；实现见 `internal/cli/edge.go` 的 `registerMulticastFlags`，单测见 `internal/cli/edge_test.go`。
- 启动阶段的硬约束（影响 P5-B）：
  - `run_edge_loop` 在非 Windows 下无条件 `FD_SET(eee->device.fd, ...)`：`src/edge_utils.c:2933`，因此进入 loop 前 fd 必须有效；
  - 首次 `REGISTER_SUPER_ACK` 只有在 `eee->device.mac_addr` 非空时才更新 `last_sup`：`src/edge_utils.c:2593-2600`，因此 MAC 必须在注册前生成；
  - 首次注册成功后会立即发送免费 ARP，使用的是 `eee->device.ip_addr`：`src/edge_utils.c:2598`、`src/edge_utils.c:1549-1560`。
- 胶水层的 `eee->device` 是 `e->dev` 的**副本**（`internal/n2n/edge_glue.c:231`），任何二次配置都必须同时更新两者。
- SN_ASSIGN 的分配规则：`reg.dev_addr` 为 0 或 `net_bitlen` 为 0 时由 supernode 分配（`src/sn_utils.c:1985-1997`）；默认网段 `10.128.0.0–10.255.255.0/24`（`include/n2n_define.h:163-165`）；候选地址按 `dev_desc` 哈希后线性探测（`src/sn_utils.c:1218-1265`）。
- n2n 的 peer 表只保存 IPv4 `dev_addr`，IPv6 无法复用现有 `peer_mac_for_ip`（`platform/macos/tuntap_osx.c:45-66`）。
- 地址自动分配（SN_ASSIGN）时机（**上游**）：
  - 先发 `REGISTER_SUPER` 取地址（`src/edge.c:1212-1243`），再 `tuntap_open`（`src/edge.c:1245-1251`）。
  - 地址回填在 `src/edge_utils.c:2573-2586`。
- **本项目与上游的差异（影响 P5-B）**：胶水层在 `edge_init` 之后立即 `tuntap_open`（`internal/n2n/edge_glue.c:216-230`），而首次注册发生在随后的 `run_edge_loop` 里。也就是说，SN_ASSIGN 回填地址的时刻晚于设备打开，不能直接照搬上游顺序。
- macOS 上游 `tuntap_open` 不使用 `address_mode`（只做静态 `ifconfig`）；DHCP 仅见于 `win32/wintap.c`。
- 当前本项目 shim 的限制：
  - 只接受 `AF_INET`，其它 family 记录日志后丢弃（`platform/macos/tuntap_osx.c` 的 `tuntap_read`）；
  - 写路径只处理以太类型 `0x0800` / `0x86DD`；`0x0806`（ARP）在 T5 起由 shim 自己应答（见 3.4）；
  - 校验和补全原仅按 IPv4 伪头计算；T4 已补上 IPv6（含 ICMPv6）。

## 3. 方案

### 3.1 组播/广播社区（P5-A）

改动很小：

1. **T0（已实现）：修正本项目 `-E` 语义**。原生 `-E` 表示接受组播；本项目改为同样语义（`-E` → `drop_multicast=false`），默认值仍为丢组播（`true`），并保留 `--drop-multicast` 作为显式开关。
2. **重要事实（T1 核对）**：本 shim 对未知目的的 IPv4 帧一律使用广播 MAC，而 n2n 的 `drop_multicast` 只匹配 `01:00:5E` 与 `33:33` 目的 MAC（`is_ethMulticast` / `is_ip6_discovery`）。因此：
   - 本项目之间（广播 MAC）的 IPv4 组播**不受 `-E` 影响**，默认丢弃设置下也能通；
   - `-E` 只在接收真实组播 MAC 时生效：原生 TAP 对端发出的 `01:00:5E` 帧，以及后续 IPv6 ND 的 `33:33` 帧；
   - 与原生对端互通时，原生端和本项目接收端都仍需开启 `-E`。
3. IPv4 组播地址映射为 `01:00:5E:..` **首期不做**，统一广播即可（supernode 反正会广播）。若后续要做，映射后 `-E` 才会成为 IPv4 组播的硬开关。

验收方式（`scripts/multicast-test.sh`，需 root，**已通过**）：

2026-11-13 实测（macOS 26.7.2 arm64，1 supernode + 3 edge，发送 4 个 UDP 报文到 `239.255.42.42:9999`）：

| 方向 | 接收端 | 发送前 Ipkts | 发送后 Ipkts | 结论 |
| --- | --- | --- | --- | --- |
| edge1(utun4) → 组播 | edge2(utun6)，`-E` | 0 | 8 | 通过 |
| edge1(utun4) → 组播 | edge3(utun5)，默认 `--drop-multicast` | 0 | 8 | 通过（未被 `-E` 拦截） |
| edge2(utun6) → 组播 | edge1(utun4)，`-E` | 0 | 8 | 通过 |

- 断言只比较 `Ipkts` 是否增长，不校验具体报文数；实测每个方向增长 8（发了 4 个 UDP 报文），同机多进程共享内核栈时计数会放大，属预期，不影响结论；
- edge3 使用默认丢弃仍收到，证实 §3.1 第 2 条：广播 MAC 不匹配 `drop_multicast` 的过滤条件；
- 不能用同一节点自测：supernode 的 `try_broadcast` 会排除发送者本身。

不做：组播路由、IGMP 代理、mDNS 反射器。

### 3.2 地址自动分配（P5-B）

目标：用户不写静态地址时，由 supernode 分配社区内地址。

- CLI/配置（改动点比表面多）：
  - `-a auto` / `"address": "auto"`，`Validate` 对 `auto` 跳过 CIDR 解析与地址必填校验（`internal/config/config.go:177-185`）；
  - 解析后设置 `conf.tuntap_ip_mode = TUNTAP_IP_MODE_SN_ASSIGN`，`ip_addr`/`netmask` 留空；
  - 胶水层不能再拒绝空的 `ip`/`mask`（`internal/n2n/edge_glue.c:101-103`），不能硬编码 `TUNTAP_IP_MODE_STATIC`（`internal/n2n/edge_glue.c:187`），`tuntap_priv_conf.ip_mode` 也不能固定写 `"static"`（`internal/n2n/edge_glue.c:241-246`）。
- 启动顺序：**采用方案 B（注册后二次配置）**。方案 A 需要复刻上游 `main()` 的 bootstrap runlevel 流程，改动面大且没有额外收益；`curr_sn` 已在 `edge_init` 中初始化（`src/edge_utils.c:404`），方案 B 没有阻塞项。
- 方案 B 的硬约束：
  1. **必须先 open（无地址）再 configure，不能延后 open**：`run_edge_loop` 会无条件 `FD_SET(eee->device.fd)`（`src/edge_utils.c:2933`），fd 为 -1 属未定义行为；且 MAC 必须在首次注册前生成，否则 `last_sup` 不会更新（`src/edge_utils.c:2593`）。
  2. **configure 必须同时更新 `e->dev` 与 `eee->device`**（后者是副本，`internal/n2n/edge_glue.c:231`），否则后续注册仍上报 `0/0`（`src/edge_utils.c:1320-1321`），supernode 会反复重新分配地址。
  3. ACK 回填的是 `eee->tuntap_priv_conf`（`src/edge_utils.c:2573-2587`），读错误重开设备会用它；`device_mac` 需提前写入，避免重开时换 MAC（当前 `Validate` 总会生成 MAC，胶水层已复制）。
  4. 首次 ACK 后 n2n 会立即发送免费 ARP，此时 `eee->device.ip_addr` 仍为 0，会发出 `0.0.0.0` 的 ARP；shim 会丢弃 ARP，影响有限，但建议 configure 完成后补发一次。
- 胶水层接口（已实现，2026-11-13）：
  - `-a auto`（大小写不敏感）→ `config.EdgeConfig.AutoAddress`，`Validate` 跳过 CIDR 解析并清空 IP/Mask；
  - 胶水层 `auto_address` → `conf.tuntap_ip_mode = TUNTAP_IP_MODE_SN_ASSIGN`，`tuntap_priv_conf.ip_mode = "sn_assign"`；open 阶段允许空地址，`tuntap_osx_configure()` 只设 MTU 与 `up`；
  - `n2n_go_edge_configure_auto()`（由 `Edge.ConfigureAuto()` 调用）：ACK 回填 `tuntap_priv_conf.ip_addr/netmask` 后复用 `tuntap_osx_configure()`（ifconfig + 子网路由），并同步 `eee->device.ip_addr/device_mask`；返回 1=已完成、0=尚未分配、-1=失败；配置成功后置 `eee->sn_wait = 2`，主循环下一次 `update_supernode_reg()` 就会用新地址重新注册（受 `select()` 超时限制，最多约 10s），supernode 随后把该地址广播给其他 edge（supernode 自己不记录分配结果，见下）；
  - Go 侧在 `Run()` 期间每 200ms 轮询一次，15s 未拿到地址则打印错误、停止 edge 并以退出码 1 退出，避免“看似连上却不通”；
  - 验收脚本：`scripts/auto-address-test.sh`（`make auto-address`，需 root）。
- `tuntap_open` 的地址校验：**open 阶段允许 `ip_addr == 0`**（这是方案 B 的前提）；最终 configure 阶段（静态模式，以及 auto 模式拿到地址后）必须非 0，否则报错。
- 风险与限制：
  - 公共 supernode 3.1.1 默认支持分配（`10.128.0.0–10.255.255.0/24`），主要风险是分配网段与本机已有 `10.x` 网段冲突，以及公共 supernode 版本过旧；
  - **supernode 不记录自己分配的地址**（`sn_utils.c:1993` 只写进 ACK），`comm->edges` 里仍是 edge 上报的 `0/0`，因此冲突检测 `ip_addr_available()` 看不到“已分配但未重新注册”的地址；
  - 候选地址由 `pearson_hash_32(dev_desc)` 取模得到，`dev_desc` 默认是主机名。所以**同主机同时启动多个 `-a auto` edge 会拿到同一个地址**（哈希相同、SN 侧又只有一条 `0/0` 记录）；不同主机或多个默认描述相同的节点也可能撞车。规避：给每个 edge 不同的 `-I` 描述，或错开启动（等前一个已重新注册），或直接用静态地址；
  - 地址冲突由 supernode 负责；
  - 方案 B 在拿到地址前接口不可用，需要明确日志提示阶段。
- 验收：本地 supernode + 两个 edge 均使用 `-a auto`，日志出现 `supernode assigned address ...`，两个 utun 地址不同且 `ifconfig` 已配置；同机时两端地址都是本机地址，单播/子网广播 ping 都不进入 utun（macOS.md §6.2），因此按 T1 的做法给组播组加主机路由并发包，让报文真正进入 overlay，再用管理端口 `edges` 断言双方 peer 表互相出现对方地址（n2n 的 peer 发现由流量驱动，L3 shim 没有 ARP 可触发）；见 `scripts/auto-address-test.sh`。

### 3.3 IPv6（P5-C）

范围：支持“IPv6 over n2n 社区”，不做 NDP 代理/RA 服务。

实现状态（2026-11-13，T3 已完成并实测通过）：读写路径、RFC 2464 组播 MAC 映射、NA/NS 邻居学习（TTL 300s、上限 64 条、LRU 淘汰）均已落地，开关为 `--ipv6`（同时强制 `drop_multicast=false`，见 3.3.5）。实测 macOS **不会**为 utun 自动生成 link-local，shim 按设备 MAC 的 EUI-64 派生 `fe80::/64` 并 `ifconfig` 配置；同时 macOS 在启用 IPv6 后会再加一个主机级（所有接口相同）的 link-local，两者共存，不影响使用。全局地址留给后续 `-6`。校验和补全仍是 T4。

#### 3.3.1 读路径（utun → 伪以太 → n2n）

1. 接受 `AF_INET6`（Darwin 值为 30），构造以太类型 `0x86DD`。
2. 伪以太目的 MAC：
   - IPv6 目的为组播（`ff00::/8`）→ 按 RFC 2464 映射 `33:33:xx:xx:xx:xx`（取 IPv6 目的低 32 位）。这覆盖 NS/RS/MLD，接收端需接受组播（T0 后 `-E`）才能放行。
   - IPv6 单播 → 查 shim 内的 IPv6→MAC 邻居表；未命中则退回广播 MAC（功能优先，先跑通再优化）。
3. 邻居表来源：写路径解析入站 NA/NS，记录 `(IPv6, src MAC)`；**以 NA 的目标地址（ICMPv6 第 8–24 字节）为主**，NS 的源地址在 DAD 阶段是 `::`，不能作为有效映射。条目 TTL 5 分钟，上限 64 条。对端重启后 MAC 会变化（除非固定 `-m`），而 supernode 的 `try_forward` 对过期 MAC 静默丢弃、不会回退广播，因此未命中时先广播、并尽快通过 NA 重新学习。这样能逐步把单播从“广播”收敛为“单播”，避免持续泛洪。
4. 报文长度：IPv6 头固定 40 字节，`total = 40 + payload_len`，直接用现有 `ip_len` 逻辑。

#### 3.3.2 写路径（n2n → 伪以太 → utun）

- 接受以太类型 `0x86DD`，去掉伪以太头，前置 `AF_INET6` family 写入 utun。
- 其它非 IP 类型维持“记录并丢弃”。

#### 3.3.3 传输层校验和（关键，T4 已实现）

macOS utun 对 IPv6 的 TCP/UDP 同样会留下待完成校验和；IPv6 伪头是 `src(16) + dst(16) + upper-layer length(4) + next header(4)`。

实现（`platform/macos/tuntap_osx.c` 的 `complete_transport_csum_v6`，由 `complete_transport_csum` 按版本号分派）：

- IPv4：原有逻辑不变；
- IPv6：直接跟在固定头之后的 `next header = 6/17/58`（TCP/UDP/ICMPv6）补全校验和；扩展头（含分片头 44）一律跳过，与 IPv4 分片策略一致；
- ICMPv6 一并纳入：重算对已正确的校验和是无操作，可同时覆盖 macOS 走 offload（校验和为 0/占位）和不走 offload 两种情况；
- 不因 UDP 校验和为 0 而跳过（IPv6 下该校验和必填，0 视为待补全）。
- 验收：`ping6` 到对端 link-local 能收到 echo reply（`scripts/ipv6-test.sh` phase C，2026-11-13 实测通过）；TCP 走 IPv6 的验证留到 T5 与原生对端联调。

#### 3.3.4 接口与路由

- 开关：`--ipv6`（布尔，默认关闭；关闭时 shim 仍按原逻辑记录并丢弃 IPv6 帧）。
  - 已实现：只承载 IPv6；实测 macOS 不为 utun 自动生成 link-local，因此 shim 用设备 MAC 的 EUI-64（U/L 位取反）派生 `fe80::/64` 并 `ifconfig utunN inet6 fe80::... prefixlen 64 up`，T3 验收基于该 link-local。
  - 后续：可选 `-6 <addr>/<prefix>` 配置全局地址，`ifconfig utunN inet6 <addr>/<prefix> up`，按需 `route add -inet6`（`/128` 跳过）。
- MTU 沿用现有 1290；IPv6 下有效载荷更小，必要时文档建议 `-M 1280`。

#### 3.3.5 社区配置要求

- 所有接收节点必须 `drop_multicast=false`，否则 NS/NA 被丢，IPv6 无法建链。
- 本项目在启用 IPv6 但用户仍保持丢弃组播（默认，或显式 `--drop-multicast`）时：
  - `Validate()` 强制 `drop_multicast=false`，启动日志打印 “IPv6 enabled: multicast is accepted regardless of -E”；
  - 对端仍需各自开启 `--ipv6`（或 `-E`），文档写明。

### 3.4 与原生（TAP）对端互通（P5-D / T5）

原生 edge 走的是二层 TAP：它在向我们的 overlay IP 发包前必须先 ARP 解析我们的 MAC。shim 是三层 utun，内核不会替我们回答 ARP，因此：

1. **ARP 应答（已实现）**：`tuntap_write` 收到 `0x0806` 且 opcode=1、target protocol address 等于本机 overlay 地址时，构造 ARP reply（sender MAC 用 `device->mac_addr`），直接调用 n2n 的 `edge_send_packet2net` 送回隧道——utun 无法承载以太帧，所以不能借道内核。
2. **出向已就绪**：`peer_mac_for_ip` 用对端注册时的 MAC 做单播目的；未知目的回落广播 MAC，原生 TAP 同样接受。
3. **IPv6 邻居通告（已实现）**：`tuntap_write` 收到 ICMPv6 NS（type 135）且 target 等于本机 link-local（`fe80::/64` + EUI-64）时，构造 NA（type 136，Solicited+Override，带 target link-layer address 选项），用 `complete_transport_csum_v6` 补全校验和后经 `edge_send_packet2net` 送回隧道；源地址为 `::` 的 DAD 探测不应答。
4. **验收脚本** `scripts/interop-test.sh`：容器内跑静态编译的原生 3.1.1 edge（TAP），与本机 edge 组成同一社区，做双向 ICMP、广播发现、peer 表断言与 IPv6 互通（本机 edge 带 `--ipv6`，ping 原生 `tap0` 的 link-local）。需要在有 docker 的机器上以 root 执行。
   - 2026-11-14 实测通过：host → native `10.44.0.2` 3/3 收到、native → host `10.44.0.1` 3/3 收到，本机 edge 的 peer 表出现 `10.44.0.2`（`p2p 7,6`）。
   - 实测细节：原生 edge 必须加 `-f`（默认 daemonize，容器里父进程退出会连带杀掉子进程）；容器内看到的本机 edge 地址是 NAT 后的宿主机地址，属正常现象。
   - 2026-11-14 IPv6 互通实测通过：`ping6 -I utun4 fe80::dc32:59ff:fe95:a88` 3/3 收到。注意 macOS 会在我们的 EUI-64 之外再加一个主机级 link-local 并优先选它作源地址，因此 NA 必须应答设备上的**所有** IPv6 地址（`getifaddrs` 收集，未命中时 1s 限频刷新）。

## 4. 数据流示意

```
Mac 内核 IPv4/IPv6
      │  utun (AF_INET / AF_INET6 原始 IP)
      ▼
tuntap_read  ──►  伪以太头（dst MAC：单播查表 / 组播映射 / 广播兜底）
      │             ├─ IPv4 → 0x0800 + 校验和补全
      │             └─ IPv6 → 0x86DD + 校验和补全
      ▼
n2n edge ──► supernode ──► 社区内其它 edge ──► 伪以太 ──► 对端 utun
```

## 5. 默认行为与兼容性

- IPv4 路径行为不变；默认仍丢弃组播。
- `-E` 的语义在 T0 修正为“接受组播”，与原生 n2n 一致；`--drop-multicast` 用于显式恢复丢弃。
- IPv6 必须显式开启（`-6` 或配置字段）；未开启时仍按现有逻辑丢弃 IPv6 帧。
- 地址自动分配必须显式 `-a auto`；静态地址行为不变。
- 与原生 n2n 对端互通：组播能力依赖对端也接受组播（原生 `-E`），这一点无法由我们单方面保证，文档必须写清楚。

## 6. 测试计划

单元测试：
- `-E` 语义：`-E` 后 `DropMulticast == false`，默认 `true`，`--drop-multicast` 可覆盖（T0）；
- IPv6 组播 MAC 映射（`ff02::1` → `33:33:00:00:00:01`）；
- IPv6 TCP/UDP/ICMPv6 校验和补全（构造伪头 + 段，校验计算结果）；
- `-a auto` 的 Validate 分支（合法/非法组合）。

组播验证（`scripts/multicast-test.sh`，需 root，已通过）：
- 单 supernode + 三个 edge（两个 `-E`、一个默认丢弃），断言双向到达与广播 MAC 行为。

自动地址验证（`scripts/auto-address-test.sh`，需 root）：
- 单 supernode + 两个 `-a auto` edge（不同 `-I` 描述），断言两者地址不同、utun 已配置该地址；再给组播组加主机路由发包，断言双方 peer 表互相包含对方地址。2026-11-13 实测通过（`10.226.198.73` / `10.226.198.117`）。

IPv6 验证（`scripts/ipv6-test.sh`，需 root）：
- 单 supernode + 三个 edge（两个 `--ipv6`、一个只 `-E`），向 `ff02::1%utun` 发 ICMPv6，断言开启端 utun 收到、未开启端 utun 不变；同时校验两端 link-local 已生成且由 MAC 派生（互不相同）；最后 `ping6 -I <utun> <对端 link-local>`，要求收到 echo reply（验证 IPv6 校验和补全）。2026-11-13 实测通过：`utun8` 0→12、未开启 IPv6 的 `utun4` 保持 0、`ping6` 收到 3 个回包。

冒烟测试扩展（`scripts/smoke-test.sh`）：
- 双 edge 使用 `-a auto`（本地 supernode），确认拿到不同地址，并用组播流量触发 peer 发现（同机 ICMP 不经过 utun）。

手工验证：
- 与原生 n2n macOS/Linux edge 互通（对端用 `-E` 接受组播）；
- IPv6 下 `ping6` 与 `ssh`，分别确认 ICMPv6 与 TCP 校验和修复生效。

## 7. 实施拆分

| 阶段 | 内容 | 依赖 |
| --- | --- | --- |
| T0 | 修正 `-E` 语义（`-E` = 接受组播）并补测试/文档（已完成） | 无 |
| T1 | 组播开关与文档；用 `multicast-test.sh` 验证广播 MAC 组播（含 `-E` 对照）（已完成，2026-11-13 三节点实测通过） | T0 |
| T2 | `-a auto`（SN_ASSIGN）+ 方案 B 二次配置 + 测试（已完成，2026-11-13 双 edge 实测通过） | 无（可与 T0 并行） |
| T3 | IPv6 读写路径（0x86DD、组播 MAC 映射、邻居表）（已完成，2026-11-13 三节点实测通过） | T1 |
| T4 | IPv6 校验和补全（TCP/UDP/ICMPv6）（已完成，2026-11-13 `ping6` 端到端实测通过） | T3 |
| T5 | 与原生对端联调（ARP 应答 + 容器内原生 edge，2026-11-14 实测通过） | T2、T4 |

## 8. 风险

- 公共 supernode 默认支持地址分配；主要风险是分配网段（`10.128.0.0–10.255.255.0/24`）与本机已有 `10.x` 网段冲突，以及公共 supernode 版本过旧。失败时给出明确错误。
- 方案 B 在拿到地址前接口不可用，若超时未退出，用户会误判为“已连上但不通”。
- IPv6 单播在邻居表未命中时走广播，可能产生额外流量；对端重启换 MAC 后，旧表项会导致单播被 supernode 静默丢弃，需要靠短 TTL/NA 重新学习兜底。
- IPv6 扩展头/分片首期不处理，极端场景会表现为丢包。
- macOS 是否自动为 utun 生成 link-local 已实测：不会，由 shim 按 MAC 派生；ICMPv6 校验和无论是否 offload 都已纳入补全（重算幂等），T4 只需验证 `ping6` 回包。

## 9. 设计选择

已确认：

1. 地址自动分配使用 `-a auto`。
2. IPv6 地址使用独立参数 `-6 <addr>/<prefix>`。
3. IPv6 仅在显式开启时转发（未开启时维持现有丢弃行为）。

待确认：

4. P5-B 的启动顺序：推荐方案 B（注册后二次配置）。方案 A 需要复刻上游 bootstrap 流程，改动大且无额外收益，基本可以排除；确认后本文按 B 定稿。
