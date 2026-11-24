# AGENTS.md

本文件面向在本仓库中工作的编码代理。修改代码或脚本前先读这里，命令与约束以本文件为准。

## 项目概览

- Go 1.25 CLI，通过 cgo 链接 n2n 3.1.1 的 `libn2n.a`，入口为 `cmd/n2n`。
- 平台：macOS（arm64/amd64）、Linux（arm64 musl/glibc 已验证；amd64 仅脚本就绪）、
  Windows（amd64 本地冒烟通过，跨主机数据面未验证）。
- 总体方案见 [docs/N2NGo.md](docs/N2NGo.md)，各平台落地见 `docs/`。

## 常用命令

```bash
git submodule update --init third_party/n2n

make build              # macOS arm64
make build-amd64        # macOS amd64
make check              # gofmt + vet + 单测
make smoke              # macOS 双节点联调
make linux-arm64        # Linux arm64 musl
make linux-arm64-glibc  # Linux arm64 glibc
make linux-smoke        # Linux musl 联调
make linux-smoke-glibc  # Linux glibc 联调
make n2n-lib-win        # Windows 静态库（MSYS2 MinGW-w64）
make win                # Windows amd64
make win-smoke          # Windows 本地冒烟（管理员 PowerShell）
make clean              # 清理全部构建产物
```

- 全新检出后先构建 `libn2n.a`（`make build` 会自动做），否则 `make check` 的 cgo 链接会失败。
- macOS 上缺少 autoconf/automake 时用 `nix-shell shell.nix --run 'make build'`。
- Linux 产物在匹配架构的容器内构建，宿主机不需要交叉工具链。

## 代码边界

- `internal/cli/edge.go`：flag 定义、CLI 覆盖配置文件的顺序、信号处理。
- `internal/config/config.go`：唯一的校验入口，`Validate()` 之后再进入 cgo。
- `internal/config/file.go`：JSON schema；新增字段必须同时改 `FileConfig`、`Apply` 和测试。
- `internal/n2n/`：cgo 胶水与链接参数；平台/架构差异用 build tag 文件表达，不要用运行时分支。
- `internal/config/device_*.go`：macOS 只接受 `utun`，Linux 留空表示内核分配 `tapN`，Windows 默认 `n2n0`。
- `internal/cli/permission_*.go`：Windows 检查管理员权限，Unix 检查 root 或 `CAP_NET_ADMIN`。
- `internal/n2n/link_windows_amd64.go`：Windows 专用链接参数与头文件路径。
- `platform/windows/tuntap_wintun.c`：wintun 设备层与 L3↔L2 shim，构建时覆盖上游 `src/win32/wintap.c`，禁止直接改 submodule。
- 常量（社区名长度、MTU、transform/compression ID）镜像 n2n 定义，改动前先核对 `third_party/n2n/include`。

## n2n 源码与补丁

`third_party/n2n` 是只读 submodule，禁止直接修改。所有改动走：

- `patches/*.patch`：构建时应用到源码副本。
- `platform/macos/tuntap_osx.c`：macOS 的 TUN/TAP 适配层，由构建脚本覆盖上游文件。
- `platform/windows/tuntap_wintun.c`：Windows 的 wintun 适配层，由构建脚本覆盖上游 `src/win32/wintap.c`。

构建流程：

- macOS：`scripts/build-n2n.sh` 复制源码到 `build/n2n-<arch>`，覆盖 `tuntap_osx.c`，应用 `n2n-keep-running.patch`。
- Linux：`scripts/build-n2n-linux.sh` 复制到 `build/n2n-linux-<arch>-<libc>`，应用 `n2n-keep-running.patch` 与 `n2n-tuntap-linux.patch`。
- Windows：`scripts/build-n2n-win.sh` 在 MSYS2 MinGW-w64 下复制到 `build/n2n-windows-amd64`，覆盖 `src/win32/wintap.c` 并同步 `src/win32` 头文件，应用 `n2n-win32-wintun.patch`。

注意：

- 两个 Linux 变体共用 `third_party/lib/linux/<arch>/libn2n.a` 作为 cgo 链接路径；每次构建都会覆盖，切换 libc 后必须重新构建再链接。
- cgo 不会对 `.a` 内容做哈希，Makefile 与脚本里的 `-a` 不能删，否则改库后可能复用旧缓存。
- 新增 C 行为修复时新建 patch，不要改 submodule，也不要只在容器里手改。

## 验证要求

- 任何 Go 改动：`make check` 必须通过。
- 涉及配置校验：确认 macOS 与 Linux 的测试都通过；测试断言不要写死 `utun`，按 `runtime.GOOS` 分支。
- 涉及 cgo、补丁、链接或构建脚本：至少跑对应平台的构建，Linux 还要跑 `make linux-smoke`；改了 glibc 路径再跑 `make linux-smoke-glibc`。
- 涉及 Windows 的 wintun/cgo 改动：在 MSYS2 MinGW-w64 下跑 `make win`，再在管理员 PowerShell 跑 `make win-smoke`。
- 同机双 edge 的 `ping` 会被 Windows 本机路由短路（TTL=128、<1ms），不能作为隧道数据面证据；跨主机连通性需真实对端。
- 涉及容器/权限逻辑：保留 `CAP_NET_ADMIN`、`/dev/net/tun` 与 TAP 生命周期检查。
- 不要声称 amd64 Linux 已可用，除非实际构建并运行过；当前它只是脚本就绪。
- 报告结果时区分“已验证”和“仅实现”。

## 环境注意事项

- Docker Hub 在当前环境不可用；Linux 基础镜像可能需要从镜像源拉取后本地打标签。
- Debian 11 已归档，`Dockerfile.glibc` 与运行镜像使用 `http://mirrors.aliyun.com/debian-archive/`。不要换回 `archive.debian.org`（会 502）或 `https`（slim 镜像无 CA 证书）。
- Go 下载使用 `golang.google.cn`。
- Windows 构建需要 MSYS2 MinGW-w64（gcc、windres、autoconf、automake、rsync、patch）；`third_party/wintun/amd64/wintun.dll` 需从 wintun 0.14.1 官方包放入，该目录被 `.gitignore` 忽略。
- 部分 Windows 环境缺少 `Remove-NetAdapter`；清理 wintun 适配器的脚本要做命令存在性检查。

## 编码约定

- Go 注释与标识符用英文；CLI 帮助文本、日志与文档用中文，保持现有风格。
- 错误信息小写、可操作，需要保留原因时用 `%w` 包装。
- 不要记录 `key`、`auth_password` 等敏感值。
- 不引入新依赖，除非现有标准库无法完成。
- 新增配置字段时同步更新 README 示例、JSON 测试和 `docs/` 对应说明。

## 提交前检查

1. `gofmt`、`go vet`、`go test` 通过。
2. 相关 smoke 测试通过。
3. Makefile、README、docs 与实际命令一致。
4. `bin/`、`build/`、`third_party/lib`、`third_party/n2n-src` 等生成物未被加入版本控制。
5. 没有把密钥或本地路径写进文档与示例。
