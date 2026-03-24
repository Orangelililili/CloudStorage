#!/usr/bin/env bash
# 上传压测：8081 直连 +（可选）本机 Nginx :80
# 依赖：tc_http_server 已监听 8081；FastDFS/MySQL/Redis 可用；Nginx 需已按仓库 nginx.conf 配置 upload 模块
#
# 用法（仓库根目录）：
#   ./wrk/scripts/bench-upload.sh
# 环境变量：
#   THREADS=4 CONN=200 DURATION=30s RANDOM_BYTES=65536 SKIP_NGINX=1

set -euo pipefail

ROOT_REPO="$(cd "$(dirname "$0")/../.." && pwd)"
WRK_BIN="$ROOT_REPO/wrk/wrk"
LUA="$ROOT_REPO/wrk/scripts/upload_multipart.lua"
PNG="$ROOT_REPO/display/页面.png"
TMPDIR="$ROOT_REPO/wrk/tmp"
RAND_FILE="$TMPDIR/wrk_random.txt"

THREADS="${THREADS:-4}"
CONN="${CONN:-200}"
DURATION="${DURATION:-30s}"
RANDOM_BYTES="${RANDOM_BYTES:-65536}"
USER="${BENCH_USER:-bench_wrk}"

mkdir -p "$TMPDIR"

ensure_wrk() {
  if [[ -x "$WRK_BIN" ]]; then
    return 0
  fi
  echo "正在编译 wrk（首次较慢）..."
  make -C "$ROOT_REPO/wrk" -j"$(nproc 2>/dev/null || echo 4)"
}

gen_random_txt() {
  head -c "$RANDOM_BYTES" /dev/urandom | base64 >"$RAND_FILE"
}

run_wrk() {
  local title=$1
  local base_url=$2
  local file=$3
  local md5=$4
  local form_name=$5
  echo ""
  echo "========== $title =========="
  echo "url=$base_url  file=$file  threads=$THREADS  connections=$CONN  duration=$DURATION"
  "$WRK_BIN" -t"$THREADS" -c"$CONN" -d"$DURATION" --latency -s "$LUA" "$base_url" -- \
    "$file" "$USER" "$md5" "$form_name"
}

port_open() {
  local host=$1
  local port=$2
  (echo >/dev/tcp/"$host"/"$port") 2>/dev/null
}

ensure_wrk

if [[ ! -f "$PNG" ]]; then
  echo "未找到 $PNG，请确认 display/页面.png 存在"
  exit 1
fi

gen_random_txt
md5_rand=$(md5sum "$RAND_FILE" | awk '{print $1}')
md5_png=$(md5sum "$PNG" | awk '{print $1}')

if ! port_open 127.0.0.1 8081; then
  echo "127.0.0.1:8081 无监听，请先启动 tc_http_server（例如 ./scripts/run-dev-8080.sh 仅起后端或整栈）"
  exit 1
fi

run_wrk "8081 + 随机 txt (${RANDOM_BYTES}B→base64)" "http://127.0.0.1:8081" "$RAND_FILE" "$md5_rand" "wrk_random.txt"
run_wrk "8081 + display/页面.png" "http://127.0.0.1:8081" "$PNG" "$md5_png" "页面.png"

if [[ "${SKIP_NGINX:-0}" == "1" ]]; then
  echo ""
  echo "已设置 SKIP_NGINX=1，跳过 Nginx 压测"
  exit 0
fi

if port_open 127.0.0.1 80; then
  run_wrk "Nginx :80 + 随机 txt" "http://127.0.0.1" "$RAND_FILE" "$md5_rand" "wrk_random.txt"
  run_wrk "Nginx :80 + 页面.png" "http://127.0.0.1" "$PNG" "$md5_png" "页面.png"
else
  echo ""
  echo "127.0.0.1:80 无监听，已跳过 Nginx 压测。需要时启动 Nginx（参考仓库 nginx.conf）后重跑，或 export SKIP_NGINX=1 静默跳过。"
fi
