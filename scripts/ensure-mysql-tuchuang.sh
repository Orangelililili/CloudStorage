#!/usr/bin/env bash
# 若 MySQL 数据卷是旧的、里没有库「tuchuang」，执行本脚本从仓库根目录导入 tuchuang.sql。
# 需已启动容器: ./scripts/docker-stack.sh up -d
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SQL="$ROOT/tuchuang.sql"
if [[ ! -f "$SQL" ]]; then
  echo "缺少 $SQL" >&2
  exit 1
fi
if ! docker ps --format '{{.Names}}' | grep -qx 'tuchuang-mysql'; then
  echo "未运行容器 tuchuang-mysql，请先: cd $ROOT && ./scripts/docker-stack.sh up -d" >&2
  exit 1
fi
docker exec -i tuchuang-mysql mysql -uroot -p123456 <"$SQL"
echo "已导入 $SQL → 库 tuchuang"
