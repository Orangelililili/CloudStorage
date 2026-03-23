#!/usr/bin/env python3
"""
开发用入口：监听 8080
  - 静态资源 + React Router：来自 tc-front
  - /api/*：反向代理到 tc_http_server（默认 127.0.0.1:8081）
与打包前端一致：浏览器访问 http://127.0.0.1:8080/ 即可。
"""
from __future__ import annotations

import argparse
import http.client
import mimetypes
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def _hop_by_hop(name: str) -> bool:
    return name.lower() in {
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "te",
        "trailers",
        "transfer-encoding",
        "upgrade",
        "host",
    }


class ProxyHandler(BaseHTTPRequestHandler):
    front_root: Path
    backend_host: str
    backend_port: int
    fdfs_data_root: Path | None

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _read_body(self) -> bytes | None:
        n = self.headers.get("Content-Length")
        if not n:
            # 浏览器（fetch/xhr）上传 multipart 时经常不设置 Content-Length，
            # 而是使用 Transfer-Encoding: chunked。此时不能直接返回 None，
            # 否则后端拿不到完整 body。
            te = self.headers.get("Transfer-Encoding", "")
            if "chunked" in te.lower():
                return self._read_chunked_body()
            return b""
        try:
            length = int(n)
        except ValueError:
            # 兜底：当 Content-Length 无法解析时尝试按 chunked 处理
            te = self.headers.get("Transfer-Encoding", "")
            if "chunked" in te.lower():
                return self._read_chunked_body()
            return b""
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def _read_chunked_body(self) -> bytes:
        """
        读取并拼接 chunked 编码的 request body。

        说明：这是仅用于本地开发的轻量代理实现，优先保证上传功能可用。
        """
        chunks: list[bytes] = []
        while True:
            size_line = self.rfile.readline()
            if not size_line:
                break
            size_str = size_line.strip().split(b";", 1)[0]
            try:
                size = int(size_str, 16)
            except ValueError:
                # 非法 chunk size，按已有数据返回（上层会导致解析失败）
                break
            if size == 0:
                # chunk 最后会有 CRLF；随后是可选的 trailer，直到空行结束
                # 先读掉该 chunk 的 CRLF
                _ = self.rfile.read(2)
                while True:
                    trailer = self.rfile.readline()
                    if trailer in (b"\r\n", b""):
                        break
                break
            data = self.rfile.read(size)
            chunks.append(data)
            # chunk data 后面紧跟 CRLF
            _ = self.rfile.read(2)
        return b"".join(chunks)

    def _proxy(self) -> None:
        body = None
        if self.command in ("POST", "PUT", "PATCH"):
            body = self._read_body()
        headers = {
            k: v
            for k, v in self.headers.items()
            if not _hop_by_hop(k)
        }
        try:
            conn = http.client.HTTPConnection(
                self.backend_host, self.backend_port, timeout=120
            )
            conn.request(self.command, self.path, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            self.send_response(resp.status)
            for h, v in resp.getheaders():
                if not _hop_by_hop(h):
                    self.send_header(h, v)
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(data)
        except OSError as e:
            self.send_error(502, f"backend unreachable: {e}")

    def _serve_file(self, path: Path) -> None:
        if not path.is_file():
            self.send_error(404)
            return
        ctype, _ = mimetypes.guess_type(str(path))
        if not ctype:
            ctype = "application/octet-stream"
        data = path.read_bytes()
        self._send_file_headers(
            ctype=ctype,
            size=len(data),
            as_attachment=False,
            attachment_filename=None,
            add_cors=False,
        )
        self.wfile.write(data)

    def _send_fastdfs_file_headers(self, path: Path) -> None:
        ctype, _ = mimetypes.guess_type(str(path))
        if not ctype:
            ctype = "application/octet-stream"
        size = path.stat().st_size
        attachment_filename = path.name
        # 给 FastDFS 直链一个“可下载”的语义，前端下载按钮在跨域场景下更容易工作。
        self._send_file_headers(
            ctype=ctype,
            size=size,
            as_attachment=True,
            attachment_filename=attachment_filename,
            add_cors=True,
        )

    def _send_file_headers(
        self,
        *,
        ctype: str,
        size: int,
        as_attachment: bool,
        attachment_filename: str | None,
        add_cors: bool,
    ) -> None:
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.send_header("Connection", "close")
        if as_attachment and attachment_filename:
            # 对浏览器“下载”按钮更友好：直接让资源以附件形式返回
            self.send_header(
                "Content-Disposition", f'attachment; filename="{attachment_filename}"'
            )
        if add_cors:
            # 仅用于开发态：前端若通过 fetch(blob) 下载，跨域需要至少允许读取响应
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Expose-Headers", "Content-Disposition")
            self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.end_headers()

    def _spa_index(self) -> None:
        self._serve_file(self.front_root / "index.html")

    def _serve_fastdfs_file(self, path: str) -> bool:
        """
        开发态兼容 FastDFS 文件直链：
        /group1/M00/xx/yy/file.png -> <fdfs_data_root>/xx/yy/file.png
        """
        root = self.fdfs_data_root
        if root is None:
            return False
        prefix = "/group1/M00/"
        if not path.startswith(prefix):
            return False
        rel = path[len(prefix) :].lstrip("/")
        if not rel:
            return False
        p = (root / rel).resolve()
        try:
            p.relative_to(root.resolve())
        except ValueError:
            return False
        if not p.is_file():
            return False
        # FastDFS 文件使用下载友好的响应头
        self._send_fastdfs_file_headers(p)
        data = p.read_bytes()
        self.wfile.write(data)
        return True

    def do_HEAD(self) -> None:
        if self.path.startswith("/api"):
            # 后端不一定支持 HEAD，直接返回 405 更明确
            self.send_error(405)
            return
        u = urllib.parse.urlparse(self.path)
        path = urllib.parse.unquote(u.path)
        root = self.fdfs_data_root
        prefix = "/group1/M00/"
        if root is not None and path.startswith(prefix):
            rel = path[len(prefix) :].lstrip("/")
            if rel:
                p = (root / rel).resolve()
                try:
                    p.relative_to(root.resolve())
                except ValueError:
                    self.send_error(404)
                    return
                if p.is_file():
                    self._send_fastdfs_file_headers(p)
                    return
        self.send_error(404)

    def _safe_file_under_front(self, rel: str) -> Path | None:
        if not rel or ".." in rel.split("/"):
            return None
        p = (self.front_root / rel).resolve()
        try:
            p.relative_to(self.front_root.resolve())
        except ValueError:
            return None
        return p if p.is_file() else None

    def do_GET(self) -> None:
        if self.path.startswith("/api"):
            self._proxy()
            return
        u = urllib.parse.urlparse(self.path)
        path = urllib.parse.unquote(u.path)
        if self._serve_fastdfs_file(path):
            return
        if path in ("", "/"):
            self._spa_index()
            return
        rel = path.lstrip("/")
        hit = self._safe_file_under_front(rel)
        if hit:
            self._serve_file(hit)
            return
        self._spa_index()

    def do_POST(self) -> None:
        base = self.path.split("?", 1)[0]
        if base.startswith("/api"):
            self._proxy()
            return
        self.send_error(404)

    def do_OPTIONS(self) -> None:
        if self.path.startswith("/api"):
            self._proxy()
            return
        self.send_response(204)
        self.end_headers()


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--backend", default="127.0.0.1:8081")
    ap.add_argument(
        "--front",
        type=Path,
        default=root / "tc-front",
        help="tc-front 构建产物目录",
    )
    ap.add_argument(
        "--fdfs-data-root",
        type=Path,
        default=Path("/home/fastdfs/storage/data"),
        help="FastDFS storage data 根目录（用于开发态直链 /group1/M00/...）",
    )
    args = ap.parse_args()
    front = args.front.resolve()
    if not front.is_dir():
        print("前端目录不存在:", front, file=sys.stderr)
        sys.exit(1)
    host, _, port_s = args.backend.partition(":")
    port = int(port_s or "8081")

    ProxyHandler.front_root = front
    ProxyHandler.backend_host = host
    ProxyHandler.backend_port = port
    fdfs_data_root = args.fdfs_data_root.resolve()
    ProxyHandler.fdfs_data_root = fdfs_data_root if fdfs_data_root.is_dir() else None

    httpd = ThreadingHTTPServer((args.listen, args.port), ProxyHandler)
    print(
        "开发入口 http://%s:%s/  (API → http://%s:%s)"
        % (args.listen, args.port, host, port),
        file=sys.stderr,
    )
    if ProxyHandler.fdfs_data_root is not None:
        print(
            "FastDFS 直链映射 /group1/M00/* -> %s"
            % ProxyHandler.fdfs_data_root,
            file=sys.stderr,
        )
    else:
        print(
            "FastDFS 直链映射未启用（目录不存在）: %s" % fdfs_data_root,
            file=sys.stderr,
        )
    httpd.serve_forever()


if __name__ == "__main__":
    main()
