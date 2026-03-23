#!/usr/bin/env bash
# 在无 libmysqlclient-dev 的机器上，将 MariaDB Connector/C 安装到仓库根目录 .deps（与 tc-src/CMakeLists.txt 约定一致）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/.deps/src"
INSTALL_PREFIX="$ROOT/.deps"
MARIADB_VER="3.3.10"

mkdir -p "$SRC"
cd "$SRC"
if [[ ! -d "mariadb-connector-c-${MARIADB_VER}" ]]; then
  curl -sL "https://github.com/mariadb-corporation/mariadb-connector-c/archive/refs/tags/v${MARIADB_VER}.tar.gz" -o mariadb-c.tgz
  tar xzf mariadb-c.tgz
fi
cd "mariadb-connector-c-${MARIADB_VER}"
cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_EXTERNAL_ZLIB=On
cmake --build build -j"$(nproc 2>/dev/null || echo 4)"
cmake --install build
echo "MariaDB C API 已安装到: $INSTALL_PREFIX"
