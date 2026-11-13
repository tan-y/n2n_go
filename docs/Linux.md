# Linux 首期实施方案

本文档对应 [N2NGo.md](N2NGo.md) 的 Linux 首期落地计划，与 [macOS.md](macOS.md) 并列。沿用同样的约束：本阶段不做任何 Git 操作，n2n 源码直接使用本地路径 `/Users/xtao/Work/n2n/n2n`，构建时复制到工作目录。

Linux 与 macOS 的关键差异在设备层：Linux 内核原生提供 TAP（二层以太网），n2n 的 `tuntap_linux.c` 可以直接复用，不需要 macOS 那种 utun（三层）到以太网的转换。因此 Linux 侧代码改动更小，主要工作是构建环境、容器化运行验收、权限模型和跨发行版兼容。

## 0. 本机环境探测

| 项目 | 现状 | 结论 |
| --- | --- | --- |
| 宿主系统 | macOS 26.7.2，arm64 | 只负责 Docker/OrbStack 调用，不参与 Linux 产物编译 |
| 容器运行时 | Docker 29.8.2，OrbStack 2.2.5，Linux 内核 7.2.12-orbstack arm64 | 可用 |
| linux/amd64 模拟 | 已验证 `docker run --platform linux/amd64` 返回 `x86_64` | 可在 Apple Silicon 上产出 amd64 产物（较慢） |
| /dev/net/tun | 已验证容器内存在 `crw-rw-rw- 10,200`（`--device /dev/net/tun --cap-add NET_ADMIN`） | 可以创建 TAP |
| 容器内工具链 | ubuntu:22.04 上已实测 autoconf/automake/gcc/make，成功产出 `libn2n.a` | 构建路径可行 |
| Go | 宿主 go1.25.5 darwin/arm64；容器内需另装 Linux 版 | 由镜像提供 |
| zig | 未安装 | 不作为主路径 |
| Nix | 2.35.3 可用 | Linux 构建不需要 |

结论：Linux 的构建、运行和冒烟测试全部在 Linux 容器内完成；宿主只负责调用 Docker，不承担 Linux 交叉编译。

## 1. 首期范围（MVP）

**实现**

- 单一子命令 `n2n edge`，前台运行，需要 root 或 `CAP_NET_ADMIN`。
- 目标平台：`linux/amd64`、`linux/arm64`。
- 设备层使用内核 TAP（`/dev/net/tun` + `IFF_TAP|IFF_NO_PI`），收发真实二层以太网帧。
- 静态 IPv4 地址（`-a IPv4/CIDR`）。
- 两套 libc 产物：`glibc`（常规发行版）与 `musl` 静态（单文件、跨发行版）。
- 构建脚本、工具链镜像、容器冒烟测试。

**明确不做（后续迭代）**

- `supernode` 子命令（沿用 n2n 自带 `supernode` 做测试）；
- `armv7` / `386` / `mips` 与 OpenWrt（OpenWrt 单独平台文档）；
- DHCP 地址模式、IPv6；
- daemon 化、systemd 单元、安装包（deb/rpm）；
- 头部加密（`-H`）、联邦认证（`-J/-P`）；
- 非 root 的 helper / `setcap` 交付形态（首期仍要求 root，见 5.1）。

**验收标准**

1. `scripts/build-linux.sh arm64 musl` 在容器内成功产出 `libn2n.a` 与 `bin/n2n-linux-arm64-musl`，`version` 可运行；
2. 一个 supernode 容器加两个 edge 容器，两个 edge 均注册成功，容器间 overlay 地址可双向 `ping` 通；
3. 发送 `SIGINT`/`SIGTERM` 后 edge 退出码为 0，TAP 接口随 fd 关闭被内核删除；
4. 参数错误（社区超长、非法 CIDR、未知算法、非法设备名）在 Go 层直接报错，不进入 C 层。

## 2. 与 macOS 方案的差异

| 维度 | macOS | Linux |
| --- | --- | --- |
| 设备层 | utun（三层），需要自写 `tuntap_osx.c` | TAP（二层），直接复用 n2n 原生 `tuntap_linux.c` |
| 帧格式 | 伪以太网头 + IPv4 载荷 | 原生以太网帧 |
| ARP / 广播 | ARP 无意义，直接丢弃 | ARP、广播正常可用 |
| IPv6 | 首期不支持，显式丢弃 | 首期不启用，但具备后续支持基础 |
| MAC | 适配层自行生成 | 内核 TAP 接受任意本地管理 MAC |
| 权限 | root | root 或 `CAP_NET_ADMIN` |
| 构建 | Nix + Apple clang + macOS SDK | 容器内 gcc/clang + Linux headers |
| 跨发行版 | 不涉及 | 需要 glibc 基线或 musl 静态 |
| 同机双节点 | 用 `/32` 规避路由冲突，ICMP 不回包 | 用独立 netns/容器，`/24` 即可双向 ping 通 |

Linux 不需要 L3→L2 桥接，因此 [platform/macos/tuntap_osx.c](../platform/macos/tuntap_osx.c) 中的伪以太头、AF 头处理逻辑在 Linux 侧完全不用实现。

## 3. 目录结构（新增部分）

```
n2n_go/
├── docker/linux/
│   ├── Dockerfile.glibc          # Debian 11 基线（glibc 2.31）
│   └── Dockerfile.musl           # Alpine（musl 静态）
├── patches/
│   └── n2n-tuntap-linux.patch    # tuntap_linux.c 的小修正
├── internal/
│   ├── config/
│   │   ├── device_darwin.go      # mac：utun / utunN 校验
│   │   └── device_linux.go       # linux：空名自动分配、IFNAMSIZ 校验
│   └── n2n/
│       ├── link_linux_amd64.go   # cgo LDFLAGS：linux/amd64/libn2n.a
│       └── link_linux_arm64.go   # cgo LDFLAGS：linux/arm64/libn2n.a
├── scripts/
│   ├── build-linux.sh            # 宿主入口：docker build/run + 产物落盘
│   ├── build-n2n-linux.sh        # 容器内：编译 n2n 静态库 + Go 二进制
│   ├── smoke-test-linux.sh       # 容器双节点联调
│   └── docker-compose.linux.yml  # supernode + edge1 + edge2
├── build/
│   └── n2n-linux-<arch>-<libc>/   # 中间源码与目标文件
├── third_party/lib/linux/
│   ├── amd64/                    # cgo 实际链接的规范路径
│   ├── amd64-glibc/              # 变体归档
│   ├── amd64-musl/
│   ├── arm64/
│   ├── arm64-glibc/
│   └── arm64-musl/
└── bin/
    ├── n2n-linux-amd64-glibc
    ├── n2n-linux-amd64-musl
    ├── n2n-linux-arm64-glibc
    └── n2n-linux-arm64-musl
```

说明：

- `build/`、`third_party/lib/`、`bin/` 仍为生成目录，`make clean` 可删除。
- cgo 只指向 `third_party/lib/linux/<arch>/libn2n.a` 这个规范路径；构建脚本把当前选择的 libc 变体复制到该位置。同一时刻只激活一个变体，CI 按变体串行构建。
- `platform/linux/` 目录首期不建：`tuntap_linux.c` 通过 patch 修正，避免复制整份文件；如果后续补丁超过约 30 行，再改为整文件替换（与 macOS 的 `platform/macos/` 方式一致）。

## 4. 编译环境与产物

### 4.1 构建方式选型

| 方案 | 做法 | 评价 |
| --- | --- | --- |
| A. 容器内原生构建（推荐） | 每个目标架构跑一个对应平台的 Linux 容器，容器内装 Go 与 autotools，直接产出该架构产物 | 与运行环境一致，最可靠；本机 Docker/OrbStack 已验证可用 |
| B. Linux 主机/CI runner 原生构建 | 在 amd64/arm64 runner 上直接执行同一套 `build-n2n-linux.sh` | CI 首选，无模拟开销 |
| C. zig cc 交叉编译 | 在 macOS 上用 `zig cc -target <triple>-linux-musl` 构建库与 Go 的 C 工具链 | 本机未安装 zig；只作为快速迭代的可选路径，不作为验收路径 |

结论：主路径采用 A（本地开发）与 B（CI）。容器架构必须与目标架构一致；在 Apple Silicon 上构建 `linux/amd64` 依赖 QEMU 模拟，能工作但明显更慢。

### 4.2 工具链镜像

镜像只提供工具链，源码通过 bind mount 进入，产物写回工作目录。

`docker/linux/Dockerfile.glibc`（glibc 基线产物）：

```dockerfile
FROM debian:11-slim

ARG GO_VERSION=1.25.5
ARG TARGETARCH
ARG GO_URL=https://golang.google.cn/dl

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl build-essential autoconf automake pkg-config \
      linux-libc-dev rsync patch \
 && case "$TARGETARCH" in \
      amd64) GOARCH=amd64 ;; \
      arm64) GOARCH=arm64 ;; \
      *) echo "unsupported arch: $TARGETARCH" >&2; exit 1 ;; \
    esac \
 && curl -fsSL "$GO_URL/go${GO_VERSION}.linux-${GOARCH}.tar.gz" \
      | tar -C /usr/local -xz \
 && rm -rf /var/lib/apt/lists/*

ENV PATH=/usr/local/go/bin:$PATH
```

`docker/linux/Dockerfile.musl`（静态产物）：

```dockerfile
FROM alpine:3.20

ARG GO_VERSION=1.25.5
ARG TARGETARCH
ARG GO_URL=https://golang.google.cn/dl

RUN apk add --no-cache ca-certificates curl build-base autoconf automake pkgconf \
      linux-headers rsync patch \
 && case "$TARGETARCH" in \
      amd64) GOARCH=amd64 ;; \
      arm64) GOARCH=arm64 ;; \
      *) echo "unsupported arch: $TARGETARCH" >&2; exit 1 ;; \
    esac \
 && curl -fsSL "$GO_URL/go${GO_VERSION}.linux-${GOARCH}.tar.gz" \
      | tar -C /usr/local -xz

ENV PATH=/usr/local/go/bin:$PATH
```

要点：

- 不在镜像里用发行版自带的 Go（Debian 11 / Ubuntu 22.04 的 apt 版本过旧，无法满足 `go.mod` 的 `go 1.25`），统一安装官方 tarball；国内网络使用 `golang.google.cn` 或 `mirrors.aliyun.com/golang`。
- `glibc` 基线选 Debian 11（glibc 2.31），覆盖 Debian 11+、Ubuntu 20.04+、RHEL 8+；若用 Ubuntu 22.04 构建，产物要求 glibc ≥ 2.35，兼容面会变窄。
- `musl` 产物完全静态，不依赖目标发行版的 glibc，是首期默认推荐产物。
- 本机已实测 `ubuntu:22.04` 容器内可完成 autogen/configure/`make libn2n.a`；Debian 11/Alpine 镜像只需在同一脚本下复跑一次即可确认（见任务 T1）。

### 4.3 容器内构建 n2n 静态库

`scripts/build-n2n-linux.sh`（在容器内执行）关键步骤：

```bash
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH=${ARCH:-arm64}
LIBC=${LIBC:-musl}
case "$ARCH" in
  amd64|arm64) ;;
  *) echo "unsupported arch: $ARCH" >&2; exit 2 ;;
esac

WORK="$ROOT/build/n2n-linux-$ARCH-$LIBC"
LIB="$ROOT/third_party/lib/linux/$ARCH-$LIBC"

rm -rf "$WORK"
mkdir -p "$WORK"
rsync -a --exclude build --exclude bin --exclude .git \
      "${N2N_SRC:-/n2n-src}/" "$WORK/"

cd "$WORK"
patch -p1 --quiet -i "$ROOT/patches/n2n-keep-running.patch"
patch -p1 --quiet -i "$ROOT/patches/n2n-tuntap-linux.patch"

./autogen.sh
./configure CFLAGS="-O2 -Wall -DSKIP_MULTICAST_PEERS_DISCOVERY"
make libn2n.a

mkdir -p "$LIB" "$ROOT/third_party/lib/linux/$ARCH"
cp libn2n.a "$LIB/libn2n.a"
cp libn2n.a "$ROOT/third_party/lib/linux/$ARCH/libn2n.a"
```

说明：

- Linux 不需要替换 `tuntap_linux.c`；`N2N_OBJS` 中本来就包含 `src/tuntap_linux.o`，文件内部用 `__linux__` 隔离（见 [Makefile](file:///Users/xtao/Work/n2n/n2n/Makefile#L94-L97)）。
- 仍然保留 `-DSKIP_MULTICAST_PEERS_DISCOVERY`，与 macOS 首期一致，避免局域网组播发现带来的权限、噪音和容器网络差异；后续做局域网发现时再评估放开。
- `--enable-pthread` 保持默认关闭；n2n 只在启用该选项时才链接 pthread（见 [configure.ac](file:///Users/xtao/Work/n2n/n2n/configure.ac#L109-L116)）。
- 不启用 `zstd`/`openssl`/`libcap`/`pcap`/`miniupnpc`/`natpmp`，避免额外系统库依赖。

### 4.4 Go 侧 cgo 配置

新增按架构与 OS 约束的链接文件：

`internal/n2n/link_linux_amd64.go`：

```go
//go:build linux && amd64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/linux/amd64/libn2n.a
import "C"
```

`internal/n2n/link_linux_arm64.go` 同理，把路径换成 `linux/arm64`。

要点：

- 直接链接 `libn2n.a`，不使用 `-ln2n`，避免运行时搜索路径问题。
- Linux 默认不需要额外系统库：n2n 默认未启用 pthread，Go 运行时会自行处理线程库；不启用 zstd/openssl 时也不需要 `-lzstd`/`-lcrypto`。如果后续开启 `--enable-pthread`，在 LDFLAGS 中补 `-lpthread`。
- cgo 的 CFLAGS 沿用 [edge.go](../internal/n2n/edge.go#L5-L9) 中的 `-I third_party/n2n-src/include -DSKIP_MULTICAST_PEERS_DISCOVERY`，Linux 无需额外头文件路径。
- 一个变体构建完成后再构建另一个变体，避免规范路径 `third_party/lib/linux/<arch>/libn2n.a` 被覆盖导致链接错库。

### 4.5 宿主入口脚本

`scripts/build-linux.sh <arch> <glibc|musl>`：

```bash
docker build --platform "linux/$ARCH" \
  -f "docker/linux/Dockerfile.$LIBC" \
  -t "n2n-go-build:linux-$ARCH-$LIBC" docker/linux

docker run --rm --platform "linux/$ARCH" \
  -e ARCH="$ARCH" -e LIBC="$LIBC" -e N2N_SRC=/n2n-src \
  -v "$PWD":/work -w /work \
  -v "${N2N_SRC_HOST:-/Users/xtao/Work/n2n/n2n}":/n2n-src:ro \
  "n2n-go-build:linux-$ARCH-$LIBC" \
  ./scripts/build-n2n-linux.sh
```

容器内随后执行 Go 构建：

```bash
export CGO_ENABLED=1 GOOS=linux GOARCH="$ARCH"
if [ "$LIBC" = "musl" ]; then
  go build -trimpath \
    -ldflags '-linkmode external -extldflags "-static"' \
    -o "bin/n2n-linux-$ARCH-musl" ./cmd/n2n
else
  go build -trimpath -o "bin/n2n-linux-$ARCH-glibc" ./cmd/n2n
fi
```

要点：

- `musl` 静态产物必须显式 `-linkmode external -extldflags "-static"`；只设置 `CGO_ENABLED=1` 仍会动态链接 musl。
- 可选 `GOPROXY=https://goproxy.cn,direct`；项目当前没有第三方 Go 依赖，影响不大。
- 可选把 `~/.cache/go-build` 挂进容器，减少重复构建时间。
- bind mount 写回产物在 Linux 宿主上可能变成 root 所有；macOS 的 OrbStack/Docker Desktop 一般会做 uid 映射。CI 中建议在容器内打包产物，或使用 `--user` 运行 Go 构建阶段。

### 4.6 Makefile 目标

| 目标 | 行为 |
| --- | --- |
| `make linux-arm64` | `scripts/build-linux.sh arm64 musl` |
| `make linux-amd64` | `scripts/build-linux.sh amd64 musl` |
| `make linux-glibc-arm64` | `scripts/build-linux.sh arm64 glibc` |
| `make linux-glibc-amd64` | `scripts/build-linux.sh amd64 glibc` |
| `make linux-smoke` | 构建 arm64 musl 后执行 `scripts/smoke-test-linux.sh` |
| `make linux-clean` | 删除 `build/n2n-linux-*`、`third_party/lib/linux/`、`bin/n2n-linux-*` |
| `make clean` | 追加删除上述 Linux 产物 |

### 4.7 常见构建问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `Please run ./configure` | 未生成 `config.mak` | 执行 `autogen.sh && ./configure` |
| `configure: error: cannot find C compiler` | 容器架构与目标架构不一致 | `--platform linux/<arch>` 必须与 `GOARCH` 一致 |
| `linux/if_tun.h: No such file` | 缺 Linux 内核头 | glibc 镜像装 `linux-libc-dev`；Alpine 装 `linux-headers` |
| cgo 报 `unrecognized command-line option '-arch'` | 误用了 macOS 的 CFLAGS | Linux 构建脚本不要带 `-arch`、`-mmacosx-version-min` |
| `ld: cannot find -ln2n` | 规范路径库缺失或变体未复制 | 先跑 `build-n2n-linux.sh`，确认 `third_party/lib/linux/<arch>/libn2n.a` 存在 |
| 运行时报 `GLIBC_2.xx not found` | glibc 基线过新 | 改用 musl 静态产物，或在更老基线镜像中重建 |
| TUNSETIFF 返回 `Operation not permitted` | 缺 `CAP_NET_ADMIN` | 以 root 运行，或容器加 `--cap-add NET_ADMIN --device /dev/net/tun` |

## 5. Linux TAP 适配

### 5.1 内核与权限

- 依赖 `/dev/net/tun` 与内核 `CONFIG_TUN`；多数发行版默认已加载 `tun` 模块，缺失时 `sudo modprobe tun`。
- 创建/配置 TAP 需要 `CAP_NET_ADMIN`：`TUNSETIFF`、`SIOCSIFHWADDR`、`SIOCSIFADDR`、`SIOCSIFNETMASK`、`SIOCSIFMTU`、`SIOCSIFFLAGS` 均受该能力约束；不需要 `CAP_NET_RAW`。
- 首期沿用 CLI 的 `os.Geteuid() == 0` 检查（[edge.go](../internal/cli/edge.go#L92-L95)），提示语改为 root 或 `CAP_NET_ADMIN`。
- 容器需要：

```bash
docker run --cap-add NET_ADMIN --device /dev/net/tun ...
```

或测试场景直接 `--privileged`。Kubernetes 需 privileged / device plugin，首期不展开。
- 可选的非 root 方案：`setcap cap_net_admin+ep bin/n2n-linux-amd64-musl`。但当前 CLI 的 euid 检查会先拒绝，若采用该方案需改成“尝试创建 TAP，失败时把 EPERM 翻译成权限提示”，首期不做。

### 5.2 复用 `tuntap_linux.c` 与补丁

Linux 的 TAP 逻辑已经完整，不需要像 macOS 那样重写。首期只做一组小修正，放在 `patches/n2n-tuntap-linux.patch`：

| 问题 | 现状（[tuntap_linux.c](file:///Users/xtao/Work/n2n/n2n/src/tuntap_linux.c)） | 修正 |
| --- | --- | --- |
| `if_idx` 取值错误 | `device->if_idx = if_nametoindex(dev)`（L251）；`dev` 为空时返回 0，自动分配 `tapN` 时拿不到真实索引 | 改为 `if_nametoindex(device->dev_name)` |
| `mtu` 字段未赋值 | 只在 `SIOCSIFMTU` 中设置内核 MTU，`device->mtu` 结构体字段保持 0 | 补 `device->mtu = (uint16_t)mtu` |
| 关闭后 fd 未复位 | `tuntap_close` 只 `close(fd)`，不置 `-1`（L269-L272） | 置 `fd = -1`，保证幂等，与 macOS 适配层一致 |
| netlink 判定含义不清 | `nh->nlmsg_type == NETLINK_GENERIC`（L234）；该值与 `RTM_NEWLINK` 恰好同为 16，靠巧合成立 | 改为 `RTM_NEWLINK`，并加超时兜底：2 秒未收到事件则用 `SIOCGIFFLAGS` 校验 `IFF_UP`，避免受限容器/异常内核下永久阻塞 |
| 错误路径 fd 泄漏 | netlink socket/bind/ioctl socket 失败后直接返回，未关闭已打开的 TAP fd（L179-L214） | 所有失败路径统一 `close(device->fd)` 并置 `-1` |
| 设备名无显式结尾 | `strncpy(device->dev_name, ifr.ifr_name, MIN(IFNAMSIZ, sizeof(devstr_t)))`（L159） | 补 `device->dev_name[sizeof(device->dev_name)-1] = '\0'` |
| MAC 解析失败被忽略 | `str2mac` 返回值未检查（L163） | 解析失败时告警并回退到随机 MAC；Go 层已前置校验，补丁只做兜底 |

保留原生行为：

- `/dev/net/tun` + `TUNSETIFF` 申请 `IFF_TAP|IFF_NO_PI`，读写的是完整以太网帧；
- 设置 MAC、IP、掩码、MTU，最后置 `IFF_UP|IFF_RUNNING`；
- 通过 `NETLINK_ROUTE` 订阅链路事件，等待接口进入 up 状态；
- `tuntap_read`/`tuntap_write` 直接读写 fd，无需任何帧格式转换。

### 5.3 设备命名与 Go 层校验

macOS 的设备名必须是 `utun`/`utunN`；Linux 则允许任意合法接口名（≤ 15 字节），也允许留空由内核自动分配 `tapN`。因此把相关逻辑拆到平台文件：

- `internal/config/device_darwin.go`：保持现有 `utun`/`utunN` 规则；
- `internal/config/device_linux.go`：空名合法（自动分配）；非空时校验长度 ≤ 15、字符集 `[A-Za-z0-9_.-]`，拒绝 `/`、空白、`:` 等；
- [config.go](../internal/config/config.go#L173-L181) 与 `DefaultEdgeConfig` 中的 `Device: "utun"` 改为调用平台函数 `defaultDeviceName()` / `validateDeviceName()`；
- [CLI edge.go](../internal/cli/edge.go#L52-L53) 当前把 `-d` 默认值硬编码为 `utun`，改为取 `cfg.Device` 的平台默认值，帮助文本改为“Linux 默认自动分配 tapN；macOS 默认 utunN”；
- [edge_glue.c](../internal/n2n/edge_glue.c#L178-L180) 的兜底设备名从 `"utun"` 改为 `""`，错误信息从 `failed to open utun device` 改为 `failed to open TUN/TAP device`；
- 新增 C 接口 `n2n_go_edge_device_name()` 与 Go 方法 `Edge.DeviceName()`，在 `NewEdge` 成功后打印实际设备名（自动分配的 `tapN` 对用户可见）；`tuntap_priv_conf.tuntap_dev_name` 已经使用 `e->dev.dev_name`，重开设备时能拿到真实名字。

### 5.4 参数映射

| CLI 参数 | C 目标 | Linux 说明 |
| --- | --- | --- |
| `-c, --community` | `conf.community_name` | 16 字节，超长截断并告警 |
| `-l, --supernode` | `edge_conf_add_supernode` | 可重复；首期取第一个 |
| `-a, --ip` | `tuntap_open` 的 `device_ip` / `device_mask` | `/24` 常见；双节点联调放在独立 netns/容器中即可，不使用同 netns 双节点 |
| `-k, --key` | `conf.encrypt_key` | `strdup` 后交给 conf |
| `-A, --cipher` | `conf.transop_id` | `none/aes/cc20/speck/tf` → `N2N_TRANSFORM_ID_*` |
| `-z, --compress` | `conf.compression` | `none/lzo`（zstd 首期不启用） |
| `-d, --device` | `tuntap_open` 的 `dev` | 空名自动分配 `tapN`；也可指定 `tap0`、`n2n0` 等 |
| `-m, --mac` | `tuntap_open` 的 `device_mac` | 缺省由 Go 生成随机本地管理地址；TAP 接受该 MAC |
| `-M, --mtu` | `tuntap_open` 的 `mtu` + `dev.mtu` | 默认 1290，范围 68–1500 |
| `-p, --local-port` | `conf.local_port` | 0 表示自动 |
| `-t, --mgmt-port` | `conf.mgmt_port` | 同一 netns 多进程测试时需互不相同；独立容器可都用默认值 |
| `-i / -L` | `conf.register_interval/register_ttl` | 默认沿用 n2n |
| `-r, --routing` | `conf.allow_routing` | 布尔 |
| `-E` | `conf.drop_multicast=false` | 接受 IPv4 组播与 IPv6 邻居发现（与原生 n2n 语义一致），默认仍丢弃 |
| `--drop-multicast` | `conf.drop_multicast=true` | 显式丢弃组播（默认行为）；广播/ARP 不受影响 |
| `-T, --tos` | `conf.tos` | 0–255 |
| `-I, --description` | `conf.dev_desc` | 最长 `N2N_DESC_SIZE` |

### 5.5 CLI 行为

- 启动前权限检查：非 root 时提示“创建 TAP 需要 root 或 CAP_NET_ADMIN”，首期不引入 helper。
- 日志统一走 stderr；`edge_glue.c` 已把 n2n 的 traceFile 重定向到 stderr 的 dup，Linux 行为一致。
- 信号处理：`signal.Notify(SIGINT, SIGTERM)` → `Stop()` → 等待 run goroutine 退出 → 正常返回；Linux 的 TAP 是非持久设备，fd 关闭后内核自动删除接口和路由，不需要 `ip link del`。
- `n2n version` 输出 Go 版本 + n2n `PACKAGE_VERSION`。
- 不实现 n2n 原生的 `-f` 后台 fork；CLI 始终前台运行，后台化交给 systemd/容器（后续迭代）。

## 6. 测试方案

### 6.1 构建与静态检查

```bash
./scripts/build-linux.sh arm64 musl
file third_party/lib/linux/arm64-musl/libn2n.a
docker run --rm --platform linux/arm64 -v "$PWD":/work -w /work \
  n2n-go-build:linux-arm64-musl ./bin/n2n-linux-arm64-musl version

# glibc 产物基线检查（符号版本应不高于构建环境的 glibc）
docker run --rm --platform linux/arm64 -v "$PWD":/work -w /work \
  n2n-go-build:linux-arm64-glibc sh -c \
  'ldd bin/n2n-linux-arm64-glibc; objdump -T bin/n2n-linux-arm64-glibc | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail'
```

静态产物应满足：

```bash
docker run --rm --platform linux/arm64 -v "$PWD":/work -w /work \
  n2n-go-build:linux-arm64-musl sh -c 'file bin/n2n-linux-arm64-musl; ldd bin/n2n-linux-arm64-musl || true'
```

`file` 显示 `ELF 64-bit LSB executable, ARM aarch64, statically linked`；`ldd` 显示没有动态依赖（glibc 的 `ldd` 输出 `not a dynamic executable`，musl 环境通常输出 `statically linked`）。

### 6.2 容器双节点联调（主路径）

运行镜像与工具链镜像分开：工具链镜像只负责编译，运行镜像从 `alpine:3.20` 构建，装入 `iproute2`（`ip` 命令）和 `iputils`（`ping`），再复制 `n2n` 与测试用 `supernode`（在 `build/n2n-linux-<arch>-<libc>/` 中 `make supernode` 产出）。`scripts/docker-compose.linux.yml` 示意：

```yaml
services:
  supernode:
    image: n2n-go-linux:arm64-musl
    command: ["/usr/local/bin/supernode", "-f", "-p", "7777"]

  edge1:
    image: n2n-go-linux:arm64-musl
    cap_add: ["NET_ADMIN"]
    devices: ["/dev/net/tun"]
    stop_grace_period: 15s
    command:
      - /usr/local/bin/n2n
      - edge
      - -c
      - smoke
      - -k
      - smoke
      - -a
      - 10.0.0.1/24
      - -l
      - supernode:7777
      - -d
      - tap0

  edge2:
    image: n2n-go-linux:arm64-musl
    cap_add: ["NET_ADMIN"]
    devices: ["/dev/net/tun"]
    stop_grace_period: 15s
    command:
      - /usr/local/bin/n2n
      - edge
      - -c
      - smoke
      - -k
      - smoke
      - -a
      - 10.0.0.2/24
      - -l
      - supernode:7777
      - -d
      - tap0
```

验证步骤：

```bash
docker compose -f scripts/docker-compose.linux.yml up -d
# 等两侧日志出现 [OK] edge <<< ... >>> supernode
docker compose -f scripts/docker-compose.linux.yml exec edge1 ping -c 3 10.0.0.2
docker compose -f scripts/docker-compose.linux.yml exec edge1 ip -s link show tap0
docker compose -f scripts/docker-compose.linux.yml exec edge1 ip neigh
```

断言：

- 两个 edge 都在 30 秒内完成首次注册；
- `ping` 0% 丢包（Linux TAP 是真实二层，容器是独立 netns，不存在 macOS 同机 ICMP 限制）；
- 两侧 `tap0` 的 RX/TX 计数随 ping 增长；
- `ip neigh` 中出现对端 MAC（ARP 通过 n2n 广播送达）。

退出与清理：

```bash
docker compose -f scripts/docker-compose.linux.yml stop
# edge 容器的退出码应为 0（可用 docker compose up --abort-on-container-exit 捕获）
# TAP 位于容器 netns，容器销毁时随 netns 一起消失；
# 若需显式验证接口删除，使用 6.3 的 netns 方案，在 edge 收到 SIGTERM 后执行
#   ip netns exec edge1 ip link show tap0   # 应报 Device "tap0" does not exist
```

注意：

- 每个 edge 容器必须加 `NET_ADMIN` 与 `/dev/net/tun`，否则 `TUNSETIFF` 直接失败；
- 两个 edge 在不同容器/不同 netns 中，管理端口可以用默认值，不会冲突；
- 若用 `--privileged` 跑测试，可省去单独的 cap/device 配置，但生产不建议。

### 6.3 宿主机 netns 联调（备选）

在原生 Linux 主机上，不用 Docker 也可以把两个 edge 放进独立 netns，避免同 netns 同网段的路由冲突：

```bash
ip netns add edge1
ip netns add edge2
# 每个 netns 建一对 veth 与宿主相连，用于访问宿主机上的 supernode
ip link add veth1 type veth peer name veth1-ns
ip link set veth1-ns netns edge1
ip addr add 192.168.50.1/24 dev veth1
ip link set veth1 up
ip netns exec edge1 ip addr add 192.168.50.2/24 dev veth1-ns
ip netns exec edge1 ip link set veth1-ns up
# edge2 同理，使用 192.168.50.3

# 宿主机启动 supernode，两个 edge 分别在各 netns 后台启动
./supernode -f -p 7777 &
ip netns exec edge1 ./bin/n2n-linux-amd64-musl edge \
  -c smoke -k smoke -a 10.0.0.1/24 -l 192.168.50.1:7777 -d tap0 &
ip netns exec edge2 ./bin/n2n-linux-amd64-musl edge \
  -c smoke -k smoke -a 10.0.0.2/24 -l 192.168.50.1:7777 -d tap0 &

# 等待两侧注册后验证
sleep 15
ip netns exec edge1 ping -c 3 10.0.0.2

# 清理：先停进程，再删 netns
pkill -f "bin/n2n-linux-amd64-musl edge" || true
pkill -f "supernode -f -p 7777" || true
ip netns delete edge1 edge2
```

这条路径只用于原生 Linux 开发机；macOS 上没有 netns，必须走容器方案。

### 6.4 退出与残留检查

- Linux 的 TAP 不是持久设备（未使用 `TUNSETPERSIST`），进程退出后接口自动消失；
- 若接口仍存在，说明进程未退出或使用了持久化配置，按 `pkill -f "bin/n2n-linux"` 处理后复查；
- 路由随接口删除自动清理；容器方案则随 netns 销毁。

## 7. 任务拆解与验收

| 序号 | 任务 | 验收 |
| --- | --- | --- |
| T1 | `docker/linux/` 两个 Dockerfile + `scripts/build-linux.sh` | 两个镜像可构建；容器内 `go version`、`autoconf --version` 正常 |
| T2 | `scripts/build-n2n-linux.sh` 产出 amd64/arm64 的 glibc 与 musl 静态库 | `third_party/lib/linux/<arch>-<libc>/libn2n.a` 存在；`file` 架构正确 |
| T3 | `patches/n2n-tuntap-linux.patch` 按 5.2 修正 | 补丁可重复应用；`make libn2n.a` 无错误；无 fd 泄漏路径 |
| T4 | 平台化设备名校验 + CLI 默认值 + 实际设备名输出 | `GOOS=linux` 下 `go build ./...` 通过；Linux 单测在容器内通过 |
| T5 | cgo 链接文件 + 胶水层错误信息中性化 + `DeviceName()` | 容器内 `n2n edge` 能创建 `tapN` 并打印真实名字 |
| T6 | `docker-compose.linux.yml` + `smoke-test-linux.sh` | 两 edge 注册成功，双向 ping 通，SIGTERM 退出码 0，TAP 消失 |
| T7 | musl 静态产物与 glibc 基线验证 | 静态产物 `ldd` 无动态依赖；glibc 产物符号版本符合基线 |
| T8 | Makefile 目标与 `make clean` 覆盖 Linux 产物 | `make linux-arm64`、`make linux-smoke`、`make linux-clean` 可执行 |

## 8. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| glibc 基线过新 | 老发行版无法运行 | 默认交付 musl 静态；glibc 产物在 Debian 11 基线构建 |
| `/dev/net/tun` 不存在或未加载 | 无法创建 TAP | 启动前检查设备；提示 `modprobe tun`；容器显式挂载设备 |
| 容器缺 `CAP_NET_ADMIN` | `TUNSETIFF` 失败 | 文档明确 `--cap-add NET_ADMIN --device /dev/net/tun`；测试可用 privileged |
| netlink 等待卡住 | edge 启动挂起 | 补丁加超时 + `SIOCGIFFLAGS` 兜底（见 5.2） |
| 同机双节点路由冲突 | ping 不通、误判 | 首期测试固定用独立 netns/容器，不承诺同 netns 双节点互通 |
| n2n 升级导致补丁失效 | 构建中断 | 补丁脚本化；CI 在每次升级时先应用补丁再编译 |
| amd64 模拟构建慢 | 本地迭代效率低 | CI 用原生 amd64 runner；本地优先 arm64，必要时 buildx 缓存 |
| 静态 musl 与 NSS | 若后续启用 `-u/-g`，用户/组解析受限 | 首期不启用权限下降；后续改用 glibc 产物或显式数字 uid/gid |
| 非 root 运行诉求 | 用户不愿 sudo | 后续迭代引入 setcap 或 helper，并改造权限检测 |
| SELinux/AppArmor 限制 | TAP/设备操作被拒 | 容器测试环境放宽；生产按发行版策略单独验证 |
| 32 位/嵌入式架构 | 覆盖不足 | 首期只做 amd64/arm64；armv7/mips 归入 OpenWrt/后续迭代 |

## 9. 首期交付物

- 可执行文件：`bin/n2n-linux-amd64-{glibc,musl}`、`bin/n2n-linux-arm64-{glibc,musl}`
- 静态库：`third_party/lib/linux/<arch>-<libc>/libn2n.a`
- 构建镜像：`docker/linux/Dockerfile.glibc`、`docker/linux/Dockerfile.musl`
- 构建脚本：`scripts/build-linux.sh`、`scripts/build-n2n-linux.sh`
- 补丁：`patches/n2n-tuntap-linux.patch`
- 测试：`scripts/docker-compose.linux.yml`、`scripts/smoke-test-linux.sh`
- 文档：本文件 + `docs/N2NGo.md` + `docs/macOS.md`

## 10. 不做 Git 操作的说明

- 不执行 `git init`、不添加 `.gitignore`、不引入 submodule；
- n2n 源码每次从 `N2N_SRC`（默认 `/Users/xtao/Work/n2n/n2n`）复制，构建产物与源码分离；
- 若后续需要固定版本，再单独补充 Git 方案。

## 11. 待确认问题

1. 首期默认交付是否以 musl 静态为主、glibc 仅作兼容产物？
2. `armv7` / `386` 是否需要在首期就覆盖，还是等 OpenWrt 方案一起做？
3. 是否需要在首期提供 systemd unit 与 deb/rpm 安装包？
4. 非 root 运行是否进入首期（`setcap` 或 helper），还是继续保持 root？
5. CI 是否统一用 buildx 矩阵同时产出 amd64/arm64 与 glibc/musl 四种组合？
