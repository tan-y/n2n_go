# n2n-go

用 Go 封装的 [n2n](https://github.com/ntop/n2n) 3.1.1 edge 客户端，通过 cgo 链接
`libn2n.a`，在保持原生 n2n 行为的前提下提供更严格的参数校验和更清晰的 CLI。

当前支持 macOS 与 Linux，Windows 仅有设计文档。

## 状态

| 平台 | 状态 | 产物 |
| --- | --- | --- |
| macOS arm64 / amd64 | 首期完成（arm64 已联调） | `bin/n2n-darwin-arm64`、`bin/n2n-darwin-amd64` |
| Linux arm64 musl | 完成并已联调 | `bin/n2n-linux-arm64-musl`（静态） |
| Linux arm64 glibc | 完成并已联调 | `bin/n2n-linux-arm64-glibc`（Debian 11 构建，实测最高符号 GLIBC_2.17） |
| Linux amd64 | 构建脚本已就绪，尚未实际验证 | `make linux-amd64` / `make linux-amd64-glibc` |

已知限制：

- 仅支持 IPv4 地址配置；`-a auto` 由 supernode 分配地址。
- 不支持 DHCP，不配置全局 IPv6 地址；`--ipv6` 只承载 IPv6 报文。
- Linux 的 edge 需要 `CAP_NET_ADMIN` 和 `/dev/net/tun`。

## 目录结构

```text
cmd/n2n/              CLI 入口
internal/cli/         子命令与参数解析
internal/config/      配置校验、JSON 文件、平台设备名规则
internal/n2n/         cgo 胶水层与 n2n 绑定
docker/linux/         Linux 构建镜像与运行镜像
scripts/              构建、联调、互操作测试脚本
third_party/n2n/      n2n 3.1.1 submodule
docs/                 各平台设计与落地文档
```

## 环境要求

- Go 1.25+
- C 工具链、autoconf、automake、make
- macOS：Xcode Command Line Tools
- Linux 产物推荐直接用 Docker 构建，无需在宿主机装交叉工具链
- 已初始化 submodule：

```bash
git submodule update --init third_party/n2n
```

## 构建

macOS：

```bash
make build        # arm64
make build-amd64  # amd64
make build-all    # 两者
```

Linux（在匹配架构的容器内构建）：

```bash
make linux-arm64         # arm64 + musl，静态链接
make linux-arm64-glibc   # arm64 + glibc，动态链接
make linux-amd64         # amd64 + musl（本机为 arm64 时走模拟）
make linux-amd64-glibc   # amd64 + glibc
```

产物输出到 `bin/`，Linux 同时生成测试用的 `supernode`。`make linux-clean`
清理 Linux 产物，`make clean` 清理全部构建结果。

## 快速开始

macOS 需要 root：

```bash
sudo ./bin/n2n-darwin-arm64 edge \
  -c mynet -k mysecret \
  -a 10.0.0.1/24 \
  -l supernode.example.com:7777
```

Linux 容器运行：

```bash
docker run --rm -it \
  --cap-add NET_ADMIN \
  --device-cgroup-rule 'c 10:200 rwm' \
  --entrypoint /bin/sh \
  n2n-go-linux:arm64-musl -c '
    mkdir -p /dev/net
    [ -e /dev/net/tun ] || mknod /dev/net/tun c 10 200
    exec /usr/local/bin/n2n edge \
      -c mynet -k mysecret -a 10.0.0.1/24 \
      -l supernode.example.com:7777'
```

macOS 设备名默认 `utun`，由内核分配 `utunN`；Linux 留空 `-d` 时由内核分配
`tapN`，启动日志会打印实际设备名。

常用参数：

```text
-c, --community     社区名，最长 19 字节
-l, --supernode     supernode 地址，可重复，最多 8 个
-a, --ip            IPv4/CIDR 或 auto
-k, --key           加密 key
-A, --cipher        none|aes|cc20|speck|tf
-z, --compress      none|lzo
-d, --device        macOS: utun/utunN；Linux: tapN 或留空自动分配
-M, --mtu           MTU，默认 1290
-I, --description   节点描述，最长 15 字节
```

完整参数见 `n2n edge -h`。

## 配置文件

`-config` 可指定 JSON 文件，命令行参数优先于文件；未知字段会直接报错。

```json
{
  "community": "mynet",
  "supernodes": ["supernode.example.com:7777"],
  "address": "10.0.0.1/24",
  "key": "mysecret",
  "cipher": "aes",
  "compression": "none",
  "device": "",
  "mtu": 1290,
  "description": "edge-1"
}
```

## 测试

```bash
make check          # gofmt + vet + 单测
make smoke          # macOS 双节点联调
make multicast      # IPv4 组播
make auto-address   # supernode 分配地址
make ipv6           # IPv6 承载
make interop        # 与原生 n2n 互操作

make linux-smoke        # Linux arm64 musl 双节点联调
make linux-smoke-glibc  # Linux arm64 glibc 双节点联调
```

Linux 冒烟脚本会检查注册、`ping` 0% 丢包、TAP 计数、自动 `tapN`、启动日志设备名，
以及 SIGTERM/SIGINT 退出码为 0、退出后 TAP 接口消失。

## 文档

- [docs/N2NGo.md](docs/N2NGo.md)：总体方案
- [docs/macOS.md](docs/macOS.md)：macOS 首期落地
- [docs/macOS-p5.md](docs/macOS-p5.md)：macOS 后续能力（组播、IPv6、互操作等）
- [docs/Linux.md](docs/Linux.md)：Linux 构建、补丁与联调
- [docs/Windows.md](docs/Windows.md)：Windows 设计（尚未实现）

## 故障排查

- `operation not permitted`：edge 需要 root（macOS）或 `CAP_NET_ADMIN`（Linux）。
- `/dev/net/tun` 不存在：容器内执行 `mknod /dev/net/tun c 10 200`，并确认宿主机加载了 `tun` 模块。
- 两个节点拿到相同地址：使用 `-a auto` 时给每个节点不同的 `-I` 描述。
- Linux glibc 产物无法启动：确认目标系统的 glibc 版本满足产物符号要求（实测最高 `GLIBC_2.17`）。
