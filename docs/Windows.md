# Windows 首期实施方案

本文档对应 [N2NGo.md](N2NGo.md) 的 Windows 首期落地计划，与 [macOS.md](macOS.md)、[Linux.md](Linux.md) 并列。沿用同样的约束：本阶段不做任何 Git 操作，n2n 源码直接使用本地路径 `/Users/xtao/Work/n2n/n2n`，构建时复制到工作目录。

Windows 与 macOS/Linux 的差异不在协议层，而在设备层、构建环境和进程模型：

1. **设备层用 wintun，而不是 TAP 驱动**。wintun 是用户态的三层（L3）虚拟网卡，不需要安装内核驱动；但 n2n 的 edge 是二层（L2）语义，因此必须在设备层加一个 L3↔L2 shim：封装/解封装以太网头、应答 ARP、维护虚拟 MAC 与对端 MAC 学习表。
2. **在 Windows 上原生构建**。工具链为 MSYS2 的 mingw-w64，直接产出 `libn2n.a` 与 `n2n-windows-amd64.exe`；不再以 macOS 交叉编译为主路径。原因是 wintun 的运行时依赖、IP Helper 配置和真实设备行为都只能在 Windows 上验证。
3. **没有 fd/select 语义**。wintun 通过 ring buffer + 读事件访问，n2n 在 `run_edge_loop` 内额外起一个读线程；主循环的 `select()` 不包含设备。
4. **停止靠事件，不靠取消 I/O**。读线程用 `WaitForMultipleObjects({wintun 读事件, 停止事件})`，停止时 `SetEvent` 唤醒；不再需要 `CancelIoEx`。
5. **需要管理员权限**，但不需要预装驱动、不需要重启；退出时由 `WintunCloseAdapter` 删除适配器。

**决策摘要**：首期只做 `windows/amd64`；在 Windows 10/11 x64 + MSYS2 MinGW x64 上原生构建；设备层默认 wintun（随包分发官方签名的 `wintun.dll`）；实现 L3↔L2 shim（ARP 应答 + 虚拟 MAC + 按需广播/MAC 学习）；wintun 模式下强制 `drop_multicast = 0`；运行验收在真实 Windows 10/11 x64 上完成，与 Linux/macOS edge 双向 `ping` 通。

## 0. 开发与构建环境

| 项目 | 现状 | 结论 |
| --- | --- | --- |
| 当前开发机 | macOS arm64 | 只能编辑/审查文档与代码，**不能构建或验收本方案** |
| 构建机 | **需要 Windows 10/11 x64**（VM 或实体机） | 首期唯一构建路径 |
| MSYS2 | **未准备**（当前无 Windows 构建机） | 安装 MSYS2，使用 “MSYS2 MinGW x64” shell |
| Go | 需 Windows 版 go1.25.x | cgo 需要 mingw-w64 的 `gcc` 在 PATH |
| mingw-w64 | 随 MSYS2 安装 | `mingw-w64-x86_64-gcc`、`binutils`、`make`、`autoconf`、`automake`、`pkg-config` |
| wintun | 需下载官方发行包 | `wintun-0.14.1.zip` 的 `bin/amd64/wintun.dll`，放 `bin/` 与 exe 同目录 |
| Windows 测试机 | **未准备** | 与构建机可为同一台；需管理员权限 |
| 对端 | 现有 macOS/Linux edge | 复用现有产物做跨平台互通验收 |
| Docker / WSL2 / Wine | — | 都不能提供 wintun 的真实 Windows 内核行为，不能替代本方案验收 |

结论：本方案**必须在 Windows 上构建与验收**；macOS 只用于文档与代码编辑。首期不承诺 arm64 Windows。

## 1. 首期范围（MVP）

**实现**

- 单一子命令 `n2n edge`，前台运行，需要管理员权限。
- 目标平台：`windows/amd64`，单个 `.exe` + 同目录 `wintun.dll`。
- 设备层使用 wintun（L3），在 `tuntap_*` 内实现 L3↔L2 shim，对上仍向 n2n 提供以太网帧。
- 静态 IPv4 地址（`-a IPv4/CIDR`），通过 IP Helper API 配置。
- 复用现有 Go CLI、配置校验与生命周期设计；仅补 Windows 平台分支。
- 原生构建脚本、补丁、Windows 构建与运行说明。
- 运行验收：与另一个 edge（Linux/macOS 任一）通过 supernode 双向 `ping` 通。

**明确不做（后续迭代）**

- tap-windows6 作为可切换的设备层（仅保留 n2n 上游实现作参考，不进首期产物）；
- IPv6 / NDP、DHCP 地址模式；
- Windows 服务（SCM）化、托盘/GUI、安装器与代码签名；
- `supernode` 子命令（测试用原生 `supernode.exe`）；
- `arm64`/`386` 产物；
- 头部加密（`-H`）、联邦认证（`-J/-P`）；
- 非 IP 以太网协议（LLDP、STP 等）的透传。

**验收标准**

1. 在 Windows 10/11 x64 + MSYS2 MinGW x64 下，`scripts/build-n2n-win.sh` 产出 `third_party/lib/windows/amd64/libn2n.a`；`scripts/build-windows.sh` 产出 `bin/n2n-windows-amd64.exe`，`file` 显示 PE32+ x86-64；
2. `bin/` 下同时存在 `wintun.dll`，`n2n-windows-amd64.exe version` 可运行并打印 wintun 驱动版本；
3. `n2n edge` 能创建名为 `n2n0` 的 wintun 适配器、配置 IP/MTU、完成 supernode 注册；
4. 与另一台机器上的 Linux/macOS edge 双向 `ping` 通；对端 `arp -a` 能看到 Windows 侧的虚拟 MAC；
5. `Ctrl+C` 后进程退出码为 0，`Get-NetAdapter -Name n2n0` 不再显示该适配器（wintun 适配器随句柄关闭删除）；
6. 参数错误（社区超长、非法 CIDR、未知算法、非法设备名/MAC）在 Go 层直接报错，不进入 C 层。

## 2. 与 macOS / Linux 的差异

| 维度 | macOS | Linux | Windows |
| --- | --- | --- | --- |
| 设备层 | 系统原生 utun（L3），需自写 L3→L2 适配 | 内核 TAP（L2），复用原生实现 | wintun（L3），需自写 L3↔L2 shim |
| 驱动依赖 | 无 | `CONFIG_TUN` + `/dev/net/tun` | 无内核驱动；用户态 `wintun.dll` |
| 帧格式 | 伪以太网头 + IPv4 载荷 | 原生以太网帧 | shim 产出/消费原生以太网帧；wintun 侧是 IP 包 |
| ARP / 广播 | ARP 无意义，直接丢弃 | 正常可用 | Windows 自身不 ARP；shim 实现 ARP 应答，出方向按需广播/单播 |
| IPv6 | 首期不支持 | 首期不启用 | 首期在 shim 丢弃 0x86DD；不配置 IPv6 地址 |
| MAC | 适配层自行生成 | 由 Go 生成，TAP 接受 | shim 维护虚拟 MAC（`-m` 显式指定，否则确定性派生） |
| 权限 | root | root 或 `CAP_NET_ADMIN` | Administrators + UAC |
| 设备名 | `utun` / `utunN`，由系统分配 | 空名自动分配 `tapN` | 空名默认 `n2n0`；适配器创建后随进程删除 |
| I/O 模型 | fd + select | fd + select | wintun ring buffer + 读事件；独立读线程 |
| 停止 | 关闭 fd | 关闭 fd，内核删接口 | `keep_running=0` + `SetEvent(stop)`，读线程返回后删除适配器 |
| 构建 | Nix + Apple clang | 容器内 gcc/clang | Windows 原生 MSYS2 + mingw-w64 |
| 同机双节点 | `/32` 规避路由冲突 | 独立 netns/容器 | 需两个适配器名，但路由仍可能绕过隧道；推荐两台机器/两个 VM |

macOS 的 [tuntap_osx.c](../platform/macos/tuntap_osx.c) 里的伪以太头、AF 头、校验和逻辑可以作为 shim 的参考，但不能直接复用：wintun 的 IP 包已经由 Windows 协议栈生成，不需要再改 IP 头。

## 3. 目录结构（新增部分）

```
n2n_go/
├── platform/
│   └── windows/
│       ├── tuntap_wintun.c           # 构建时覆盖 n2n 的 src/win32/wintap.c
│       ├── tuntap_wintun.h           # tuntap_interrupt / 适配器诊断声明
│       ├── n2n.manifest              # 可选：requireAdministrator 清单
│       └── n2n.rc                    # 生成 .syso 的资源脚本
├── patches/
│   ├── n2n-keep-running.patch        # 复用 macOS/Linux 的补丁
│   └── n2n-win32-wintun.patch        # edge_read_from_tap 的停止检查
├── internal/
│   ├── config/
│   │   ├── config.go                 # 现有：设备名校验内联在 Validate（L184-L191）
│   │   ├── device_darwin.go          # 计划（见 Linux.md）：utun 校验
│   │   ├── device_linux.go           # 计划（见 Linux.md）：tapN 校验
│   │   └── device_windows.go         # 本方案新增：wintun 适配器名校验
│   ├── cli/
│   │   ├── edge.go                   # 现有：euid 检查内联（L134）
│   │   └── permission_windows.go     # 本方案新增：管理员权限检测
│   └── n2n/
│       ├── edge_glue.c               # 增加 _WIN32 分支
│       └── link_windows_amd64.go     # 新增：cgo LDFLAGS → windows/amd64/libn2n.a
├── scripts/
│   ├── build-n2n-win.sh              # 复制源码 → 替换 wintap.c → 打补丁 → 编 libn2n.a
│   ├── build-windows.sh              # 宿主入口：库 + Go 二进制 + 拷贝 wintun.dll
│   └── smoke-test-windows.ps1        # Windows 上的联调脚本
├── build/
│   └── n2n-windows-amd64/            # 中间源码与目标文件
├── third_party/
│   ├── lib/windows/amd64/libn2n.a
│   ├── n2n-src/include/              # 与 macOS/Linux 共用
│   ├── n2n-src/src/win32/            # n2n_typedefs.h 相对引用的 MinGW 头
│   └── wintun/amd64/wintun.dll       # 官方签名 DLL（构建时拷到 bin/）
└── bin/
    ├── n2n-windows-amd64.exe
    └── wintun.dll
```

说明：

- `build/`、`third_party/lib/`、`bin/` 仍为生成目录，`make clean` 可删除。
- `platform/windows/tuntap_wintun.c` 直接覆盖 `src/win32/wintap.c`：该文件的对外符号（`tuntap_open`/`tuntap_read`/`tuntap_write`/`tuntap_close`/`tuntap_get_address`）与 n2n 期望完全一致，替换后不需要改 Makefile 的对象列表。上游 TAP 实现仍保留在 n2n 源码目录中，作为备选参考。
- 头文件同步要跳过 `config.h`：它是 configure 生成的，三个平台的内容只在 `PACKAGE_BUILDDATE` 等字段上有差异；若用 Windows 生成的副本覆盖共享目录，会让 macOS/Linux 的下一次构建误以为头文件变更而全量重编译。共享目录缺少 `config.h` 时再复制一份即可（见脚本末尾）。
- 不再需要 `shell-win.nix`；MSYS2 的包管理器承担工具链固定。

## 4. 编译环境与产物（Windows 原生）

### 4.1 工具链

安装 MSYS2 后，在 “MSYS2 MinGW x64” shell 中安装：

```bash
pacman -S --needed \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-binutils \
  make autoconf automake pkg-config rsync patch \
  git
```

Go 使用官方 Windows 安装包即可；确保 `go version` 与 `gcc --version` 都在同一个 shell 可见：

```bash
gcc --version | head -1          # 应为 x86_64-w64-mingw32-gcc
windres --version | head -1
go version
```

要点：

- n2n 的 Windows 版只支持 mingw 构建（[Makefile](file:///Users/xtao/Work/n2n/n2n/Makefile#L194-L200) 通过 `CONFIG_HOST_OS` 是否包含 `mingw` 决定追加 `src/win32/*.o`）；MSVC 不作为本方案路径。
- 即使是在 Windows 上原生构建，也要显式传 `--host=x86_64-w64-mingw32`。MSYS2 的 `config.guess` 可能把宿主识别为 `x86_64-pc-msys`，导致 configure 生成 `CONFIG_HOST_OS=msys`，Makefile 就不会加入 `wintap.o`、`edge_utils_win32.o`、`getopt.o`、`getopt1.o` 与 `edge_rc.o`。
- `windres` 不能省：mingw 下 `N2N_OBJS` 无条件包含 `src/win32/edge_rc.o`；`binutils` 已包含它。
- 不需要 Windows SDK：mingw-w64 自带 `winsock2.h`、`iphlpapi.h`、`netioapi.h`、`ws2tcpip.h` 等头与导入库。

### 4.2 构建 n2n 静态库

`scripts/build-n2n-win.sh` 关键步骤（在 MSYS2 MinGW x64 shell 中执行）：

```bash
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
N2N_SRC="${N2N_SRC:-/c/Work/n2n/n2n}"
ARCH=amd64
WORK="$ROOT/build/n2n-windows-$ARCH"
LIB_DIR="$ROOT/third_party/lib/windows/$ARCH"
INCLUDE_DIR="$ROOT/third_party/n2n-src/include"

rm -rf "$WORK"
mkdir -p "$WORK"
rsync -a --exclude build --exclude bin --exclude .git "$N2N_SRC/" "$WORK/"

# 用 wintun 设备层替换 tap-windows6 实现（头文件同目录，供 wintap.c include）
cp "$ROOT/platform/windows/tuntap_wintun.c" "$WORK/src/win32/wintap.c"
cp "$ROOT/platform/windows/tuntap_wintun.h" "$WORK/src/win32/tuntap_wintun.h"

( cd "$WORK" \
  && patch -p1 --quiet -i "$ROOT/patches/n2n-keep-running.patch" \
  && patch -p1 --quiet -i "$ROOT/patches/n2n-win32-wintun.patch" )

cd "$WORK"
./autogen.sh
./configure \
  --host=x86_64-w64-mingw32 \
  CC=gcc AR=ar WINDRES=windres \
  CFLAGS="-O2 -Wall -D_WIN32_WINNT=0x0601 -DSKIP_MULTICAST_PEERS_DISCOVERY"

make libn2n.a

mkdir -p "$LIB_DIR" "$INCLUDE_DIR"
cp libn2n.a "$LIB_DIR/libn2n.a"
# config.h 由 configure 生成，三平台内容等价但时间戳不同；不要用 Windows 的副本
# 覆盖共享目录，否则会无谓触发 macOS/Linux 全量重编译。
rsync -a --exclude config.h include/ "$INCLUDE_DIR/"
[ -f "$INCLUDE_DIR/config.h" ] || cp include/config.h "$INCLUDE_DIR/config.h"
# n2n_typedefs.h 在 MinGW 下会 include "../src/win32/n2n_win32.h"，cgo 编译
# edge_glue.c 时需要该头文件位于共享头目录旁边。
rsync -a --include '*/' --include '*.h' --exclude '*' \
  src/win32/ "$(dirname "$INCLUDE_DIR")/src/win32/"
```

要点：

- `-D_WIN32_WINNT=0x0601`（Windows 7）是 IP Helper API（`SetIpInterfaceEntry`、`CreateUnicastIpAddressEntry`）与 wintun 的最低声明门槛；n2n 的 [defs.h](file:///Users/xtao/Work/n2n/n2n/src/win32/defs.h#L15-L22) 只在非 `_WIN64` 时把它压到 `0x501`，amd64 构建仍需显式抬到 `0x0601`。
- 保留 `-DSKIP_MULTICAST_PEERS_DISCOVERY`：n2n 的组播发现依赖默认路由网卡，在 wintun 场景下不可用（[edge_utils_win32.h](file:///Users/xtao/Work/n2n/n2n/src/win32/edge_utils_win32.h#L23-L29) 也注明该机制不稳定），首期不用。
- 不启用 `zstd`/`openssl`/`libcap`/`pcap`/`miniupnpc`/`natpmp`，避免额外 DLL/导入库依赖。
- 备选：如果不想装 autotools，可以用 `scripts/hack_fakeautoconf.sh` 生成 `config.mak`，但它生成的版本号不准确；首期以真实 autotools 为准。
- 产物检查：`file libn2n.a` 应为 `current ar archive`，`ar t libn2n.a` 能看到 `wintap.o`、`edge_utils_win32.o`。

### 4.3 Go 侧 cgo 配置

新增按 OS/架构约束的链接文件，避免 `darwin`/`linux` 的 LDFLAGS 混入：

`internal/n2n/link_windows_amd64.go`：

```go
//go:build windows && amd64

package n2n

/*
#cgo CFLAGS: -D_WIN32_WINNT=0x0601 -I${SRCDIR}/../../platform/windows
#cgo LDFLAGS: ${SRCDIR}/../../third_party/lib/windows/amd64/libn2n.a -lnetapi32 -lws2_32 -liphlpapi
*/
import "C"
```

`internal/n2n/edge.go` 的 CFLAGS 保持 `-I${SRCDIR}/../../third_party/n2n-src/include -I${SRCDIR} -DSKIP_MULTICAST_PEERS_DISCOVERY` 不变即可；额外的 `-I.../platform/windows` 用于 `tuntap_wintun.h`（`tuntap_interrupt` 声明），系统头文件由 mingw 提供。

构建命令（在 MSYS2 MinGW x64 shell 中）：

```bash
export CGO_ENABLED=1 GOOS=windows GOARCH=amd64
go build -trimpath -buildvcs=false \
  -ldflags '-linkmode external -extldflags "-static"' \
  -o bin/n2n-windows-amd64.exe ./cmd/n2n
```

要点：

- `-static` 让 mingw 静态链接 `libgcc` 与 `libwinpthread`，否则目标机可能缺 `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`。
- 链接顺序必须是 `libn2n.a` 在前、系统库在后；`ws2_32`（Winsock）与 `iphlpapi`（IP Helper）是必需的。`wintun.dll` 走运行时 `LoadLibrary`，**不链接** `wintun.lib`。
- `build-windows.sh` 在构建成功后把 `third_party/wintun/amd64/wintun.dll` 复制到 `bin/`，并在缺失时直接报错。
- 发布前用 `objdump -p bin/n2n-windows-amd64.exe | grep 'DLL Name'` 检查导入表，除系统 DLL 外不应出现 `libgcc_*` / `libwinpthread-*`。

### 4.4 胶水层的 Windows 编译分支

[edge_glue.c](../internal/n2n/edge_glue.c) 目前假设 POSIX，需要按平台隔离：

| 位置 | 现状 | Windows 处理 |
| --- | --- | --- |
| `#include <unistd.h>` | POSIX 头 | `#ifdef _WIN32` 改 `<io.h>`，并 `#define dup _dup`、`#define fdopen _fdopen`、`#define close _close`、`#define STDERR_FILENO 2` |
| `e->dev.fd = -1` | `tuntap_dev` 在 Windows 没有 `fd` 字段（[n2n_win32.h](file:///Users/xtao/Work/n2n/n2n/src/win32/n2n_win32.h#L63-L75)） | `#ifndef _WIN32` 包起来；Windows 用 `device_handle = INVALID_HANDLE_VALUE` |
| `e->dev.dev_name` | Windows 没有 `dev_name`，只有 `char *device_name` / `char *ifName` | 平台分支：优先用 `e->dev.device_name` 填充 `tuntap_priv_conf.tuntap_dev_name`；空则用 `ifName` |
| 设备默认值 | `"utun"` | Windows 用 `""`（由 `tuntap_open` 映射为 `n2n0`），错误信息改为 “no wintun adapter available” |
| `init_trace()` | `dup`/`fdopen` | 语义不变，替换为 `_dup`/`_fdopen` |
| WSA 初始化 | 无 | `n2n_go_edge_create` 首次调用 `initWin32()`（内部 `WSAStartup`）；进程内不调用 `destroyWin32()`，避免破坏 Go `net` 包持有的 Winsock 引用计数 |
| 停止 | 只置 `keep_running=0` | 追加 `tuntap_interrupt(dev)` 唤醒读线程（见 5.6） |
| 组播策略 | 默认 `drop_multicast = 1` | wintun 模式下强制置 0（见 5.3），否则 shim 的广播封装会被 n2n 直接丢弃 |

注意 [n2n_win32.h](file:///Users/xtao/Work/n2n/n2n/src/win32/n2n_win32.h) 会把 `snprintf` 映射为 `_snprintf`、`strdup` 映射为 `_strdup`。胶水层如果依赖 NUL 结尾语义，应保证错误信息长度远小于缓冲区别名，或改用 `_snprintf_s`/`vsnprintf`。

### 4.5 Makefile 目标

| 目标 | 行为 |
| --- | --- |
| `make n2n-lib-win` | 调用 `scripts/build-n2n-win.sh` |
| `make win` | 依赖 `n2n-lib-win`，原生构建 `bin/n2n-windows-amd64.exe` 并复制 `wintun.dll` |
| `make win-clean` | 删除 `build/n2n-windows-*`、`third_party/lib/windows/`、`bin/n2n-windows-*`、`bin/wintun.dll` |
| `make clean` | 追加删除上述 Windows 产物 |

`make win` 在非 Windows 宿主上直接报错退出，并提示“请在 Windows 的 MSYS2 MinGW x64 shell 中构建”；首期不提供交叉编译别名，避免产出无法验证的二进制。

### 4.6 常见构建问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `Please run ./configure` | 未生成 `config.mak` | 执行 `autogen.sh && ./configure` |
| `windres: command not found` | 未安装 binutils，或不在 MinGW shell | `pacman -S mingw-w64-x86_64-binutils` |
| configure 未加入 win32 目标文件 | 宿主被识别为 msys | 显式传 `--host=x86_64-w64-mingw32` |
| `undefined reference to WSAStartup` | 未链接 `ws2_32` | 在 LDFLAGS 中补 `-lws2_32` |
| `undefined reference to SetIpInterfaceEntry` | 未链接 `iphlpapi` | 补 `-liphlpapi` |
| `undefined reference to CreateUnicastIpAddressEntry` | 同上 | 补 `-liphlpapi` |
| `_WIN32_WINNT` 相关 API 未声明 | 版本宏太低 | CFLAGS 加 `-D_WIN32_WINNT=0x0601` |
| `gcc: command not found`（go build 时） | 不在 MinGW shell 或 PATH 不含 mingw | 使用 “MSYS2 MinGW x64” shell |
| `file` 显示 Mach-O/ELF | 误用了非 Windows 构建 | 在 Windows 上构建，设置 `GOOS=windows GOARCH=amd64` |
| 目标机提示缺 `libgcc_s_seh-1.dll` | 动态链接了 mingw 运行时 | 使用 `-linkmode external -extldflags "-static"` |
| `make libn2n.a` 报 `src/win32/edge.rc` 找不到 manifest | `windres` 工作目录不对 | 在 `$WORK` 根目录执行 `make`，或给 `windres` 加 `-I src/win32` |

## 5. Windows 设备层（wintun + L3↔L2 shim）

### 5.1 驱动选型

| 方案 | 层次 | 与 n2n 的匹配度 | 结论 |
| --- | --- | --- | --- |
| wintun | 三层，无以太网帧/ARP | 需要新的设备层与 L3↔L2 shim | **首期采用** |
| tap-windows6 | 二层 TAP，暴露以太网帧 | n2n 原生支持（`src/win32/wintap.c`），但需预装内核驱动 | 不进首期产物，保留作参考/回退 |
| NDIS filter / 自研驱动 | 内核驱动 | 分发与签名成本高 | 不做 |

wintun 的公开事实（用于锁定实现范围）：

- 支持 Windows 7/8/8.1/10/11，首期只做 `AMD64`；
- 单文件 `wintun.dll`，官方预编译版本已签名，允许随应用分发；源码本身是 GPLv2，首期不分发源码、不做静态链接；
- 官方当前版本 0.14.1；DLL 必须与进程架构一致；
- 适配器由 `WintunCreateAdapter` 创建，`WintunCloseAdapter` 关闭时删除；不需要安装驱动、不需要重启；
- 创建适配器与配置 IP 都需要管理员权限。

### 5.2 wintun 生命周期

```
LoadLibraryExW(<exe 目录>\wintun.dll)
  → WintunGetRunningDriverVersion
  → WintunCreateAdapter(name, L"n2n", &deterministic_guid)
  → WintunStartSession(adapter, 0x400000)      # 4 MiB ring
  → WintunGetAdapterLuid / ConvertInterfaceLuidToIndex
  → 配置 IPv4 地址与 MTU（5.5）
  → 运行期：WintunReceivePacket / WintunAllocateSendPacket / WintunSendPacket
  → WintunEndSession
  → WintunCloseAdapter                          # 删除适配器
  → FreeLibrary
```

要点：

- 用 `GetModuleFileNameW` 拼出 exe 同目录的绝对路径再 `LoadLibraryExW`，避免从 PATH/当前目录加载到同名 DLL。
- `WintunCreateAdapter` 的 `RequestedGUID` 用适配器名 + 社区名做确定性派生（FNV-1a 128，按 UUID 规则设置 version/variant 位），避免每次启动生成新的 NLA（网络位置）注册表条目。
- 适配器名冲突（另一个 edge 正在运行）时，创建会失败；首期直接报错并提示换 `-d`，**不要**退回 `WintunOpenAdapter` 共享别人的适配器。
- 进程被强杀时，系统关闭句柄后适配器也会被删除，不会留下持久设备。
- `WintunGetRunningDriverVersion` 的返回值按 `major << 16 | minor` 解析后写日志；低于 0.14 直接拒绝启动。

### 5.3 L3↔L2 shim 设计

n2n 的 edge 只认识以太网帧；wintun 只吐/收 IP 包。shim 的职责：

```
Windows IPv4 栈
    │ IP 包
    ▼
[wintun session] ── WintunReceivePacket ──► tuntap_read
                                              │ 加 14 字节以太头
                                              │ dst = 学习表命中 ? 对端虚拟 MAC : ff:ff:ff:ff:ff:ff
                                              │ src = 本端虚拟 MAC
                                              ▼
                                        n2n edge → supernode → peer

peer → n2n edge → tuntap_write
    │ 以太网帧
    ├─ ARP request（target IP == 本端 IP）→ 合成 ARP reply，入队 → 下一次 tuntap_read 发出
    ├─ IPv4（dst MAC 是本端虚拟 MAC 或广播）→ 去头 → WintunSendPacket → Windows IPv4 栈
    ├─ 其他以太类型（含 IPv6 0x86DD）→ 丢弃
    └─ 从 IPv4 帧的 src 学习 IP→MAC
```

**虚拟 MAC**：

- `-m` 显式指定时直接使用（要求单播、非全零）；
- 未指定时按适配器名 + 社区名 + `-a` 地址 + 本机名做确定性派生，首字节固定 `0x02`（本地管理、单播）；
- 写入 `tuntap_dev.mac_addr`，并作为 shim 发出的所有以太帧的源 MAC；
- 同一节点重启后 MAC 不变，避免对端 n2n 的 MAC 表长期指向旧值。

**ARP 应答**：

- wintun 是 L3，Windows 自身不会发 ARP；但对端的 Linux/macOS TAP 会为 `-a` 地址发 ARP 请求；
- shim 收到 `0x0806`、opcode=1、target IP 等于本端地址时，合成 opcode=2 的 ARP reply（sender MAC/IP = 本端虚拟 MAC/IP，target = 请求方），放入读队列，由 `tuntap_read` 取走；
- 启动时额外入队一个 gratuitous ARP（广播，sender/target 都是本端地址），帮助对端与 n2n 边缘尽快学习虚拟 MAC；
- 首期只处理 IPv4 ARP；IPv6 的 NDP 不实现。

**出方向（tuntap_read）**：

- 先检查 ARP 回复队列，有则直接返回；
- 否则等待 wintun 读事件，`WintunReceivePacket` 取出一个 IP 包；
- 按首字节版本号选择 ethertype：IPv4 `0x0800`，IPv6 `0x86DD`（首期直接丢弃并继续读）；
- 目的 MAC：查学习表（按目的 IPv4）；未命中、广播地址 `255.255.255.255` 或 IPv4 组播时回退广播 `ff:ff:ff:ff:ff:ff`；
- 组播地址按 RFC 1112 映射为 `01:00:5e:xx:xx:xx` 也可行，但首期统一用广播，减少实现差异；
- 返回长度 = 14 + IP 包长度，直接写进 n2n 提供的 `buf`。

**入方向（tuntap_write）**：

- 长度 < 14 或以太类型不是 ARP/IPv4 时丢弃；
- ARP request 命中本端地址 → 入队 reply，不写 wintun；
- IPv4：校验目的 MAC 是本端 MAC 或广播后，去掉 14 字节头，`WintunAllocateSendPacket(len)` 拷贝后 `WintunSendPacket`；
- 从 IPv4 的源地址/源 MAC 更新学习表（TTL 60 秒，容量 64 项，LRU 淘汰）；
- ring 满导致 `WintunAllocateSendPacket` 返回 NULL 时丢弃该帧并记录限频日志，不阻塞 edge 主循环。

**组播与 `drop_multicast`（关键）**：

n2n 默认 `drop_multicast = 1`（[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L3179)），RX 与 TX 都会丢弃目的 MAC 为组播/广播的帧（[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L1769-L1773)、[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L2194-L2197)）。shim 的「未命中时广播封装」和 ARP 应答都依赖广播帧，因此：

- `n2n_go_edge_run` 在 Windows 分支中**强制** `e->conf.drop_multicast = 0`；
- CLI 的 `-E`/`--drop-multicast` 在 Windows 上不改变该值（始终接受），启动时打印一条说明；
- 这意味着 Windows edge 会透传广播/组播流量；在受控社区内可接受，后续如需收紧再引入「仅 ARP + 已知单播」策略。

**MTU**：wintun 适配器 MTU 设为 `-M`（默认 1290）。IP 包 ≤ 1290，加上 14 字节以太头后 1304，仍在 `N2N_PKT_BUF_SIZE` 内；隧道封装后的 UDP 报文也在常规 MTU 下。

### 5.4 `tuntap_*` 实现

`platform/windows/tuntap_wintun.c` 需要提供与 [wintap.c](file:///Users/xtao/Work/n2n/n2n/src/win32/wintap.c) 相同的符号：

| 函数 | 实现 |
| --- | --- |
| `tuntap_open(device, dev, mode, ip, mask, mac, mtu, metric)` | 解析 IP/掩码；创建/打开 wintun 适配器与会话；配置地址/MTU；初始化虚拟 MAC、学习表、ARP 队列、stop 事件；`metric` 首期忽略 |
| `tuntap_read(device, buf, len)` | 先取 ARP 队列；否则**先** `WintunReceivePacket`，仅在返回 `ERROR_NO_MORE_ITEMS` 时才 `WaitForMultipleObjects({read_event, stop_event})` 并重试；收到 IP 包后封装以太头返回；stop 时返回 0 |
| `tuntap_write(device, buf, len)` | ARP 应答入队；IPv4 去头后写入 wintun；更新学习表 |
| `tuntap_close(device)` | `WintunEndSession` → `WintunCloseAdapter` → 释放队列/名字 → `FreeLibrary`；stop 事件只 `ResetEvent`，保留到进程退出 |
| `tuntap_get_address(device)` | 空实现（与上游一致） |
| `tuntap_interrupt(device)` | `SetEvent(stop_event)`，供 `n2n_go_edge_stop` 调用 |
| `initWin32()` / `destroyWin32()` | `WSAStartup(2.2)` / `WSACleanup()`；胶水层不调用后者 |
| `win_print_available_adapters()` | 用 `GetAdaptersAddresses` 列出描述含 “Wintun” 的适配器，供打开失败时诊断 |

实现约束：

- 首期按「单进程单 edge」实现，wintun 会话、stop 事件、学习表放在文件级静态上下文；胶水层停止时调用 `tuntap_interrupt`。若后续同进程多 edge，需要把上下文挂到 `tuntap_dev` 并改为实例级。
- stop 事件不在 `tuntap_close` 中关闭：`n2n_go_edge_stop` 可能由信号 goroutine 与 run 线程的 teardown 并发调用，关闭句柄后再 `SetEvent` 会触发未定义行为。首期保留句柄到进程退出，每次 `tuntap_open` 前 `ResetEvent`。
- 设备名只接受 ASCII `[A-Za-z0-9_.-]`，长度 1–127；空名映射为 `n2n0`。这样 `MultiByteToWideChar(CP_UTF8)` 与日志都不需要处理复杂编码。
- `device_name` / `ifName` 用 `strdup` 分配，`tuntap_close` 负责释放；`device_handle` 置 `INVALID_HANDLE_VALUE`，保证 close 幂等。
- 所有诊断信息走 `traceEvent(TRACE_ERROR/TRACE_NORMAL, ...)`，不要用 `printf` 污染 stdout。

### 5.5 网络配置（IP Helper）

不要 `system("netsh ...")`。用 wintun 的 LUID 直接配置：

```c
NET_LUID luid;
WintunGetAdapterLuid(adapter, &luid);

/* IPv4 地址与前缀 */
MIB_UNICASTIPADDRESS_ROW addr;
InitializeUnicastIpAddressEntry(&addr);
addr.InterfaceLuid = luid;
addr.Address.si_family = AF_INET;
addr.Address.Ipv4.sin_addr.s_addr = inet_addr(ip);
addr.OnLinkPrefixLength = mask_to_prefix(mask);
addr.DadState = IpDadStatePreferred;
CreateUnicastIpAddressEntry(&addr);

/* MTU（Family=AF_INET；SitePrefixLength 必须为 0） */
MIB_IPINTERFACE_ROW row;
InitializeIpInterfaceEntry(&row);
row.InterfaceLuid = luid;
row.Family = AF_INET;
row.NlMtu = mtu;
row.SitePrefixLength = 0;
SetIpInterfaceEntry(&row);
```

要点：

- `DadState = IpDadStatePreferred` 跳过重复地址检测，避免点对点适配器上 DAD 超时；
- `SetIpInterfaceEntry` 若传入 `SitePrefixLength != 0` 会失败（上游 `wintap.c` 的注释也记录了这一点）；
- 适配器在 `WintunStartSession` 之后即为 Up，不需要 TAP 的 `TAP_IOCTL_SET_MEDIA_STATUS`；
- 不配置 IPv6 地址；Windows 仍可能在适配器上生成 link-local/ND 流量，shim 直接丢弃 `0x86DD`；
- 退出时不清理地址/MTU：适配器本身会被删除，配置随之消失。

### 5.6 停止与唤醒

`n2n_go_edge_stop` 在 Windows 上：

```c
void n2n_go_edge_stop(n2n_go_edge_t *e) {
    if(!e) return;
    e->stop_requested = 1;
    e->keep_running = 0;
#ifdef _WIN32
    tuntap_interrupt(&e->dev);
#endif
}
```

读侧（顺序很关键，不能先等事件）：

```c
for(;;) {
    packet = WintunReceivePacket(session, &size);
    if(packet != NULL)
        break;                          /* 有包，封装以太头后返回 */

    if(GetLastError() != ERROR_NO_MORE_ITEMS)
        return -1;                      /* ERROR_HANDLE_EOF 等按停止处理 */

    HANDLE ev[2] = { WintunGetReadWaitEvent(session), stop_event };
    DWORD w = WaitForMultipleObjects(2, ev, FALSE, INFINITE);
    if(w == WAIT_OBJECT_0 + 1)
        return 0;                       /* 停止，不再读 */
}
```

读事件是自动复位的（auto-reset），ring 中已积压多个包时事件早已被上一次读取消费。若先等事件再读一次，就会在仍有包可读的情况下永久阻塞，表现为“适配器 `SentBytes` 持续增长，但 edge 始终读不到包”。必须按 wintun.h 的规定：**先轮询 receive，`ERROR_NO_MORE_ITEMS` 才等事件**。

配合 `patches/n2n-win32-wintun.patch` 的唯一关键修改：`edge_read_from_tap` 在 `tuntap_read` 返回 `<= 0` 后先检查 `keep_running`：

```c
int len = tuntap_read(&eee->device, pktbuf, N2N_PKT_BUF_SIZE);
if(!*eee->keep_running)
    return;
if(len <= 0) {
    /* 原有的 sleep(3) + close + reopen 逻辑 */
}
```

这样停止时不会触发 `sleep(3)`、不会关闭/重建适配器，`run_edge_loop` 末尾的 `WaitForSingleObject(tun_read_thread, INFINITE)`（[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L3058-L3060)）会立即返回。无需 `CancelIoEx`，也不需要暴露 `getTunReadThread()`。

`n2n-keep-running.patch` 继续复用：必须移除 `run_edge_loop` 入口的 `*eee->keep_running = true;`（[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L2897)），否则会覆盖 Go 侧在进入循环前发出的停止请求。

### 5.7 设备名与参数映射

`-d` 在 wintun 模式下是**要创建的适配器名**，不再是“从已安装 TAP 中选择”：

- 空：默认 `n2n0`；
- 非空：1–127 个 ASCII 字符，允许字母、数字、`_`、`.`、`-`；
- 适配器随进程创建/删除，不跨重启保留。

| CLI 参数 | C 目标 | Windows/wintun 说明 |
| --- | --- | --- |
| `-c, --community` | `conf.community_name` | 20 字节数组，最多 19 字节；超长在 Go 层报错 |
| `-l, --supernode` | `edge_conf_add_supernode` | 可重复；首期取第一个 |
| `-a, --ip` | `tuntap_open` 的 `device_ip` / `device_mask` | 静态 IPv4；`/24` 常见 |
| `-k, --key` | `conf.encrypt_key` | `strdup` 后交给 conf |
| `-A, --cipher` | `conf.transop_id` | `none/aes/cc20/speck/tf` |
| `-z, --compress` | `conf.compression` | `none/lzo`（zstd 首期不启用） |
| `-d, --device` | wintun 适配器名 | 空 → `n2n0`；见上 |
| `-m, --mac` | shim 的虚拟 MAC | 默认确定性派生；指定时只影响 shim，不写注册表、不重启适配器 |
| `-M, --mtu` | 适配器 MTU | 默认 1290，范围 68–1500；通过 `SetIpInterfaceEntry` 设置 |
| `-p, --local-port` | `conf.local_port` | 0 表示自动 |
| `-t, --mgmt-port` | `conf.mgmt_port` | 多 edge 共存时必须错开 |
| `-i / -L` | `conf.register_interval/register_ttl` | 默认沿用 n2n |
| `-r, --routing` | `conf.allow_routing` | 布尔 |
| `-E` / `--drop-multicast` | `conf.drop_multicast` | **wintun 模式强制为 0**（见 5.3）；`-E` 语义与原生 n2n 一致 |
| `-T, --tos` | `conf.tos` | 0–255 |
| `-I, --description` | `conf.dev_desc` | 16 字节数组，最多 15 字节 |
| `-f, --foreground` | 无 | 兼容保留，接受但忽略 |

Go 侧要做的平台化：

- `internal/config/device_windows.go`：空名合法并默认 `n2n0`；非空校验为 1–127 个 `[A-Za-z0-9_.-]`；`DeviceNameMax` 改为平台函数 `deviceNameMax()`（Windows 返回 128，即可用 127 字节）；
- [config.go](../internal/config/config.go#L184-L191) 的 `if c.Device == "" { c.Device = "utun" }` 与 `isUTUNName` 改为调用平台函数 `defaultDeviceName()` / `validateDeviceName()`；
- [config.go](../internal/config/config.go#L200-L215) 的 `randomMAC()` 在 Windows 上不生成：`-m` 为空时把空串交给 C，由 shim 确定性派生；显式 `-m` 时校验单播与非全零；
- [CLI edge.go](../internal/cli/edge.go#L87-L88) 的 `-d` 默认值改为取平台默认，帮助文本（[edge.go](../internal/cli/edge.go#L203)）改为“Windows: wintun 适配器名，空表示 n2n0”；
- [edge_glue.c](../internal/n2n/edge_glue.c#L183) 的兜底设备名改为 `""`。

### 5.8 权限与分发

- 创建 wintun 适配器、配置 IP/MTU 都需要管理员。启动前检测当前进程是否提升：Windows 上 `os.Geteuid()` 恒为 -1，改用 `advapi32!GetTokenInformation(TokenElevation)` 或简化为 `shell32!IsUserAnAdmin`；未提升时提示 “run this command from an Administrator terminal” 并退出。
- 不默认嵌入 `requireAdministrator` 清单：否则 `n2n version` 也会弹 UAC。若后续做安装器/快捷方式，再为 `n2n.exe` 提供带清单的入口（`platform/windows/n2n.manifest` + `windres -O coff -o cmd/n2n/n2n.syso`）。
- `wintun.dll` 必须与 exe 同目录、同架构；构建脚本负责复制，启动时校验文件存在与版本。官方签名 DLL 允许随应用分发，发布包内保留其原始文件名与许可证说明。
- 不需要预装驱动、不需要重启；卸载/退出时适配器自动删除。
- Windows 防火墙会拦截入站 ICMP：wintun 适配器默认归入 Public 配置文件，系统内置 ICMPv4-In 规则在该配置下为 Disabled，对端 ping 本机会 100% 丢包。文档（7.2）给出放行命令，首期不自动修改防火墙。

## 6. 生命周期与并发

### 6.1 初始化

- 首次 `n2n_go_edge_create` 调用 `initWin32()`（`WSAStartup(2.2)`）。Go 的 `net` 包也可能已经初始化 Winsock；重复 `WSAStartup` 是引用计数安全的。
- **不要调用 `destroyWin32()`**：`WSACleanup()` 会把引用计数减一，可能让 Go 运行时的残留 socket 失效。进程退出时由系统回收。
- `n2n_srand(n2n_seed())` 在 `edge_init` 内部完成，无需额外处理。
- wintun DLL 的加载放在 `tuntap_open` 内，失败时返回 -1 并给出明确错误；不要在 `DllMain`/全局构造阶段加载。

### 6.2 运行

`n2n_go_edge_run` 的顺序与 macOS 一致，但设备相关部分按平台分流：

```c
edge_init_conf_defaults(&e->conf);
/* 字段映射：community、key、transop_id、compression、端口、注册参数 ... */

#ifdef _WIN32
/* wintun 的广播封装与 ARP 应答依赖广播帧透传 */
e->conf.drop_multicast = 0;
#endif

if(edge_verify_conf(&e->conf) != 0) { ... }

e->eee = edge_init(&e->conf, &rc);
if(!e->eee) { ... }

/* Windows: dev 为空串表示默认 n2n0 */
if(tuntap_open(&e->dev, dev_name, "static", ip, mask, mac, mtu, 0) < 0) {
    win_print_available_adapters();
    ...
}
memcpy(&e->eee->device, &e->dev, sizeof(e->dev));

e->eee->keep_running = &e->keep_running;
if(e->stop_requested)
    e->keep_running = 0;
n2nGoOnStarted(e->started_handle);
rc = run_edge_loop(e->eee);
```

`tuntap_priv_conf` 仍需填充，供读错误后的重开路径使用：

```c
#ifdef _WIN32
const char *actual = e->dev.device_name ? e->dev.device_name
                   : (e->dev.ifName ? e->dev.ifName : "");
char *dst = e->eee->tuntap_priv_conf.tuntap_dev_name;
strncpy(dst, actual, sizeof(e->eee->tuntap_priv_conf.tuntap_dev_name) - 1);
dst[sizeof(e->eee->tuntap_priv_conf.tuntap_dev_name) - 1] = '\0';
#else
strncpy(e->eee->tuntap_priv_conf.tuntap_dev_name, e->dev.dev_name, ...);
#endif
```

`run_edge_loop` 内部会自行创建读线程（[edge_utils.c](file:///Users/xtao/Work/n2n/n2n/src/edge_utils.c#L2891-L2895)），胶水层不重复创建。

### 6.3 停止（关键）

`n2n_go_edge_stop` 只做两件事：置 `keep_running = 0`、`tuntap_interrupt` 唤醒读线程。读线程返回 0 后，补丁阻止 `edge_read_from_tap` 进入重开路径，`run_edge_loop` 的 `WaitForSingleObject` 随即返回。

如果漏掉 5.6 的补丁，停止时会看到 “device read error” 日志并卡 3 秒，然后重新创建适配器；这是首期必须验证的回归点。

### 6.4 清理

- `run_edge_loop` 返回后执行 `edge_term_conf → tuntap_close → edge_term`。
- `tuntap_close` 顺序：`WintunEndSession` → `WintunCloseAdapter` → 释放 ARP 队列与 `device_name`/`ifName` → `FreeLibrary`；stop 事件只 `ResetEvent`（见 5.4）；`device_handle` 置 `INVALID_HANDLE_VALUE`，保证 `destroy` 幂等。
- 关闭的对象应是 `e->eee->device`（读错误重开后的当前句柄），不是创建时的 `e->dev` 浅拷贝。wintun 首期正常情况下不会走重开路径，但仍按同一约定处理。
- `e->eee->device` 与 `e->dev` 是浅拷贝关系：`tuntap_close` 释放名字后，另一份中的同名指针悬空。`edge_term` 不使用这些字段，后续维护时应避免在 close 之后访问。

## 7. 测试方案

### 7.1 构建与静态检查

在 Windows 的 MSYS2 MinGW x64 shell 中：

```bash
./scripts/build-n2n-win.sh
file third_party/lib/windows/amd64/libn2n.a
ar t third_party/lib/windows/amd64/libn2n.a | grep -E 'wintap|edge_utils_win32'

./scripts/build-windows.sh
file bin/n2n-windows-amd64.exe        # PE32+ executable (console) x86-64
objdump -p bin/n2n-windows-amd64.exe | grep 'DLL Name'
```

`file` 应显示 `PE32+ executable (console) x86-64, for MS Windows`；导入表里除系统 DLL 外不应出现 mingw 运行时 DLL。

### 7.2 Windows 环境准备

```powershell
# 1. 把官方 wintun.dll 放到 exe 同目录（构建脚本已复制）
Get-Item .\wintun.dll | Select-Object Name, Length, VersionInfo

# 2. 以管理员身份运行；确认版本输出包含 wintun 驱动版本
.\n2n-windows-amd64.exe version

# 3. 放行入站 ping（必需，否则对端 ping 本机 100% 丢包）
#    注意：wintun 适配器默认归入 Public 配置文件，且内置 ICMPv4-In 规则在
#    Private/Public 下默认 Disabled，因此必须显式放行。
New-NetFirewallRule -DisplayName "n2n ICMPv4 echo in" -Protocol ICMPv4 `
  -IcmpType 8 -Direction Inbound -Action Allow -Profile Any
```

测试用 supernode 可选用：在 Windows 上运行 n2n 源码构建的 `supernode.exe`，或复用局域网中已有的 supernode。

### 7.3 单节点注册

```powershell
.\n2n-windows-amd64.exe edge -c demo -k demo -a 10.0.0.1/24 `
  -l 192.168.1.10:7777 -t 15644
```

预期：

- 日志出现 `wintun driver version=0.14.x`、`adapter=n2n0`、`ip=10.0.0.1/24`；
- 出现 `[OK] edge <<< ... >>> supernode`；
- `Get-NetAdapter -Name n2n0` 存在，`InterfaceDescription` 为 `Wintun Userspace Tunnel`，状态 `Up`；
- `Get-NetIPAddress -InterfaceAlias n2n0 -AddressFamily IPv4` 显示 `10.0.0.1`；
- `Get-NetIPInterface -InterfaceAlias n2n0 -AddressFamily IPv4` 的 `NlMtu` 为 1290。

### 7.4 双节点连通（跨机/跨平台）

Windows 没有 netns，同机两个 edge 需要两个适配器，且同网段时 Windows 可能把目的地址判为本机直连，绕过隧道，结果不可靠。首期验收采用**跨机**：

| 组合 | 用途 | 说明 |
| --- | --- | --- |
| Windows wintun ↔ Linux TAP | 主验收 | 验证 ARP 应答与 L3↔L2 封装 |
| Windows wintun ↔ macOS utun | 互操作验证 | 复用现有 macOS 产物 |
| Windows wintun ↔ Windows wintun | 可选 | 两端都不 ARP，验证广播/学习路径 |

两端使用同社区、同 key、不同静态地址：

```powershell
# Windows 端
.\n2n-windows-amd64.exe edge -c demo -k demo -a 10.0.0.1/24 -l <supernode>:7777
```

```bash
# macOS 端（现有产物；Linux 端按 docs/Linux.md 构建后同理）
sudo ./bin/n2n-darwin-arm64 edge -c demo -k demo -a 10.0.0.2/24 -l <supernode>:7777
```

验证：

```powershell
ping -n 4 10.0.0.2
Get-NetAdapterStatistics -Name n2n0
```

```bash
# 在 TAP 对端确认 shim 的 ARP 应答与虚拟 MAC
arp -a | grep 10.0.0.1
```

断言：

- 两端均在 30 秒内注册成功；
- `ping` 0% 丢包（Windows 防火墙放行 ICMP 的前提下）；
- TAP 对端的 `arp -a` 中出现 `10.0.0.1` 对应的虚拟 MAC（首字节 `02`）；
- `Get-NetAdapterStatistics` 的 `ReceivedBytes`/`SentBytes` 随 ping 增长；
- Windows 侧 `arp -a` 没有该适配器的条目属正常现象（L3 设备不产生 ARP）。

**验证记录（2026-11-25）**：Windows 10/11 x64 主机（edge `10.10.10.20/24`，适配器 `n2n0`）
↔ WSL2 Ubuntu-24.04（Linux glibc 产物，edge `10.10.10.21/24`），supernode 跑在 WSL 内。
结果：Windows `ping 10.10.10.21` 4/4 通（TTL=64，7–14ms），WSL `ping 10.10.10.20` 3/3 通
（TTL=128，约 6ms），双向 0% 丢包。前提是已按 7.2 放行入站 ICMPv4 echo（Public 配置文件）。
注意：wintun 是 L3 适配器（`MediaType=IP`、无 MAC），`New-NetNeighbor -LinkLayerAddress`
会报 `ERROR_INVALID_PARAMETER`，ARP 由 shim 自行应答，不需要也不要配置静态邻居。

### 7.5 停止与清理

```powershell
# 在 edge 窗口按 Ctrl+C
$LASTEXITCODE            # 应为 0

Get-Process n2n-windows-amd64 -ErrorAction SilentlyContinue   # 应无残留
Get-NetAdapter -Name n2n0 -ErrorAction SilentlyContinue       # 应不存在
```

预期：进程退出、无残留读线程；wintun 适配器被删除（与 TAP 的持久设备不同），IP/MTU 配置一并消失，不需要额外清理。

### 7.6 冒烟脚本

`scripts/smoke-test-windows.ps1` 串起：检查 `wintun.dll` 与管理员权限 → 启动 edge → 等待注册 → 检查 `Get-NetAdapter`/`Get-NetIPAddress` → 由对端发起 `ping` → 发送 `Ctrl+C`/`Stop-Process` → 检查退出码、残留进程与适配器是否删除。脚本应在失败时统一清理子进程，避免残留 edge 占住适配器名。

跨平台冒烟建议放在 macOS/Linux 侧编排：启动 supernode + macOS/Linux edge，Windows edge 作为被测端，由脚本远程执行 PowerShell 或人工执行。

### 7.7 常见运行错误

| 报错 | 原因 | 修正 |
| --- | --- | --- |
| `wintun.dll not found` / `LoadLibrary failed` | DLL 不在 exe 同目录或架构不匹配 | 复制 `bin/amd64/wintun.dll`；确认进程是 x64 |
| `wintun driver version too old` | DLL 版本低于 0.14 | 更新到官方 0.14.1 |
| `ERROR_ACCESS_DENIED` / 创建适配器失败 | 非管理员 | 以管理员身份运行 |
| `adapter "n2n0" already exists` | 另一个 edge 正在运行 | 换 `-d`，或先结束占用进程 |
| `CreateUnicastIpAddressEntry failed` | 地址冲突/权限/IP 非法 | 换地址；确认管理员；检查 `-a` 格式 |
| `SetIpInterfaceEntry failed` | `SitePrefixLength` 非 0 或 MTU 越界 | 实现按 5.5 清零；MTU 在 68–1500 |
| 注册成功但 `ping` 100% 丢包 | 防火墙拦截 ICMP；或同机双适配器路由绕过隧道 | 放行 ICMP；改用跨机验收 |
| 对端收不到 ARP 应答 | `drop_multicast` 未被置 0，广播被 n2n 丢弃 | 确认 6.2 的强制赋值与启动日志 |
| 停止时卡住数秒 | 缺少 5.6 的 `keep_running` 检查 | 应用 `patches/n2n-win32-wintun.patch` |
| 目标机提示缺 DLL | mingw 运行时未静态链接 | 使用 `-extldflags "-static"` |

## 8. 任务拆解与验收

| 序号 | 任务 | 验收 |
| --- | --- | --- |
| T1 | Windows + MSYS2 MinGW x64 工具链 + `scripts/build-n2n-win.sh` 骨架 | `gcc --version`、`windres --version`、`go version` 正常 |
| T2 | `platform/windows/tuntap_wintun.c`：DLL 加载、适配器/会话创建、虚拟 MAC、ARP 队列 | 单节点可创建/删除 `n2n0`，`version` 打印 wintun 版本 |
| T3 | L3↔L2 shim：IPv4 封装/解封装、ARP 应答、学习表、广播回退 | 与 TAP 对端互 ping 通；对端 `arp -a` 可见虚拟 MAC |
| T4 | IP Helper 配置、`patches/n2n-win32-wintun.patch`、`n2n-keep-running.patch` | 适配器有地址/MTU；`Ctrl+C` 无 3 秒卡顿、无重开 |
| T5 | 胶水层 `_WIN32` 分支 + `device_windows.go` + 权限检测 | macOS/Linux 构建保持通过；Windows 参数错误在 Go 层报出 |
| T6 | `link_windows_amd64.go` + Makefile 目标 + 静态链接 + 拷贝 DLL | 产出 `bin/n2n-windows-amd64.exe` 与 `bin/wintun.dll`；`file` 为 PE32+ |
| T7 | Windows 上启动 `n2n edge` 并注册 supernode | 适配器 Up、地址正确、注册成功 |
| T8 | 跨机/跨平台双向连通 + 停止清理 + 冒烟脚本 | `ping` 0% 丢包；退出码 0；适配器删除、无残留进程 |

## 9. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| wintun 是 L3，n2n 是 L2 | 帧语义不匹配 | 首期实现 shim（封装/解封装 + ARP 应答 + MAC 学习），并明确非 IP 协议不透传 |
| 广播封装依赖组播透传 | 默认配置下 ARP/首包被丢弃 | 强制 `drop_multicast = 0`，并在启动日志中显式提示 |
| 未命中时广播 | 首包延迟、广播流量增加 | 收到对端帧后学习 IP→MAC，60 秒 TTL 内走单播；后续可加 ARP 客户端主动解析 |
| IPv6/NDP 不支持 | IPv6 用户不可用 | 首期不配置 IPv6；shim 丢弃 `0x86DD`；文档声明 |
| wintun.dll 缺失/版本不匹配/架构不符 | 无法启动 | 构建脚本复制；启动校验版本与架构；发布包固定 0.14.1 |
| 需要管理员/UAC | 普通用户无法启动 | 启动前提升检测 + 明确提示；后续安装器/服务化 |
| 适配器名冲突 | 创建失败 | 报错并提示换 `-d`；不共享他人适配器 |
| Ring 满/丢包 | 突发流量丢帧 | `WintunAllocateSendPacket` 失败即丢并限频告警，不阻塞主循环 |
| 依赖 MSYS2 包版本漂移 | 构建结果不一致 | 固定 pacman 包版本或 CI 镜像；记录工具链版本 |
| cgo 动态依赖 mingw DLL | 目标机缺 DLL | `-extldflags "-static"`；发布前检查导入表 |
| n2n 升级导致补丁失效 | 构建中断 | 补丁脚本化；升级时先应用再编译；CI 校验 |
| Windows 防火墙 | 注册成功但 ping 不通 | 放行 ICMP；文档给出 PowerShell 命令 |
| 同机双 edge 路由绕过 | 误判隧道不通 | 首期只用跨机验收；同机仅做注册冒烟 |
| arm64 Windows | 覆盖不足 | 首期只做 amd64；arm64 需要对应的 wintun.dll 与 `aarch64-w64-mingw32` 工具链单独评估 |

## 10. 首期交付物

- 可执行文件：`bin/n2n-windows-amd64.exe` + `bin/wintun.dll`
- 静态库：`third_party/lib/windows/amd64/libn2n.a`
- 构建脚本：`scripts/build-n2n-win.sh`、`scripts/build-windows.sh`
- 补丁：`patches/n2n-win32-wintun.patch`（复用 `patches/n2n-keep-running.patch`）
- 平台文件：`platform/windows/tuntap_wintun.c`、`platform/windows/tuntap_wintun.h`、`internal/config/device_windows.go`、`internal/cli/permission_windows.go`、`internal/n2n/link_windows_amd64.go`
- 测试：`scripts/smoke-test-windows.ps1`
- 文档：本文件 + `docs/N2NGo.md` + `docs/macOS.md` + `docs/Linux.md`

## 11. 不做 Git 操作的说明

- 不执行 `git init`、不添加 `.gitignore`、不引入 submodule；
- n2n 源码每次从 `N2N_SRC`（默认 `/c/Work/n2n/n2n`，按 Windows 路径调整）复制，构建产物与源码分离；
- 若后续需要固定版本，再单独补充 Git 方案。

## 12. 待确认问题

1. 首期是否必须支持 Windows wintun ↔ Linux/macOS TAP 的双向互通？（本方案按“必须”设计，因此实现了 ARP 应答与 MAC 学习）
2. 是否需要首期支持 IPv6/NDP？当前方案明确不支持。
3. `wintun.dll` 是固定随包分发 0.14.1，还是允许用户自行替换并只做最低版本校验？
4. 是否接受首期只支持 `windows/amd64`，`arm64` 延后？
5. 是否需要保留 tap-windows6 作为可切换的设备层（当前只作为上游参考）？
6. 是否需要首期暴露 `-x <metric>` 或自动修改防火墙？
