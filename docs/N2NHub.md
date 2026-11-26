# n2nhub（SaaS 设计参考）

n2nhub 是叠加在开源 n2n CLI 之上的最小托管控制面，产品参照 Tailscale 的核心功能，
不做全家桶。内部名称用 `n2nhub`，代号 NetHive；CLI 仍为 `n2n`，SaaS 子命令统一放在
`n2n hub` 下。用户可见的网络统一称为 `n2nnet`，一个 `n2nnet` 对应一个 n2n community；
平台托管的中心化 supernode 统一称为 `n2nserver`，是所有 `n2nnet` 共享的单一逻辑中继入口；
P2P 失败时的中继角色统一称为 `n2nrelay`，首期由 `n2nserver` 兼任，不单独暴露入口。

## 1. 定位

- n2nhub 的控制面只做三件事：登录与设备授权、n2nnet（= community）管理、地址与密钥下发；
- 数据面沿用 n2n：edge 之间 P2P，失败时经 n2nrelay 中继；n2nserver 中心化部署，对外只有一个逻辑入口，内部通过多租户隔离与负载均衡承载所有 n2nnet；
- 客户端一条命令加入 n2nnet，不暴露 n2n 原始参数。

## 2. 核心功能（精简版）

| 能力 | Tailscale 核心做法 | 本项目 |
| --- | --- | --- |
| 登录 | SSO/OIDC 登录 + 设备授权 | 首期只接一种 OIDC，签发设备令牌 |
| n2nnet | tailnet，邀请设备加入 | 一个 n2nnet = 一个 community，邀请码加入，租户在 n2nserver 内隔离 |
| 地址 | 自动分配虚拟地址 | `-a auto`（SN_ASSIGN），已实现 |
| 密钥 | 控制面分发并轮换节点密钥 | community 密钥由控制面下发 |
| 设备 | 设备列表与在线状态 | 最小字段：名称、地址、最近注册时间 |
| 客户端 | 登录后自动配置隧道 | `n2n hub login` / `n2n hub join`，写配置并启动 |

## 3. 与 n2n 概念的映射

| Tailscale | 本项目/n2n | 说明 |
| --- | --- | --- |
| tailnet | n2nnet（= community） | 一个 n2nnet 对应一个 community；所有 n2nnet 共享同一个 n2nserver |
| 设备 | edge | 设备标识用公钥或 MAC |
| 自动地址 | `-a auto`（SN_ASSIGN） | 默认池 10.128.0.0–10.255.255.0 |
| 中继 | n2nrelay（首期由 n2nserver 兼任） | 单一逻辑入口，内部多租户 + 负载均衡 |

## 4. 技术实现参考

现有 n2n Go 封装样例可参考（cgo 链接 `libn2n.a`、运行标志控制生命周期、平台 TUN/TAP 补丁、
helper 提权），细节见 §6；产品设计不以它为依据。

## 5. 首期边界

- 明确不做：ACL、MagicDNS、subnet router / exit node、审计日志、设备健康检查、用户自建或专属中继。
- 中心化 supernode（n2nserver）由平台统一运维，对外只有一个逻辑地址；容量通过内部多租户与负载均衡扩展。
- 控制面故障不应阻断已配置好的隧道；本地配置与密钥允许离线使用。
- 后续要加的能力作为独立模块叠加，不改变 edge/n2nserver 的核心行为。

## 6. 技术参考索引

- macOS utun 适配与 3.1.1 迁移修正：[N2NGo.md](N2NGo.md) §7.1、[macOS.md](macOS.md) §4.1。
- `-DSKIP_MULTICAST_PEERS_DISCOVERY`：[macOS.md](macOS.md) §3.2。
- 封装样例仓库（作者本机）：`/Users/xtao/Work/n2n/omniedge-cli`。

## 7. 待确认

1. 首期接哪种 OIDC（自建、Google 或 GitHub）？
2. 密钥由控制面下发时，是否允许本地覆盖以支持离线启动？
3. 设备列表的最小字段是否够用（名称、地址、最近注册时间）？
