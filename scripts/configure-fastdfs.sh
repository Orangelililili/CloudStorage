#!/usr/bin/env bash
# 一键同步 FastDFS 与项目上传配置，避免迁移服务器后手工重复修改
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

TRACKER_HOST="${TRACKER_HOST:-}"
TRACKER_PORT="${TRACKER_PORT:-22122}"
STORAGE_WEB_IP="${STORAGE_WEB_IP:-}"
STORAGE_WEB_PORT="${STORAGE_WEB_PORT:-8080}"
DFS_CLIENT_CONF="${DFS_CLIENT_CONF:-/etc/fdfs/client.conf}"
TC_SERVER_CONF="${TC_SERVER_CONF:-$ROOT/tc-src/tc_http_server.conf}"
CAN_USE_SUDO=0

usage() {
  cat <<'EOF'
用法：
  ./scripts/configure-fastdfs.sh [选项]

选项（也可通过同名环境变量传入）：
  --tracker-host <ip/host>      FastDFS tracker 地址（默认：本机主 IP）
  --tracker-port <port>         FastDFS tracker 端口（默认：22122）
  --storage-web-ip <ip/host>    生成文件 URL 的 host（默认：本机主 IP）
  --storage-web-port <port>     生成文件 URL 的端口（默认：8080，适配开发入口 run-dev-8080）
  --dfs-client-conf <path>      tc_http_server.conf 中 dfs_path_client（默认：/etc/fdfs/client.conf）
  --tc-server-conf <path>       后端配置路径（默认：tc-src/tc_http_server.conf）
  --help                        查看帮助

示例：
  ./scripts/configure-fastdfs.sh
  ./scripts/configure-fastdfs.sh --tracker-host 10.0.0.12 --storage-web-ip cdn.example.com
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tracker-host)
      TRACKER_HOST="$2"; shift 2 ;;
    --tracker-port)
      TRACKER_PORT="$2"; shift 2 ;;
    --storage-web-ip)
      STORAGE_WEB_IP="$2"; shift 2 ;;
    --storage-web-port)
      STORAGE_WEB_PORT="$2"; shift 2 ;;
    --dfs-client-conf)
      DFS_CLIENT_CONF="$2"; shift 2 ;;
    --tc-server-conf)
      TC_SERVER_CONF="$2"; shift 2 ;;
    --help|-h)
      usage; exit 0 ;;
    *)
      echo "未知参数: $1" >&2
      usage
      exit 1 ;;
  esac
done

detect_primary_ip() {
  local ip
  ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
  if [[ -z "$ip" ]]; then
    ip="127.0.0.1"
  fi
  printf '%s' "$ip"
}

TRACKER_HOST="${TRACKER_HOST:-$(detect_primary_ip)}"
STORAGE_WEB_IP="${STORAGE_WEB_IP:-$(detect_primary_ip)}"
TRACKER_ADDR="${TRACKER_HOST}:${TRACKER_PORT}"

if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
  CAN_USE_SUDO=1
fi

rewrite_tracker_server() {
  local file="$1"
  local temp_file
  temp_file="$(mktemp)"
  awk -v tracker_addr="$TRACKER_ADDR" '
    /^[[:space:]]*#/ { print; next }
    /^[[:space:]]*tracker_server[[:space:]]*=/ {
      print "tracker_server = " tracker_addr
      next
    }
    { print }
  ' "$file" >"$temp_file"
  cat "$temp_file" >"$file"
  rm -f "$temp_file"
}

rewrite_kv_line() {
  local file="$1"
  local key="$2"
  local value="$3"
  local temp_file
  temp_file="$(mktemp)"
  awk -v k="$key" -v v="$value" '
    /^[[:space:]]*#/ { print; next }
    $0 ~ "^[[:space:]]*" k "[[:space:]]*=" {
      print k "=" v
      next
    }
    { print }
  ' "$file" >"$temp_file"
  cat "$temp_file" >"$file"
  rm -f "$temp_file"
}

rewrite_if_exists() {
  local file="$1"
  if [[ -f "$file" ]]; then
    if [[ -w "$file" ]]; then
      rewrite_tracker_server "$file"
      echo "[OK] 已更新 $file"
    elif [[ "$CAN_USE_SUDO" -eq 1 ]]; then
      local temp_file
      temp_file="$(mktemp)"
      cp "$file" "$temp_file"
      rewrite_tracker_server "$temp_file"
      if sudo cp "$temp_file" "$file"; then
        echo "[OK] 已更新 $file (sudo)"
      else
        echo "[WARN] 无法通过 sudo 更新 $file（请手动修改 tracker_server=$TRACKER_ADDR）"
      fi
      rm -f "$temp_file"
    else
      echo "[WARN] 无权限更新 $file（请手动修改 tracker_server=$TRACKER_ADDR）"
    fi
  fi
}

echo "== FastDFS 配置同步 =="
echo "tracker_server: $TRACKER_ADDR"
echo "storage_web:    ${STORAGE_WEB_IP}:${STORAGE_WEB_PORT}"
echo "dfs_client:     $DFS_CLIENT_CONF"
echo "tc_server_conf: $TC_SERVER_CONF"

rewrite_if_exists "/etc/fdfs/client.conf"
rewrite_if_exists "/etc/fdfs/storage.conf"
rewrite_if_exists "/etc/fdfs/mod_fastdfs.conf"
rewrite_if_exists "/etc/fdfs/downloaded.conf"

if [[ ! -f "$TC_SERVER_CONF" ]]; then
  echo "[ERR] 未找到后端配置: $TC_SERVER_CONF" >&2
  exit 1
fi

rewrite_kv_line "$TC_SERVER_CONF" "dfs_path_client" "$DFS_CLIENT_CONF"
rewrite_kv_line "$TC_SERVER_CONF" "storage_web_server_ip" "$STORAGE_WEB_IP"
rewrite_kv_line "$TC_SERVER_CONF" "storage_web_server_port" "$STORAGE_WEB_PORT"
echo "[OK] 已更新 $TC_SERVER_CONF"

echo
echo "== 连通性自检 =="
if timeout 3 bash -lc "echo > /dev/tcp/$TRACKER_HOST/$TRACKER_PORT" 2>/dev/null; then
  echo "[OK] tracker TCP 可达: $TRACKER_ADDR"
else
  echo "[WARN] tracker TCP 不可达: $TRACKER_ADDR"
  echo "       请确认 fdfs_trackerd / fdfs_storaged 已启动，或地址端口是否正确"
fi

if command -v fdfs_upload_file >/dev/null 2>&1; then
  tmp_file="$(mktemp)"
  echo "fastdfs-health-check" >"$tmp_file"
  if timeout 8 fdfs_upload_file "$DFS_CLIENT_CONF" "$tmp_file" >/tmp/fdfs_upload_check.out 2>/tmp/fdfs_upload_check.err; then
    echo "[OK] fdfs_upload_file 验证通过"
    echo "     file_id: $(tr -d '\r' </tmp/fdfs_upload_check.out | awk 'NR==1{print $0}')"
  else
    echo "[WARN] fdfs_upload_file 验证失败（详情见下）"
    if [[ -s /tmp/fdfs_upload_check.err ]]; then
      sed 's/^/       /' /tmp/fdfs_upload_check.err
    fi
  fi
  rm -f "$tmp_file"
else
  echo "[WARN] 未安装 fdfs_upload_file，跳过上传验证"
fi

echo
echo "下一步建议："
echo "1) sudo systemctl restart fdfs_trackerd fdfs_storaged  (若本机部署 FastDFS)"
echo "2) 重启后端：./scripts/run-dev-8080.sh"
echo "3) 浏览器重新测试上传"
