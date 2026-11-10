# n2n_go 的 macOS 构建环境
# 固定 nixpkgs 快照，仅提供 autotools 相关工具；Apple clang 与 macOS SDK 仍来自 Xcode CLT。
{
  pkgs ? import (builtins.fetchTarball {
    url = "https://mirrors.tuna.tsinghua.edu.cn/nix-channels/nixpkgs-unstable/nixexprs.tar.xz";
    sha256 = "1pmz0z8gpmrfvw7mg42qkp4iwi7bq0ddp1mw4q7hkvdrcf4qvgr4";
  }) {}
}:

pkgs.mkShell {
  packages = [
    pkgs.autoconf
    pkgs.automake
    pkgs.pkg-config
    pkgs.gnumake
  ];

  shellHook = ''
    export CC=/usr/bin/clang

    echo "n2n_go dev shell (macOS)"
    echo "  autoconf   $(autoconf --version | head -n1)"
    echo "  automake   $(automake --version | head -n1)"
    echo "  pkg-config $(pkg-config --version)"
    echo "  make       $(make --version | head -n1)"
    echo "  CC         $CC"
  '';
}
