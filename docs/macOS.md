# macOS 首期实施方案

本文档对应 [N2NGo.md](N2NGo.md) 的 macOS 首期落地计划，与 [Linux.md](Linux.md)、[Windows.md](Windows.md) 并列。n2n 源码以 Git submodule 固定在 `third_party/n2n`（3.1.1），构建时复制到工作目录。

## 0. 本机环境探测

| 项目 | 现状 | 结论 |
| --- | --- | --- |
| 系统 | macOS 26.7.2，arm64 | 主目标平台 |
| 编译器 | Apple clang 21.0.0 | 可用 |
| Go | go1.25.5 darwin/arm64 | 可用 |
| make / git / lipo | 已安装 | 可用 |
| autoconf | **缺失** | 通过 Nix 提供（见 3.1） |
| automake | **缺失** | 通过 Nix 提供（见 3.1） |
| pkg-config | **缺失** | 通过 Nix 提供（见 3.1） |
| Nix | 2.35.3，位于 `/nix/var/nix/profiles/default/bin/nix` | 已安装；flakes 未启用、无 channel，但已验证可用 `-I` 直接指定 nixpkgs |
| /usr/bin/libtool | Apple libtool，非 GNU | n2n 构建不需要 GNU libtool |

## 1. 首期范围（MVP）

**实现**

- 单一子命令：`n2n edge`，前台运行，需要 root 权限。
- 参数：社区名、supernode、静态 IP/CIDR 或 `auto`（supernode 分配）、加密 key、加密算法、压缩算法、设备名、MAC、MTU、本地端口、管理端口、注册间隔/TTL、允许路由、组播丢弃、TOS、设备描述。
- 生命周期：启动、日志输出、`SIGINT`/`SIGTERM` 触发优雅退出并清理 utun。
- 构建：macOS arm64 原生二进制 + 复用的 `libn2n.a`。

**明确不做（后续迭代）**

- `supernode` 子命令（仅用 n2n 自带二进制做测试）；
- CLI 内置的管理 API 客户端（可直接用 `printf 'edges\n' | nc -u 127.0.0.1:5644` 查询）；
- DHCP 地址模式、IPv6、GUI/托盘；
- universal binary（arm64 与 x86_64 已分别产出，合并需双架构构建，见 3.2）。

**验收标准**

1. `scripts/build-n2n.sh` 在 arm64 上成功产出 `libn2n.a`；
2. 本机启动本地 supernode + 两个 edge 进程，两个 edge 均注册成功，且各自的 utun 接口都能收到经 n2n 隧道送达的报文（同机场景的内核 ICMP 限制见 6.2）；
3. `Ctrl+C` 后进程退出码为 0，utun 接口与路由被清理；
4. 参数错误（社区超长、非法 CIDR、未知算法）在 Go 层直接报错，不进入 C 层。

## 2. 初始目录结构（不含 Git）

```
n2n_go/
├── docs/
│   ├── N2NGo.md
│   └── macOS.md
├── go.mod                        # module n2n-go（名称暂定）
├── Makefile
├── shell.nix                     # Nix 构建环境（autoconf/automake/pkg-config）
├── cmd/n2n/
│   └── main.go                   # 入口：cobra 或标准库 flag
├── internal/
│   ├── cli/
│   │   ├── root.go
│   │   ├── edge.go               # 参数定义、校验、信号处理
│   │   └── version.go
│   ├── config/
│   │   ├── config.go             # EdgeConfig 结构、校验、默认值
│   │   └── config_test.go        # 参数校验单元测试
│   └── n2n/
│       ├── edge.go               # cgo 绑定（Go 侧，CFLAGS 与头文件）
│       ├── edge_glue.c           # C 适配层
│       ├── edge_glue.h
│       ├── link_arm64.go         # arm64 的 cgo LDFLAGS
│       └── link_amd64.go         # x86_64 的 cgo LDFLAGS
├── patches/
│   └── n2n-keep-running.patch    # 移除 run_edge_loop 入口的强制 keep_running=true
├── platform/macos/
│   └── tuntap_osx.c              # utun 实现（替代 n2n 原文件）
├── scripts/
│   ├── build-n2n.sh              # 复制源码 → 替换 tuntap → 编译 libn2n.a
│   └── smoke-test.sh             # 本地 supernode + 双节点冒烟
├── build/                        # 中间产物：build/n2n-arm64 等
├── third_party/
│   ├── n2n/                      # n2n 源码（Git submodule，固定 3.1.1）
│   ├── lib/darwin/arm64/libn2n.a
│   ├── lib/darwin/x86_64/libn2n.a
│   └── n2n-src/include/          # 构建脚本复制的头文件只读副本
└── bin/                          # 最终产物：bin/n2n-darwin-arm64、bin/n2n-darwin-amd64
```

说明：

- `build/`、`third_party/lib/`、`third_party/n2n-src/include`、`bin/` 为生成目录，可随时 `make clean` 删除。
- n2n 源码以 Git submodule 形式固定在 `third_party/n2n`（3.1.1）；首次克隆后需执行 `git submodule update --init third_party/n2n`。
- `scripts/build-n2n.sh` 默认使用该 submodule 作为源码（可用 `N2N_SRC` 覆盖），每次构建复制一份，避免污染原始源码。

## 3. 编译环境

### 3.1 依赖安装（Nix）

构建期工具统一由 Nix 提供，不使用 Homebrew：`autoconf`、`automake`、`pkg-config`。Apple clang 与 macOS SDK 仍来自 Xcode Command Line Tools，不通过 Nix 提供。

当前机器状态：Nix 2.35.3 已安装，但 **flakes 未启用、没有配置 channel**，且 `nix` 不在非交互 shell 的 PATH 中。已验证无需配置 channel，直接用 `-I` 指定 nixpkgs 即可：

**方式 A：临时 shell（已验证可用）**

```bash
NIXPKGS=https://mirrors.tuna.tsinghua.edu.cn/nix-channels/nixpkgs-unstable/nixexprs.tar.xz
/nix/var/nix/profiles/default/bin/nix-shell \
  -I nixpkgs=$NIXPKGS \
  -p autoconf automake pkg-config
```

实测版本（macOS 26.7.2 arm64）：

| 工具 | 版本 |
| --- | --- |
| autoconf / autoreconf | 2.73 |
| automake | 1.18.1 |
| pkg-config | 0.29.2 |
| make（Nix 提供） | GNU Make 4.4.1 |
| clang（Nix 提供） | 21.1.8 |

注意：

- `nix-shell -p` 会把 nixpkgs 的 `clang-wrapper` 和 `gnumake` 放到 PATH 最前面，此时 `clang` **不再指向 Apple clang**。因此构建脚本显式使用 `CC=/usr/bin/clang`，保证使用 Apple SDK；`make` 用 Nix 的 GNU Make 无冲突。
- `nix` 不在非交互 shell 的 PATH 中，统一用绝对路径 `/nix/var/nix/profiles/default/bin/nix-shell`，或把该目录加入 PATH。
- 不需要修改 `~/.nix-channels` 或全局 `nix.conf`；当前未配置 channel 不影响该方式。

**方式 B：项目内固定版本（已落地，推荐）**

仓库根目录已提供 `shell.nix`，用内容哈希固定 nixpkgs 快照，不依赖全局 channel，也不随 channel 更新漂移。等价内容：

```nix
{ pkgs ? import (builtins.fetchTarball {
    url = "https://mirrors.tuna.tsinghua.edu.cn/nix-channels/nixpkgs-unstable/nixexprs.tar.xz";
    sha256 = "1pmz0z8gpmrfvw7mg42qkp4iwi7bq0ddp1mw4q7hkvdrcf4qvgr4";
  }) {} }:

pkgs.mkShell {
  packages = [ pkgs.autoconf pkgs.automake pkgs.pkg-config pkgs.gnumake ];
  shellHook = ''
    export CC=/usr/bin/clang
  '';
}
```

进入环境并验证：

```bash
nix-shell shell.nix --run 'autoconf --version | head -1; autoreconf --version | head -1; automake --version | head -1; pkg-config --version; make --version | head -1; echo CC=$CC'
```

已实测通过：autoconf/autoreconf 2.73、automake 1.18.1、pkg-config 0.29.2、GNU Make 4.4.1，`CC=/usr/bin/clang`。

注意：`nix-shell` 中 `command -v clang` 仍会指向 nixpkgs 的 clang-wrapper；构建以 `CC` 环境变量为准，`build-n2n.sh` 也会显式传 `CC=/usr/bin/clang`。

说明：

- `autoconf` 是必需的：`n2n/autogen.sh` 会执行 `autoreconf -if`，删除并重新生成 `configure`、`config.mak`、`include/config.h`。
- `automake` 建议安装：`autoreconf` 在部分环境中会探测 automake 宏。
- `pkg-config` 用于 Makefile 中的 OpenSSL 探测；MVP 不启用 OpenSSL/zstd，缺失也不会阻断，但建议安装。
- 不通过 Nix 提供/不启用：`zstd`、`openssl`、`miniupnpc`、`natpmp`、`pcap`、`libcap`。这些在 macOS 上要么不必要，要么会引入额外链接依赖。
- Go 需支持 cgo，`CGO_ENABLED=1`；Go 本身继续使用系统安装的 go1.25.5。
- 所有 `make n2n-lib` / `make build` 命令都应在 `nix-shell` 内执行，确保 `autoreconf`、`pkg-config` 在 PATH 中。
- 若 `nix` 命令不可用，先确认 `/nix/var/nix/profiles/default/bin` 已加入 PATH（多用户安装的默认位置）。

### 3.2 构建 n2n 静态库

n2n 3.1.1 的 `Makefile` 中 `N2N_OBJS` 无条件包含 `src/tuntap_osx.o`，文件内部用 `#ifdef __APPLE__` 隔离。因此替换 utun 实现的正确方式是**替换源文件**，无需修改 Makefile：

```
复制 third_party/n2n（submodule）→ build/n2n-<arch>/
用 platform/macos/tuntap_osx.c 覆盖 build/n2n-<arch>/src/tuntap_osx.c
应用 patches/n2n-keep-running.patch（移除 run_edge_loop 入口的强制 keep_running=true）
清除 nix-shell 注入的 SDK 变量，固定 MACOSX_DEPLOYMENT_TARGET=11.0
执行 autogen.sh → configure → make libn2n.a
输出到 third_party/lib/darwin/<arch>/libn2n.a
把头文件复制到 third_party/n2n-src/include/
```

`scripts/build-n2n.sh` 关键参数（arm64 示例）：

```bash
ROOT=$(cd "$(dirname "$0")/.." && pwd)  # 仓库根目录，用于定位补丁与产物
N2N_SRC=${N2N_SRC:-$ROOT/third_party/n2n}  # submodule 中的 n2n 源码
ARCH=${1:-arm64}                       # arm64 | x86_64
WORK=$ROOT/build/n2n-$ARCH

rm -rf "$WORK"
mkdir -p "$WORK"
rsync -a --exclude build --exclude bin "$N2N_SRC/" "$WORK/"
cp platform/macos/tuntap_osx.c "$WORK/src/tuntap_osx.c"
( cd "$WORK" && patch -p1 --quiet -i "$ROOT/patches/n2n-keep-running.patch" )

# nix-shell 会通过 SDKROOT/NIX_CFLAGS_* 注入自带的 Apple SDK，覆盖 /usr/bin/clang 的默认 SDK
unset SDKROOT NIX_CFLAGS_COMPILE NIX_CFLAGS_LINK NIX_LDFLAGS
export MACOSX_DEPLOYMENT_TARGET=11.0

cd "$WORK"
./autogen.sh
./configure \
  CC=/usr/bin/clang \
  CFLAGS="-arch $ARCH -O2 -Wall -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -DSKIP_MULTICAST_PEERS_DISCOVERY" \
  LDFLAGS="-arch $ARCH"
make libn2n.a

mkdir -p "$ROOT/third_party/lib/darwin/$ARCH" "$ROOT/third_party/n2n-src/include"
cp libn2n.a "$ROOT/third_party/lib/darwin/$ARCH/libn2n.a"
rsync -a include/ "$ROOT/third_party/n2n-src/include/"
```

要点：

- `CC` 显式写成 `/usr/bin/clang`：在 `nix-shell` 中 PATH 上的 `clang` 是 nixpkgs 的 wrapper（21.1.8），不保证指向 Apple SDK；显式指定可避免工具链漂移。
- 静态库只做编译和 `ar` 归档，不涉及链接，因此 arm64 机器交叉编译 `x86_64` 不需要 Rosetta。
- `-DSKIP_MULTICAST_PEERS_DISCOVERY` 与 omniedge-cli 的做法一致，避免局域网组播发现带来的权限与噪音；首期也不依赖该发现机制。
- `patches/n2n-keep-running.patch`：3.1.1 的 `run_edge_loop` 入口会无条件写 `*keep_running = true`，会覆盖 Go 侧在循环启动前发出的停止请求；补丁移除该赋值，由胶水层自行设置初值并在进入循环前检查 `stop_requested`。
- `unset SDKROOT NIX_CFLAGS_COMPILE NIX_CFLAGS_LINK NIX_LDFLAGS`：避免 nix-shell 注入的 SDK 覆盖 Xcode Command Line Tools 的 SDK；`MACOSX_DEPLOYMENT_TARGET=11.0` 与 Go darwin/arm64 的默认部署目标一致，并通过 `-mmacosx-version-min` 传给 clang。
- 头文件：编译后把 `include/` 复制到 `third_party/n2n-src/include/`，使 cgo 只依赖仓库内副本，`build/` 可随时清理。
- arm64 与 x86_64 都用同一脚本产出（`make build` / `make build-amd64`）；在 Apple Silicon 上交叉构建 x86_64 时，nix 环境里的 clang 运行库是 arm64，链接阶段会出现 `ignoring file ... built for macOS-arm64` 的告警，属预期，产物为 `Mach-O 64-bit executable x86_64`，可在 Rosetta 下运行。universal binary 需双架构构建后再合并，单独评估。
- 若后续启用 zstd/openssl，需要追加 `--with-zstd` / `--with-openssl`，并在 cgo LDFLAGS 中补 `-lzstd` / `-lcrypto`；MVP 不引入。

### 3.3 Go 侧 cgo 配置

LDFLAGS 按架构拆分，`edge.go` 只保留 CFLAGS 与头文件引用：

`internal/n2n/edge.go`：

```go
/*
#cgo CFLAGS: -I${SRCDIR}/../../third_party/n2n-src/include -DSKIP_MULTICAST_PEERS_DISCOVERY
#include "edge_glue.h"
*/
import "C"
```

`internal/n2n/link_arm64.go`：

```go
//go:build darwin && arm64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/darwin/arm64/libn2n.a
import "C"
```

`internal/n2n/link_amd64.go`：

```go
//go:build darwin && amd64

package n2n

// #cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/darwin/x86_64/libn2n.a
import "C"
```

- 直接链接 `libn2n.a` 的仓库内路径，避免 `-ln2n` 的运行时搜索问题；构建约束保证只链接当前 GOARCH 对应的产物。
- 头文件来源：把 `third_party/n2n-src/include` 作为只读副本（由构建脚本从 `build/n2n-<arch>/include` 拷贝），或 cgo 直接指向 `build/n2n-arm64/include`。推荐前者，保证 `build/` 可清理。
- 链接阶段的系统库由 macOS SDK 隐式提供（pthread、resolv 均在 libSystem），无需额外 `-lpthread`。

### 3.4 Makefile 目标

| 目标 | 行为 |
| --- | --- |
| `make n2n-lib` | 调用 `scripts/build-n2n.sh arm64` |
| `make n2n-lib-amd64` | 调用 `scripts/build-n2n.sh x86_64` |
| `make build` | 依赖 `n2n-lib`，`CGO_ENABLED=1 go build -o bin/n2n-darwin-arm64 ./cmd/n2n` |
| `make build-amd64` | 依赖 `n2n-lib-amd64`，`GOARCH=amd64 CGO_ENABLED=1 go build -o bin/n2n-darwin-amd64 ./cmd/n2n` |
| `make build-all` | 依次构建 arm64 与 amd64 |
| `make check` | `gofmt` 检查 + `go vet` + `go test`（提交前使用） |
| `make smoke` | 构建并用本地 supernode 做双节点联调（需 sudo） |
| `make clean` | 删除 `build/`、`bin/`、`third_party/lib/` |
| `make shell` | （可选）`nix-shell shell.nix`，进入构建环境 |

上述命令均需在 `nix-shell` 内执行；`make shell` 只是便捷入口，不改变依赖来源。

### 3.5 常见构建问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `Please run ./configure` | 未生成 `config.mak` | 执行 `autogen.sh && ./configure` |
| `config.h: No such file` | `include/config.h` 未生成 | 同上；`config.h` 由 configure 产出 |
| `autoreconf: command not found` | 缺 autoconf，或未进入 `nix-shell` | 在 `nix-shell shell.nix` 内执行 |
| `pkg-config: command not found` | 缺 pkg-config，或未进入 `nix-shell` | 同上；MVP 中仅影响 OpenSSL 探测 |
| cgo 链接架构不匹配 | Go 架构与 `libn2n.a` 架构不一致 | 确认 `GOARCH` 与 `third_party/lib/darwin/<arch>` 对应 |

### 3.6 能否用 Docker 构建（结论：不能用于 macOS 产物）

- 在 macOS 上，Docker Desktop / OrbStack / Colima 运行的都是 **Linux 容器**（`linux/arm64` 或 `linux/amd64`），不存在 macOS 容器，无法调用 Apple 的 clang 与 macOS SDK，因此无法产出 Mach-O 二进制。
- macOS 产物必须使用 Apple SDK + Apple clang。`osxcross` 类方案技术上可在 Linux 中交叉编译 Darwin，但依赖 Apple SDK 的分发授权，且产物仍需在 macOS 上验证 utun 与签名，不适合作为主路径。
- utun 属于 macOS 内核接口，**只能在 macOS 上运行验收**；即使交叉编译成功，也无法在 Docker 中完成本方案的联调测试。
- Docker 的合理用途是中后期 Linux 目标：用容器固定 autotools 与交叉工具链，构建 Linux 版 `libn2n.a`（对应 `docs/N2NGo.md` 的 M3），首期 macOS 不引入 Docker。
- 可复现性替代方案：用 `shell.nix` 固定 nixpkgs 版本并提供 autotools，记录 Xcode CLT/SDK 版本；CI 使用 macOS arm64 runner 执行同一套脚本。

## 4. 实现设计

### 4.1 utun 适配层

以 omniedge-cli 的 [tuntap_osx_utun.c](../patches/tuntap_osx_utun.c)（本地参考路径 `/Users/xtao/Work/n2n/omniedge-cli/patches/tuntap_osx_utun.c`）为蓝本，但**不能直接复制**。相对该补丁必须做以下修正：

| 问题 | 参考补丁现状 | 本项目修正 |
| --- | --- | --- |
| 函数签名 | `tuntap_open` 7 参数（2.6 分支） | 3.1.1 为 8 参数，补 `int metric`，与 [n2n.h](../n2n/include/n2n.h#L128-L129) 对齐 |
| `device_mask` | 未赋值 | 必须 `device->device_mask = inet_addr(device_mask)`（网络字节序）；否则注册 supernode 的 `net_bitlen` 为 0（[edge_utils.c](../n2n/src/edge_utils.c#L1321) 用 `mask2bitlen(ntohl(...))`） |
| `mtu` / `if_idx` | 未赋值 | 补 `device->mtu`；`if_idx` 在 macOS 不参与组播实现，可置 0 |
| 配置命令 | `system("ifconfig ...")` 字符串拼接 | 改为 `/sbin/ifconfig` 参数数组 `fork + execv`（或 ioctl），避免 root 下命令注入；随后用 `/sbin/route add -net <network> -netmask <mask> -interface <dev>` 补子网路由（点到点 ifconfig 只装本地主机路由），`/32` 与 `/0` 不添加 |
| 读缓冲 | 固定 2048 字节 | 临时缓冲按 `(mtu>0 ? mtu : DEFAULT_MTU) + 4` 分配（4 字节 AF 头）；补伪以太头前检查 `ip_len + 14 <= len`，超出则告警丢弃；伪以太头直接写入 n2n 传入的大缓冲 |
| 设备名 | `strncpy` 不保证结尾 | 显式置 `dev_name[sizeof-1]='\0'` |
| 目的 MAC | 固定广播 | 优先按目的 IPv4 在 edge 的 `pending_peers`/`known_peers` 中查对端 MAC，命中则用单播 MAC；否则退回广播。注意 n2n 3.x 的 `drop_multicast` 只匹配 `01:00:5E`/`33:33` 目的 MAC，广播帧不受影响；单播主要用于减少泛洪，并非互通前提 |
| 传输层校验和 | 不处理 | macOS utun 上交的 TCP/UDP 校验和可能处于 offload 待完成状态，原生对端内核会静默丢弃（表现为 ICMP 通、TCP/UDP 全超时）。读路径对 IPv4 TCP/UDP 重算校验和；分片、以及 UDP 校验和为 0 的情况跳过 |
| 关闭 | `fd` 未复位 | `tuntap_close` 后置 `fd = -1`，避免重复关闭 |
| 协议判定 | 写时仅处理 `0x0800`，ARP 假成功 | 读入 `AF_INET`/`AF_INET6`，写出 `0x0800`/`0x86DD`；IPv6 需 `--ipv6` 显式开启（见 p5 T3），关闭时记录并丢弃 IPv6 帧 |

保留的既有设计：

- `socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL)` + `CTLIOCGINFO` + `com.apple.net.utun_control`；
- `sc_unit = 0` 由系统分配 `utunN`；若用户传入 `-d utunN`，则 `sc_unit = N + 1`；
- get `UTUN_OPT_IFNAME` 获取实际设备名；
- L3→L2 适配：读时补伪以太头（目的 MAC 优先取对端 MAC，未命中用广播；类型 IPv4），写时剥以太头并加 4 字节 AF 头。

已知语义限制（首期接受，需在帮助信息中提示）：

- 默认仅 IPv4 地址；不支持 DHCP。IPv6 报文可通过 `--ipv6` 承载（T3：shim 按 MAC 的 EUI-64 配置 link-local；T4：TCP/UDP/ICMPv6 校验和已补全，`ping6` 端到端实测通过）；
- IPv4 组播/广播：出站帧目的 MAC 未知时用广播 MAC，n2n 的 `drop_multicast` 只匹配 `01:00:5E`/`33:33`，因此广播 MAC 的组播帧不会被丢弃；已用 `scripts/multicast-test.sh` 三节点实测通过（`-E` 与默认丢弃均能收到）；
- 出站帧目的 MAC 优先按目的 IPv4 查对端 MAC（单播），查不到才用广播；单播主要用于减少泛洪；
- 与原生 TAP 对端互通时，对端仍需 `-E` 才能接收真实的 `01:00:5E` 组播 MAC；
- 读路径需补全 TCP/UDP 校验和（macOS utun 上交时可能处于 offload 待完成状态，原始对端会静默丢弃）；
- 写路径对发往本机 overlay 地址的 ARP 请求直接应答（`edge_send_packet2net` 回送隧道），原生 TAP 对端据此解析到我们的 MAC；已用 `scripts/interop-test.sh` 双向 ping 实测通过；
- IPv6 写路径同样应答邻居通告请求（ICMPv6 NS → NA，target 为本机任一 IPv6 地址），原生 TAP 对端可解析我们的 IPv6 地址；已用 `scripts/interop-test.sh` 的 IPv6 阶段实测通过（3/3）；

### 4.2 CGO 适配层

`edge_glue.h` 对外接口：

```c
typedef struct n2n_go_edge n2n_go_edge_t;

n2n_go_edge_t *n2n_go_edge_create(const n2n_go_config_t *cfg);
int  n2n_go_edge_run(n2n_go_edge_t *e);   /* 阻塞，直到 keep_running=false */
void n2n_go_edge_stop(n2n_go_edge_t *e);
void n2n_go_edge_destroy(n2n_go_edge_t *e);
```

`n2n_go_config_t` 字段与 Go `EdgeConfig` 一一对应（社区、supernode 列表、key、算法、压缩、IP/mask、设备名、MAC、MTU、端口、注册参数、TOS、描述等）。

`n2n_go_edge_run` 执行顺序（对齐 3.1.1 的 `edge.c`）：

```c
edge_init_conf_defaults(&e->conf);
/* 逐字段赋值：community_name 补零到 16 字节、encrypt_key = strdup(...)、
   transop_id、compression、local_port、mgmt_port、allow_routing、tos 等 */
edge_conf_add_supernode(&e->conf, sn);        /* 可多次调用 */
if (edge_verify_conf(&e->conf) != 0) { ... }

e->eee = edge_init(&e->conf, &rc);            /* 3.1.1 不接收 tuntap 参数 */
if (!e->eee) { ... }

if (tuntap_open(&e->dev, dev_name, "static", ip, mask, mac, mtu, 0) < 0) { ... }
memcpy(&e->eee->device, &e->dev, sizeof(e->dev));   /* 与 edge.c 相同 */

e->eee->keep_running = &e->keep_running;
if (e->stop_requested)                         /* Stop 可能早于循环入口 */
    e->keep_running = 0;
n2nGoOnStarted(e->started_handle);             /* 通知 Go 侧已进入 edge loop */
rc = run_edge_loop(e->eee);

edge_term_conf(&e->conf);                      /* 会 free encrypt_key */
tuntap_close(&e->dev);
edge_term(e->eee);
```

关键点：

- **停止机制**：`patches/n2n-keep-running.patch` 移除了 `run_edge_loop` 入口处强制写 `*eee->keep_running = true` 的语句（3.1.1 的该行会覆盖 Go 侧在循环启动前发出的停止请求）。胶水层先设置 `keep_running` 指针、检查 `stop_requested` 并据此置 0，再通过 `n2nGoOnStarted` 通知 Go 侧；CLI 收到信号后先等 `Started()` 再调用 `Stop()`，`Stop()` 同时置 `stop_requested = 1` 与 `keep_running = 0`，保证早到的停止请求不丢失。
- **所有权**：`encrypt_key` 必须 `strdup`，由 `edge_term_conf` 释放；supernode 节点由 n2n 管理，不要重复释放。
- **并发**：`keep_running` 用 `volatile bool`；Go 的 `Stop()` 只做一次写操作，保持幂等。
- **错误路径**：`cleanup()` 在 `eee` 存在时按 `edge_term_conf → tuntap_close → edge_term` 释放；`edge_init` 失败只释放 conf，`tuntap_open` 失败会关闭已打开的 fd，避免泄漏。

### 4.3 参数映射

| CLI 参数 | C 目标 | 说明 |
| --- | --- | --- |
| `-config, --config` | 无 | JSON 配置文件路径，作为参数默认值；命令行显式参数优先（见 4.5） |
| `-c, --community` | `conf.community_name` | 最长 19 字节，超长在 Go 层直接报错 |
| `-l, --supernode` | `edge_conf_add_supernode` | 可重复，最多 8 个，全部传给 C 层 |
| `-a, --ip` | `tuntap_open` 的 `device_ip` / `device_mask` | 支持 `IPv4/CIDR`（如 `10.0.3.13/24`、`10.0.3.13/32`）、裸 IPv4 与 `auto`；裸 IP（如 `10.0.3.13`）自动按 `/24` 处理；`auto` 由 supernode 分配（SN_ASSIGN，见 [macOS-p5.md](macOS-p5.md) §3.2） |
| `-k, --key` | `conf.encrypt_key` | `strdup` 后交给 conf |
| `-H, --header-encryption` | `conf.header_encryption` | 头部加密，必须配合 `-k`，否则 Go 层直接报错 |
| `-P, --federation-public-key` | `conf.federation_public_key` | user-password 认证的联邦公钥（ASCII，长度 ≥ 44），由 `ascii_to_bin` 解码 |
| `-J, --auth-password` | `conf.shared_secret` | user-password 认证密码，由 `generate_private_key` 派生 |
| `-A, --cipher` | `conf.transop_id` | `none/aes/cc20/speck/tf` → `N2N_TRANSFORM_ID_*`；未指定且提供 `-k` 时自动使用 `aes` |
| `-z, --compress` | `conf.compression` | `none/lzo`（zstd 首期不启用） |
| `-d, --device` | `tuntap_open` 的 `dev` | 默认自动分配；可传 `utunN` |
| `-m, --mac` | `tuntap_open` 的 `device_mac` | 缺省随机生成本地管理地址 |
| `-M, --mtu` | `tuntap_open` 的 `mtu` + `dev.mtu` | 默认 1290，允许 68–1500（需 ≤ 1500） |
| `-p, --local-port` | `conf.local_port` | 0 表示自动 |
| `-t, --mgmt-port` | `conf.mgmt_port` | 同机多进程测试时必须互不相同 |
| `-i / -L` | `conf.register_interval/register_ttl` | 默认沿用 n2n |
| `-r, --routing` | `conf.allow_routing` | 布尔 |
| `-E` | `conf.drop_multicast=false` | 接受组播（与原生 n2n 语义一致），默认仍丢弃 |
| `--drop-multicast` | `conf.drop_multicast=true` | 显式丢弃 IPv4 组播与 IPv6 ND（默认行为） |
| `-T, --tos` | `conf.tos` | 0–255 |
| `-I, --description` | `conf.dev_desc` | 最长 `N2N_DESC_SIZE` |
| `--ipv6` | shim 的 IPv6 开关 | 承载 IPv6（读写 `0x86DD`、33:33 组播映射、NA/NS 邻居学习）；自动强制 `drop_multicast=false`，并按设备 MAC 的 EUI-64 配置 `fe80::/64`（macOS 不为 utun 自动生成） |
| `-f, --foreground` | 无 | 为兼容原生 n2n 命令行而保留：参数被接受但忽略，本 CLI 始终前台运行 |

### 4.4 CLI 行为

- 启动前检查 `os.Geteuid() == 0`，非 root 直接提示退出（首期不引入 helper）。
- `-f`/`--foreground`：为兼容原生 n2n 的命令行而保留，接受但不产生效果；`n2n edge` 始终前台运行，Go 进程内不做 daemon 化。
- 日志统一走 stderr；n2n 的 `traceEvent` 默认输出到 stdout，可在 C 适配层调整或接受现状，首期以“可读、可定位”为准。
- 信号处理：`signal.Notify(SIGINT, SIGTERM)` → `Stop()` → 等待 run goroutine 退出 → 正常返回。
- `n2n version` 输出 Go 版本 + n2n `PACKAGE_VERSION`（可通过 cgo 调 `print_n2n_version()` 或读 `config.h`）。

### 4.5 配置文件

`-config/--config <path>` 读取 JSON，作为所有参数的默认值；显式命令行参数会覆盖文件里的同名字段。未知字段直接报错，避免拼写错误被静默忽略。

```json
{
  "community": "spirit",
  "supernodes": ["wg.yixuedou.com:5356"],
  "address": "10.0.3.13",
  "key": "attentions",
  "cipher": "aes",
  "device": "utun",
  "mtu": 1290,
  "mgmt_port": 5644,
  "register_interval": 20,
  "register_ttl": 2,
  "drop_multicast": true
}
```

支持的字段：`community`、`supernodes`、`address`、`key`、`cipher`、`compression`、`header_encryption`、`federation_public_key`、`auth_password`、`device`、`mac`、`mtu`、`local_port`、`mgmt_port`、`register_interval`、`register_ttl`、`allow_routing`、`drop_multicast`、`disable_pmtu_discovery`、`tos`、`description`。未写出的字段沿用内置默认值，随后统一走 `Validate`。

```bash
sudo ./bin/n2n-darwin-arm64 edge -config /etc/n2n-go/edge.json
# 命令行覆盖示例：临时换管理端口
sudo ./bin/n2n-darwin-arm64 edge -config /etc/n2n-go/edge.json -t 5645
```

### 4.6 服务化（launchd）

`scripts/install-service.sh` 把 edge 装成系统级 LaunchDaemon，开机自启、崩溃自动重启，无需每次手动 `sudo` 前台运行：

```bash
# 安装（需 root）
sudo ./scripts/install-service.sh --config /path/to/edge.json

# 查看状态 / 日志
sudo launchctl print system/com.n2n-go.edge | head -n 20
sudo tail -f /var/log/n2n-go-edge.log

# 重启 / 停止
sudo launchctl kickstart -k system/com.n2n-go.edge
sudo launchctl bootout system/com.n2n-go.edge

# 卸载（保留配置与日志）
sudo ./scripts/install-service.sh --uninstall
```

安装位置：

| 项目 | 路径 |
| --- | --- |
| 二进制 | `/usr/local/bin/n2n-go-edge` |
| 配置 | `/Library/Application Support/n2n-go/edge.json` |
| plist | `/Library/LaunchDaemons/com.n2n-go.edge.plist` |
| 日志 | `/var/log/n2n-go-edge.log` |

说明：LaunchDaemon 以 root 运行，因此不需要额外的特权 helper；`KeepAlive=true` 会在进程退出后自动拉起，停服请用 `launchctl bootout` 或 `--uninstall`。

## 5. 任务拆解与验收

| 序号 | 任务 | 验收 |
| --- | --- | --- |
| T1 | 建目录、`go.mod`、Makefile、`shell.nix`、脚本骨架 | `nix-shell` 可进入；`make clean` 可执行；`go vet ./...` 通过 |
| T2 | `scripts/build-n2n.sh` 完成 arm64 `libn2n.a` 构建 | 产出文件存在；`lipo -info`/`file` 显示 arm64 |
| T3 | `platform/macos/tuntap_osx.c` 按 4.1 修正并通过编译 | `make n2n-lib` 无错误；无 `system()` 调用 |
| T4 | CGO 适配层 + `EdgeConfig` + 单元测试 | 配置校验测试通过；cgo 测试可创建/销毁句柄 |
| T5 | `n2n edge` 启动、信号停止、日志 | `sudo ./bin/n2n-darwin-arm64 edge ...` 创建 utun，Ctrl+C 后清理 |
| T6 | 本地 supernode 双节点联调 + 使用说明 | 两个 edge 均注册成功，且能通过 n2n 隧道双向收发包；`make smoke` 可复现 |

## 6. 测试方案

### 6.1 构建与静态检查

```bash
make n2n-lib
file third_party/lib/darwin/arm64/libn2n.a
make build
file bin/n2n-darwin-arm64
./bin/n2n-darwin-arm64 version

# Intel Mac（或交叉构建）
make n2n-lib-amd64 build-amd64
file bin/n2n-darwin-amd64
```

### 6.2 双节点联调（同一台 Mac）

准备本地 supernode（用 n2n 源码构建，仅测试用）：

```bash
make -C build/n2n-arm64 supernode
# -f 是原生 supernode 的“前台运行”开关；不加会 daemon 化
./build/n2n-arm64/supernode -f -p 7777
```

注意：`-a` 可写完整 CIDR，也可写裸 IPv4（裸 IP 自动按 `/24` 处理），或写 `auto` 由 supernode 分配地址（本地 supernode 默认从 `10.128.0.0/24` 起分配）；`-f` 仅为兼容保留，本 CLI 始终前台运行。

启动两个 edge（各自使用 `/32` 地址，避免同机路由冲突；管理端口错开）：

```bash
sudo ./bin/n2n-darwin-arm64 edge -c demo -k demo -a 10.0.0.1/32 \
  -l 127.0.0.1:7777 -d utun -t 15644 &
# 5645 是 supernode 的管理端口，这里必须错开
sudo ./bin/n2n-darwin-arm64 edge -c demo -k demo -a 10.0.0.2/32 \
  -l 127.0.0.1:7777 -d utun -t 15650 &
```

注意：`utun4` 只是示例。设备名由系统分配，`-d utun` 时实际名称以启动日志与 `ifconfig | grep utun` 为准，`netstat -I` 也要换成对应设备名。

验证：

```bash
ifconfig | grep -A3 utun
netstat -rn | grep 10.0.0   # 应有 10.0.0.0/... 经对应 utun 的路由
# 两个 edge 首次注册约需 10s，之后日志出现 [OK] edge <<< ... >>> supernode
ping -c 3 10.0.0.2        # 同机场景 100% 丢包，属预期，见下方说明
netstat -ib -I utun4      # 两侧 utun（名称以上一步为准）的 Ipkts 随 ping 增长，说明隧道通路正常
```

注意（同机测试限制）：两个 edge 跑在同一台机器上时，对端地址同时也是本机地址。macOS 不会为「送往本机地址、却从非所属接口送入」的报文生成 ICMP 回包（`net.inet.ip.check_interface` 打开时更是直接丢弃），因此 `ping` 一定 100% 丢包，这不代表隧道不通。判断隧道是否可用应看两侧 utun 的收包计数是否增长，6.3 的冒烟脚本即按此断言。

停止后验证：

```bash
kill -INT <pid>
ifconfig | grep utun      # 对应 utun 消失
netstat -rn | grep 10.0.0 # 路由随接口清理
```

注意：第一次在终端访问局域网时，macOS 15/26 可能弹出「本地网络」授权提示；本机回环测试不受影响，连接真实局域网节点前需允许终端应用访问本地网络。

### 6.3 冒烟脚本

`scripts/smoke-test.sh` 串起：构建库 → 构建 CLI → 启动 supernode → 启动两个 edge → 等两侧注册 → 发流量并核对两侧 utun 收包计数 → 发 `SIGINT` 并检查退出码与 utun 清理。脚本在失败时统一 `trap` 杀掉子进程，避免残留 utun。supernode 与管理端口每轮随机选取，因此可以反复执行；启动前只清理遗留的**测试 supernode**，不会误杀其它正在运行的 edge（可用 `SN_PORT`、`EDGE1_MGMT`、`EDGE2_MGMT` 覆盖）。

### 6.4 常见参数错误

| 报错 | 原因 | 修正 |
| --- | --- | --- |
| `invalid address "10.0.3.13" ... invalid CIDR address: 10.0.3.13` | 旧版本 CLI 只接受 CIDR，裸 IP 被 `net.ParseCIDR` 拒绝；当前版本裸 IPv4 会自动补 `/24` | 直接写 `-a 10.0.3.13`；需要非 `/24` 网段时写 `-a 10.0.3.13/16`（同机测试用 `/32`） |
| `flag provided but not defined: -f` | 旧版本 CLI 未定义 `-f`；当前版本已接受该参数并忽略（本 CLI 始终前台运行） | 可直接带 `-f`，去掉也不影响 |

正确的最小启动命令示例：

```bash
sudo ./bin/n2n-darwin-arm64 edge \
  -c demo -k demo \
  -a 10.0.3.13 \
  -l 127.0.0.1:7777 \
  -d utun -f
```

## 7. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 需要 root 才能创建 utun | 启动前检测并明确提示；后续迭代再引入特权 helper |
| utun 仅 L3 | 在文档和 `--help` 中声明限制；IPv6 由 `--ipv6` 单独开启，接口语义（ND、MTU、路由）仍按 L3 处理 |
| Apple 收紧 utun 使用政策 | 首期接受；持续跟进，必要时迁移 NetworkExtension |
| autotools 依赖缺失 | 由 `shell.nix` 固定提供 autoconf/automake/pkg-config；CI 同样用 Nix 或预装 |
| 本地网络隐私授权 | 首次运行引导用户到「隐私与安全性 → 本地网络」开启；测试用回环 |
| 同机双节点路由冲突 | 测试统一使用 `/32` + 独立管理端口 |
| x86_64/universal 后续需求 | `make build-amd64` 已可直接产出 x86_64；universal 仍需双架构合并，单独评估 |

## 8. 首期交付物

- 可运行的 CLI：`bin/n2n-darwin-arm64`、`bin/n2n-darwin-amd64`
- 可复现的静态库：`third_party/lib/darwin/{arm64,x86_64}/libn2n.a`
- 构建脚本：`scripts/build-n2n.sh`
- 服务化脚本：`scripts/install-service.sh`（launchd，见 4.6）
- 冒烟脚本：`scripts/smoke-test.sh`
- 组播验证脚本：`scripts/multicast-test.sh`
- 自动地址验证脚本：`scripts/auto-address-test.sh`
- IPv6 验证脚本：`scripts/ipv6-test.sh`
- 原生对端互通脚本：`scripts/interop-test.sh`（容器内跑原生 3.1.1 edge，需 docker）
- 文档：本文件 + `docs/N2NGo.md`
- 后续设计：[macOS-p5.md](macOS-p5.md)（IPv6；T0 `-E` 语义、T1 组播、T2 `-a auto`、T3 IPv6 读写路径、T4 IPv6 校验和、T5 原生对端互通均已实测通过）

## 9. 源码与版本管理

- n2n 源码以 Git submodule 固定在 `third_party/n2n`（3.1.1）；首次克隆后执行 `git submodule update --init third_party/n2n`；
- `scripts/build-n2n.sh` 默认从 submodule 复制源码到 `build/n2n-<arch>/`，构建产物与源码分离；
- 可用 `N2N_SRC` 覆盖为其他源码路径。
