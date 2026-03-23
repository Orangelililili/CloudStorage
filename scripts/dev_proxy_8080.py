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

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _read_body(self) -> bytes | None:
        n = self.headers.get("Content-Length")
        if not n:
            return None
        try:
            length = int(n)
        except ValueError:
            return None
        if length <= 0:
            return b""
        return self.rfile.read(length)

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
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def _spa_index(self) -> None:
        self._serve_file(self.front_root / "index.html")

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

    httpd = ThreadingHTTPServer((args.listen, args.port), ProxyHandler)
    print(
        "开发入口 http://%s:%s/  (API → http://%s:%s)"
        % (args.listen, args.port, host, port),
        file=sys.stderr,
    )
    httpd.serve_forever()


if __name__ == "__main__":
    main()
