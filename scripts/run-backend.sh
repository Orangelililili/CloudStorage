#!/usr/bin/env bash
# 启动 MySQL/Redis 容器后，在 tc-src/build 运行 tc_http_server
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! (echo >/dev/tcp/127.0.0.1/6379) 2>/dev/null; then
  echo "本机 6379 无 Redis。任选其一："
  echo "  cd $ROOT && docker-compose pull && docker-compose up -d"
  echo "  或: sudo apt install -y redis-server && sudo systemctl start redis-server"
  exit 1
fi

BUILD="$ROOT/tc-src/build"
CONF="$ROOT/tc-src/tc_http_server.conf"
if [[ ! -x "$BUILD/tc_http_server" ]]; then
  echo "未找到 $BUILD/tc_http_server，请先编译 tc-src"
  exit 1
fi
cp -f "$CONF" "$BUILD/"
cd "$BUILD"
export LD_LIBRARY_PATH="${ROOT}/.deps/lib/mariadb:${LD_LIBRARY_PATH:-}"
exec ./tc_http_server
