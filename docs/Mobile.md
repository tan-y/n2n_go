# 移动端与 Web 接入

本文是 [N2NHub.md](N2NHub.md) 的接入层设计，描述手机、平板和浏览器如何访问
`n2nnet`。移动端默认不直接运行 n2n，而是通过 Web 网关或 WireGuard 网关接入。

## 1. 目标与边界

目标：

- 手机浏览器零安装访问 `n2nnet` 内的 Web 应用；
- 需要完整网络时，用成熟客户端接入；
- 移动端不持有 community 密钥，鉴权与授权在网关完成；
- 同一套控制面管理桌面 edge 与移动接入。

首期不做：

- 移动端二层网络、广播/组播；
- 按应用分流（split tunneling）；
- 自建 OIDC 之外的多身份源。

## 2. 方案对比

| 方案 | 移动端形态 | 可访问范围 | 主要代价 |
| --- | --- | --- | --- |
| Web 网关 | 浏览器，零安装 | HTTP/HTTPS 应用 | 不支持任意 TCP/UDP、ICMP |
| 浏览器远程桌面 | 浏览器 | RDP/VNC/SSH 与 Web 应用 | 网关资源开销，交互体验受限 |
| WireGuard 网关 | 官方 WireGuard App | `n2nnet` 的 L3 网段 | 需地址池、路由/NAT 规划，不支持 L2 |

传统 WebVPN 设备依赖 URL 改写，对 SPA、WebSocket 和现代前端不友好，不作为新方案。
需要 Web 接入时，采用“反向代理 + OIDC/设备令牌”的网关。

## 3. 推荐架构

### 3.1 Web 网关（首期）

```text
手机浏览器
   │ HTTPS 443
   ▼
n2nhub Web Gateway（OIDC 登录、按 n2nnet/应用鉴权）
   │ 作为 edge 加入目标 n2nnet
   ▼
内网 Web 应用
```

- 网关是 `n2nnet` 的一个 edge，只暴露 HTTPS；
- 支持反向代理、WebSocket、会话超时与按应用 ACL；
- 每个 `n2nnet` 一个逻辑网关，或多租户共享网关按租户路由；
- 移动端只保存登录态，不接触 community 密钥。

远程桌面复用同一入口：网关后部署 Guacamole 或 Teleport，浏览器访问 RDP/VNC/SSH，
不额外开放端口。

### 3.2 WireGuard 网关（需要完整网络时）

```text
手机 WireGuard App
   │ UDP
   ▼
接入网关（WireGuard + edge）
   │ 路由 / NAT
   ▼
n2nnet 其他节点与目标网段
```

- 移动端使用官方 WireGuard 客户端，系统集成、耗电和后台保活由系统负责；
- 网关一侧同时运行 WireGuard 与 edge，负责把客户端流量路由进 `n2nnet`；
- 客户端地址池与 `n2nnet` 地址池分离，必要时做 NAT；
- 只支持 L3，广播、组播和依赖 L2 的发现协议不可用；
- WireGuard 配置由 `n2nhub` 登录后下发，设备撤销时同步删除公钥。

## 4. 与现有组件的映射

| 组件 | 角色 |
| --- | --- |
| `n2nnet` | 租户网络，对应一个 community |
| `n2nserver` | 中心化注册与协调入口，移动端不直连 |
| `n2nrelay` | P2P 失败时的中继，首期由 `n2nserver` 兼任 |
| Web Gateway | 新增接入层，作为 edge 加入 `n2nnet`，提供 L7 访问 |
| WireGuard Gateway | 新增接入层，作为 edge 加入 `n2nnet`，提供 L3 访问 |

## 5. 安全要求

- 移动端登录使用 OIDC，网关签发短期设备令牌；
- Web 模式下 community 密钥只保存在服务端；
- 网关到控制面使用 mTLS，按 `n2nnet` 做租户隔离；
- `n2nserver`、`n2nrelay` 不因移动接入额外暴露管理端口；
- 设备撤销后，令牌与 WireGuard 公钥同时失效；
- 审计日志作为后续独立模块，不阻塞首期。

## 6. 落地顺序

1. Web Gateway + OIDC，打通 HTTP/HTTPS 应用；
2. 接入 Guacamole 或 Teleport，覆盖远程桌面；
3. 有完整网络需求时增加 WireGuard 网关。

## 7. 验收标准

- 手机浏览器无需安装客户端即可访问授权应用；
- 未授权 `n2nnet` 的用户无法通过网关访问目标服务；
- 撤销设备后，旧令牌和 WireGuard 配置不再可用；
- WireGuard 客户端可访问 `n2nnet` 内指定网段，且不影响其他租户。

## 8. 待确认

1. 首期 Web 网关是每租户独立，还是共享网关按租户路由？
2. WireGuard 客户端地址池、DNS 与 NAT 策略如何规划？
3. 远程桌面首期选 Guacamole 还是 Teleport？
4. 是否需要浏览器端的会话录屏或审计？
