#!/usr/bin/env bash
# 一键：后台启动 tc_http_server(8081) + 前台 8080 静态与 /api 反代（浏览器打开 http://127.0.0.1:8080/）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! (echo >/dev/tcp/127.0.0.1/6379) 2>/dev/null; then
  echo "本机 6379 无 Redis，后端无法启动。任选其一："
  echo "  A) Docker（仓库根目录；无「docker compose」时用带连字符的 docker-compose）:"
  echo "       cd $ROOT && ./scripts/docker-stack.sh pull && ./scripts/docker-stack.sh up -d"
  echo "  B) 本机服务（无需 Docker）:"
  echo "       sudo apt install -y redis-server default-mysql-server"
  echo "       sudo systemctl start redis-server mysql  # 或 mariadb"
  echo "       mysql -uroot -p < $ROOT/tuchuang.sql"
  exit 1
fi

BUILD="$ROOT/tc-src/build"
CONF="$ROOT/tc-src/tc_http_server.conf"
if [[ ! -x "$BUILD/tc_http_server" ]]; then
  echo "未找到 $BUILD/tc_http_server。编译示例："
  echo "  cd $ROOT/tc-src/build && cmake .. && make && cp -f ../tc_http_server.conf ."
  echo "启动反代请在仓库根目录执行（勿在 tc-src/build 下执行 ./scripts/...）："
  echo "  cd $ROOT && ./scripts/run-dev-8080.sh"
  exit 1
fi

cp -f "$CONF" "$BUILD/"
export LD_LIBRARY_PATH="${ROOT}/.deps/lib/mariadb:${LD_LIBRARY_PATH:-}"

# 开发时常见：上次 tc_http_server 仍占用 8081 → bind err 98。
# 部分环境 ss -lntp 不显示 pid（权限策略），仅靠 pid 杀进程会失效，故对「8081 有监听」再尝试 fuser。
_free_port_8081_for_dev() {
  local line pid exe
  if ! (echo >/dev/tcp/127.0.0.1/8081) 2>/dev/null; then
    return 0
  fi
  while IFS= read -r line; do
    [[ "$line" == *8081* ]] || continue
    [[ "$line" == *pid=* ]] || continue
    if [[ "$line" =~ pid=([0-9]+) ]]; then
      pid="${BASH_REMATCH[1]}"
      exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
      if [[ "$(basename "$exe" 2>/dev/null)" == "tc_http_server" ]]; then
        echo "结束占用 8081 的旧 tc_http_server (pid=$pid)"
        kill -TERM "$pid" 2>/dev/null || true
      fi
    fi
  done < <(ss -lntp 2>/dev/null || true)
  sleep 0.4
  if ! (echo >/dev/tcp/127.0.0.1/8081) 2>/dev/null; then
    return 0
  fi
  if command -v fuser >/dev/null 2>&1; then
    echo "8081 仍被占用，开发脚本将执行: fuser -k 8081/tcp"
    fuser -k 8081/tcp 2>/dev/null || true
    sleep 0.5
  else
    echo "8081 已被占用且未安装 fuser（如: sudo apt install psmisc）。请手动："
    echo "  ss -lntp | grep 8081"
    echo "  kill <pid>"
    exit 1
  fi
  if (echo >/dev/tcp/127.0.0.1/8081) 2>/dev/null; then
    echo "无法释放 8081，请检查占用进程："
    ss -lntp 2>/dev/null | grep 8081 || true
    exit 1
  fi
}
_free_port_8081_for_dev

cd "$BUILD"
./tc_http_server &
SRV_PID=$!
trap 'kill "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null || true' EXIT

sleep 0.2
if ! kill -0 "$SRV_PID" 2>/dev/null; then
  echo "tc_http_server 启动后立即退出，常见原因是 8081 已被占用（bind err 98）。可先："
  echo "  ss -lntp | grep 8081"
  wait "$SRV_PID" 2>/dev/null || true
  exit 1
fi

for _ in $(seq 1 50); do
  if (echo >/dev/tcp/127.0.0.1/8081) 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if ! (echo >/dev/tcp/127.0.0.1/8081) 2>/dev/null; then
  echo "等待 8081 超时，tc_http_server 可能启动失败，请查看上方日志"
  exit 1
fi

# 上次未退出的 dev_proxy_8080.py 会占 8080 → Python OSError: Address already in use
_free_port_8080_for_dev() {
  local line pid exe cmd
  if ! (echo >/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    return 0
  fi
  while IFS= read -r line; do
    [[ "$line" == *8080* ]] || continue
    [[ "$line" == *pid=* ]] || continue
    if [[ "$line" =~ pid=([0-9]+) ]]; then
      pid="${BASH_REMATCH[1]}"
      cmd=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)
      if [[ "$cmd" == *dev_proxy_8080.py* ]]; then
        echo "结束占用 8080 的旧 dev_proxy (pid=$pid)"
        kill -TERM "$pid" 2>/dev/null || true
      fi
    fi
  done < <(ss -lntp 2>/dev/null || true)
  sleep 0.4
  if ! (echo >/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    return 0
  fi
  if command -v fuser >/dev/null 2>&1; then
    echo "8080 仍被占用，开发脚本将执行: fuser -k 8080/tcp"
    fuser -k 8080/tcp 2>/dev/null || true
    sleep 0.5
  else
    echo "8080 已被占用。请结束占用进程后重试："
    echo "  ss -lntp | grep 8080"
    exit 1
  fi
  if (echo >/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    echo "无法释放 8080："
    ss -lntp 2>/dev/null | grep 8080 || true
    exit 1
  fi
}
_free_port_8080_for_dev

# 让代理同时监听本机 LAN IP（FastDFS 生成的 url 默认带宿主机 IP）
exec python3 "$ROOT/scripts/dev_proxy_8080.py" --listen 0.0.0.0 "$@"
