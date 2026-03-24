#!/usr/bin/env bash
# 一键启动 shorturl gRPC 服务（50051）：
# - 检查依赖（go、mysql/容器）
# - 导入 shorturl.sql
# - 编译 short-server
# - 启动并探活 50051
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHORT_DIR="$ROOT/shorturl/shorturl-server"
SQL_FILE="$SHORT_DIR/sql/shorturl.sql"
CONF_FILE="$SHORT_DIR/dev.config.yaml"
BIN_FILE="$SHORT_DIR/short-server"
LOG_DIR="$ROOT/shorturl/logs"
LOG_FILE="$LOG_DIR/shorturl-server.log"

MYSQL_HOST="${MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_USER="${MYSQL_USER:-root}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-123456}"
MYSQL_CONTAINER="${MYSQL_CONTAINER:-tuchuang-mysql}"

_die() {
  echo "错误: $*" >&2
  exit 1
}

_has_cmd() {
  command -v "$1" >/dev/null 2>&1
}

_check_path() {
  [[ -d "$SHORT_DIR" ]] || _die "目录不存在: $SHORT_DIR"
  [[ -f "$SQL_FILE" ]] || _die "SQL 文件不存在: $SQL_FILE"
  [[ -f "$CONF_FILE" ]] || _die "配置文件不存在: $CONF_FILE"
}

_check_go() {
  if ! _has_cmd go; then
    _die "未检测到 go。请先安装（例如: sudo apt install -y golang-go）"
  fi
}

_prepare_go_proxy() {
  # 默认使用国内代理，避免 proxy.golang.org 超时
  local current_proxy
  current_proxy="$(go env GOPROXY 2>/dev/null || true)"
  if [[ -z "${current_proxy}" || "${current_proxy}" == "https://proxy.golang.org,direct" ]]; then
    echo "设置 GOPROXY=https://goproxy.cn,direct"
    go env -w GOPROXY="https://goproxy.cn,direct"
  fi
  go env -w GO111MODULE=on >/dev/null 2>&1 || true
}

_check_mysql_tcp() {
  python3 - <<PY
import socket, sys
s = socket.socket()
s.settimeout(1.2)
try:
    s.connect(("${MYSQL_HOST}", int("${MYSQL_PORT}")))
except Exception:
    sys.exit(1)
finally:
    s.close()
sys.exit(0)
PY
}

_import_sql_via_local_mysql() {
  mysql -h"${MYSQL_HOST}" -P"${MYSQL_PORT}" -u"${MYSQL_USER}" -p"${MYSQL_PASSWORD}" \
    < "${SQL_FILE}"
}

_import_sql_via_docker_mysql() {
  _has_cmd docker || _die "本机无 mysql 客户端，且未检测到 docker 命令"
  if ! docker ps --format '{{.Names}}' | grep -Fx "${MYSQL_CONTAINER}" >/dev/null 2>&1; then
    echo "未找到 MySQL 容器 ${MYSQL_CONTAINER}，尝试启动 docker compose（mysql/redis）"
    if [[ -x "${ROOT}/scripts/docker-stack.sh" ]]; then
      "${ROOT}/scripts/docker-stack.sh" up -d || true
    fi
  fi
  docker ps --format '{{.Names}}' | grep -Fx "${MYSQL_CONTAINER}" >/dev/null 2>&1 \
    || _die "未找到 MySQL 容器 ${MYSQL_CONTAINER}，请先启动数据库（例如: ./scripts/docker-stack.sh up -d）"
  docker exec -i "${MYSQL_CONTAINER}" mysql -u"${MYSQL_USER}" -p"${MYSQL_PASSWORD}" \
    < "${SQL_FILE}"
}

_init_db() {
  if _check_mysql_tcp; then
    if _has_cmd mysql; then
      echo "导入 shorturl.sql（本机 mysql 客户端）"
      _import_sql_via_local_mysql
      return
    fi
    echo "未检测到 mysql 客户端，尝试通过容器导入 shorturl.sql"
    _import_sql_via_docker_mysql
    return
  fi
  _die "无法连接 MySQL ${MYSQL_HOST}:${MYSQL_PORT}，请先启动数据库"
}

_build_server() {
  echo "编译 short-server"
  (
    cd "${SHORT_DIR}"
    local try
    for try in 1 2 3; do
      if go mod tidy; then
        break
      fi
      if [[ "$try" -eq 3 ]]; then
        _die "go mod tidy 失败，请检查网络或代理（当前 GOPROXY: $(go env GOPROXY)）"
      fi
      echo "go mod tidy 失败，2 秒后重试（${try}/3）"
      sleep 2
    done
    go build -o short-server ./shorturl-server
  )
  [[ -x "${BIN_FILE}" ]] || _die "编译失败，未生成可执行文件: ${BIN_FILE}"
}

_stop_old_server() {
  # 仅尝试结束占用 50051 的旧 short-server 进程，避免重复启动。
  local line pid cmd
  while IFS= read -r line; do
    [[ "$line" == *":50051"* ]] || continue
    [[ "$line" == *"pid="* ]] || continue
    if [[ "$line" =~ pid=([0-9]+) ]]; then
      pid="${BASH_REMATCH[1]}"
      cmd=$(tr '\0' ' ' <"/proc/${pid}/cmdline" 2>/dev/null || true)
      if [[ "$cmd" == *"short-server"* ]]; then
        echo "结束旧 short-server (pid=${pid})"
        kill -TERM "${pid}" 2>/dev/null || true
      fi
    fi
  done < <(ss -lntp 2>/dev/null || true)
  sleep 0.4
}

_start_server() {
  mkdir -p "${LOG_DIR}"
  echo "启动 short-server（日志: ${LOG_FILE}）"
  (
    cd "${SHORT_DIR}"
    nohup "${BIN_FILE}" --config="${CONF_FILE}" >> "${LOG_FILE}" 2>&1 &
  )
}

_wait_port() {
  local port="$1" max="${2:-50}"
  for _ in $(seq 1 "$max"); do
    if python3 - <<PY
import socket, sys
s = socket.socket()
s.settimeout(0.8)
try:
    s.connect(("127.0.0.1", ${port}))
except Exception:
    sys.exit(1)
finally:
    s.close()
sys.exit(0)
PY
    then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

# ── shorturl-proxy（HTTP /p/:key 302 跳转，监听 8082） ──

PROXY_DIR="$ROOT/shorturl/shorturl-proxy"
PROXY_BIN="$PROXY_DIR/shorturl-proxy"
PROXY_CONF="$PROXY_DIR/dev.config.yaml"
PROXY_LOG="$LOG_DIR/shorturl-proxy.log"

_build_proxy() {
  echo "编译 shorturl-proxy"
  (
    cd "${PROXY_DIR}"
    local try
    for try in 1 2 3; do
      if go mod tidy; then
        break
      fi
      if [[ "$try" -eq 3 ]]; then
        _die "go mod tidy (proxy) 失败，请检查网络或代理（当前 GOPROXY: $(go env GOPROXY)）"
      fi
      echo "go mod tidy (proxy) 失败，2 秒后重试（${try}/3）"
      sleep 2
    done
    go build -o shorturl-proxy .
  )
  [[ -x "${PROXY_BIN}" ]] || _die "编译失败，未生成: ${PROXY_BIN}"
}

_stop_old_proxy() {
  local line pid cmd
  while IFS= read -r line; do
    [[ "$line" == *":8082"* ]] || continue
    [[ "$line" == *"pid="* ]] || continue
    if [[ "$line" =~ pid=([0-9]+) ]]; then
      pid="${BASH_REMATCH[1]}"
      cmd=$(tr '\0' ' ' <"/proc/${pid}/cmdline" 2>/dev/null || true)
      if [[ "$cmd" == *"shorturl-proxy"* ]]; then
        echo "结束旧 shorturl-proxy (pid=${pid})"
        kill -TERM "${pid}" 2>/dev/null || true
      fi
    fi
  done < <(ss -lntp 2>/dev/null || true)
  sleep 0.3
}

_start_proxy() {
  mkdir -p "${LOG_DIR}"
  echo "启动 shorturl-proxy（日志: ${PROXY_LOG}）"
  (
    cd "${PROXY_DIR}"
    nohup "${PROXY_BIN}" --config="${PROXY_CONF}" >> "${PROXY_LOG}" 2>&1 &
  )
}

main() {
  _check_path
  _check_go
  _prepare_go_proxy
  _init_db

  # shorturl-server (gRPC 50051)
  _build_server
  _stop_old_server
  _start_server
  if ! _wait_port 50051; then
    _die "short-server 未成功监听 50051，请查看日志: ${LOG_FILE}"
  fi
  echo "short-server 已就绪: 127.0.0.1:50051"

  # shorturl-proxy (HTTP 8082)
  _build_proxy
  _stop_old_proxy
  _start_proxy
  if ! _wait_port 8082; then
    _die "shorturl-proxy 未成功监听 8082，请查看日志: ${PROXY_LOG}"
  fi
  echo "shorturl-proxy 已就绪: 127.0.0.1:8082（/p/:key 短链 302 跳转）"
}

main "$@"
