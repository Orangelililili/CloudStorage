#!/usr/bin/env bash
# 在仓库根目录执行 docker compose / docker-compose（自动选用本机可用的那种）
# 例：./scripts/docker-stack.sh down -v
#     ./scripts/docker-stack.sh up -d
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

_run() {
  if docker compose version >/dev/null 2>&1; then
    docker compose "$@"
    return
  fi
  if command -v docker-compose >/dev/null 2>&1; then
    docker-compose "$@"
    return
  fi
  echo "未找到 Docker Compose。请安装其一：" >&2
  echo "  sudo apt install -y docker-compose-plugin   # 推荐，使用: docker compose ..." >&2
  echo "  sudo apt install -y docker-compose          # 旧包，使用: docker-compose ..." >&2
  exit 1
}

_run "$@"
