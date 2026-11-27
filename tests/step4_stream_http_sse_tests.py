#!/usr/bin/env python3
"""Step 4 end-to-end acceptance: std.stream (4A), std.url + std.http/HTTPS/cancel (4B), std.sse + SQLite Agent (4C)."""

from __future__ import annotations

import argparse
import datetime
import http.server
import ipaddress
import json
import os
import shutil
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import sqlite_package_tests as sqt  # noqa: E402


def clean_env(sdk: Path | None, extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    sys_root = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = str(Path(sys_root) / "System32")
    if sdk is not None:
        env["HUA_HOME"] = str(sdk)
    else:
        env.pop("HUA_HOME", None)
    env.pop("HUA_PATH", None)
    if extra:
        env.update(extra)
    return env


def run(argv: list[str | Path], cwd: Path, env: dict[str, str], timeout: int = 120) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(a) for a in argv],
        cwd=str(cwd),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )


def huab_header(path: Path) -> tuple[int, int]:
    magic, fmt, abi, size, crc, flags = struct.unpack("<8sIIQII", path.read_bytes()[:32])
    assert magic == b"HUAB\r\n\x1a\n" and flags == 0
    return fmt, abi


def generate_test_pki(dst: Path) -> tuple[Path, Path, Path, Path]:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

    now = datetime.datetime.now(datetime.timezone.utc)

    def make_ca(cn: str) -> tuple[rsa.RSAPrivateKey, x509.Certificate]:
        key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
        cert = (
            x509.CertificateBuilder()
            .subject_name(name)
            .issuer_name(name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .sign(key, hashes.SHA256())
        )
        return key, cert

    ca_key, ca_cert = make_ca("Hua Test Root CA")
    _, wrong_ca_cert = make_ca("Hua Wrong Root CA")

    srv_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    srv_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1")])
    srv_cert = (
        x509.CertificateBuilder()
        .subject_name(srv_name)
        .issuer_name(ca_cert.subject)
        .public_key(srv_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.SubjectAlternativeName(
                [x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")), x509.DNSName("localhost")]
            ),
            critical=False,
        )
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .sign(ca_key, hashes.SHA256())
    )

    ca_pem = dst / "ca.pem"
    wrong_ca_pem = dst / "wrong_ca.pem"
    srv_pem = dst / "server.pem"
    srv_key_pem = dst / "server.key"
    ca_pem.write_bytes(ca_cert.public_bytes(serialization.Encoding.PEM))
    wrong_ca_pem.write_bytes(wrong_ca_cert.public_bytes(serialization.Encoding.PEM))
    srv_pem.write_bytes(srv_cert.public_bytes(serialization.Encoding.PEM))
    srv_key_pem.write_bytes(
        srv_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    return ca_pem, wrong_ca_pem, srv_pem, srv_key_pem


class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass


def start_test_servers(srv_pem: Path, srv_key_pem: Path):
    state = {"other_port": 0, "cancel_closed": threading.Event()}

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args):
            pass

        def do_GET(self):
            if self.path == "/echo":
                body = json.dumps(
                    {
                        "auth": self.headers.get("Authorization", ""),
                        "x_test": self.headers.get("X-Test", ""),
                        "msg": "你好HTTP",
                    },
                    ensure_ascii=False,
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("X-Custom", "Value-42")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/status404":
                body = b"not here"
                self.send_response(404)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/redir-same":
                self.send_response(302)
                self.send_header("Location", "/echo")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path == "/redir-cross":
                self.send_response(302)
                self.send_header("Location", f"http://127.0.0.1:{state['other_port']}/echo")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path == "/redir-loop":
                self.send_response(302)
                self.send_header("Location", "/redir-loop")
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path == "/big":
                body = b"A" * 20000
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path == "/slow":
                time.sleep(2.5)
                body = b"late"
                try:
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except OSError:
                    pass
            elif self.path == "/sse":
                # Deliberately split multi-byte UTF-8 ("你好世界") and lines across 1-7 byte chunks
                payload = (
                    ": keepalive\n\n"
                    "id: 1\n"
                    "event: delta\n"
                    "data: {\"chunk\":\"你好\"}\n\n"
                    "id: 2\n"
                    "event: delta\n"
                    "data: line1\n"
                    "data: 世界\n\n"
                    "event: done\n"
                    "data: [DONE]\n\n"
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                pos = 0
                step = 1
                while pos < len(payload):
                    chunk = payload[pos : pos + step]
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    pos += step
                    step = (step % 7) + 1
                    time.sleep(0.002)
            elif self.path == "/sse-hang":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                first = b"id: 1\nevent: delta\ndata: first_msg\n\n"
                try:
                    self.wfile.write(f"{len(first):x}\r\n".encode("ascii") + first + b"\r\n")
                    self.wfile.flush()
                    # Hold connection open until client cancels/closes
                    for _ in range(100):
                        time.sleep(0.05)
                        # Send heartbeat comment to detect closed socket
                        hb = b": hb\n\n"
                        self.wfile.write(f"{len(hb):x}\r\n".encode("ascii") + hb + b"\r\n")
                        self.wfile.flush()
                except OSError:
                    state["cancel_closed"].set()
            else:
                self.send_response(400)
                self.end_headers()

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length)
            self.send_response(201)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

    srv_http = ThreadedHTTPServer(("127.0.0.1", 0), Handler)
    srv_other = ThreadedHTTPServer(("127.0.0.1", 0), Handler)
    srv_https = ThreadedHTTPServer(("127.0.0.1", 0), Handler)
    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(certfile=str(srv_pem), keyfile=str(srv_key_pem))
    srv_https.socket = ssl_ctx.wrap_socket(srv_https.socket, server_side=True)

    state["other_port"] = srv_other.server_address[1]
    for s in (srv_http, srv_other, srv_https):
        threading.Thread(target=s.serve_forever, daemon=True).start()
    return srv_http, srv_other, srv_https, state


CORE_4AB_HUA = r'''import std.buffer as buffer
import std.bytes as bytes
import std.error as error
import std.file as file
import std.http as http
import std.json as json
import std.os as os
import std.stream as stream
import std.test as test
import std.url as url

fn check_4a(tmp_file string) {
    # 1. Bytes reader: short reads, EOF=nil, embedded NUL + non-UTF-8
    let raw = unwrap(bytes.from_ints([0, 255, 65, 66, 0, 128]))
    let r = stream.from_bytes(raw)
    let c1 = unwrap(stream.read(r, 2))
    assert_true(is_some(c1) && bytes.len(unwrap(c1)) == 2)
    let c2 = unwrap(stream.read_exact(r, 4))
    assert_true(bytes.len(c2) == 4)
    let eof = unwrap(stream.read(r, 8))
    assert_true(is_none(eof))
    let ue = stream.read_exact(r, 1)
    assert_true(is_err(ue) && error.code(unwrap_err(ue)) == "unexpected_eof")
    unwrap(stream.close_reader(r))
    unwrap(stream.close_reader(r)) # idempotent
    let after_close = stream.read(r, 1)
    assert_true(is_err(after_close) && error.code(unwrap_err(after_close)) == "closed")

    # 2. Limit reader + read_all limit error
    let r2 = stream.from_bytes(bytes.from_text("0123456789"))
    let lim_r = unwrap(stream.limit(r2, 4))
    let got4 = unwrap(stream.read_all(lim_r, 10))
    assert_true(unwrap(bytes.to_text(got4)) == "0123")
    let r3 = stream.from_bytes(bytes.from_text("0123456789"))
    let over = stream.read_all(r3, 5)
    assert_true(is_err(over) && error.code(unwrap_err(over)) == "limit")

    # 3. File <-> Buffer stream.copy
    let wf = unwrap(file.open(tmp_file, "w"))
    let w = unwrap(stream.to_file(wf))
    unwrap(stream.write_all(w, raw))
    unwrap(stream.flush(w))
    unwrap(stream.close_writer(w))
    unwrap(file.close(wf))

    let rf = unwrap(file.open(tmp_file, "r"))
    let fr = unwrap(stream.from_file(rf))
    var dst_buf = buffer.new()
    let bw = stream.to_buffer(dst_buf)
    let copied = unwrap(stream.copy(bw, fr, 1024))
    assert_true(copied == 6)
    assert_true(unwrap(bytes.to_ints(buffer.bytes(dst_buf))) == [0, 255, 65, 66, 0, 128])
    unwrap(stream.close_reader(fr))
    unwrap(stream.close_writer(bw))
    unwrap(file.close(rf))
    print("4A=OK")
}

fn assert_true(cond bool) {
    test.assert(cond, "assertion failed")
}

fn main() {
    let argv = os.args()
    let http_base = argv[0]
    let https_base = argv[1]
    let ca_pem = argv[2]
    let wrong_ca = argv[3]
    let tmp_file = argv[4]

    check_4a(tmp_file)

    # 4B.1 std.url
    let u = unwrap(url.parse("https://Example.COM:8443/a/b?k=1#frag"))
    assert_true(unwrap(u["scheme"]) == "https")
    assert_true(unwrap(u["host"]) == "example.com")
    assert_true(unwrap(u["port"]) == "8443")
    assert_true(unwrap(u["path"]) == "/a/b")
    assert_true(url.encode("a b/中文") == "a%20b%2F%E4%B8%AD%E6%96%87")
    assert_true(unwrap(url.decode(url.encode("a b/中文"))) == "a b/中文")
    assert_true(url.build_query(map[string]string{"b": "2", "a": "x y"}) == "a=x%20y&b=2")

    # 4B.2 HTTP GET + headers + UTF-8 + POST binary + 404 is ok(Response)
    let r_get = unwrap(http.get(http_base + "/echo", map[string]string{"X-Test": "hello"}, 5000))
    assert_true(http.status(r_get) == 200)
    assert_true(unwrap(http.header(r_get, "x-custom")) == "Value-42")
    let j = unwrap(json.decode(unwrap(http.text(r_get))))
    assert_true(unwrap(json.as_string(unwrap(json.get(j, "x_test")))) == "hello")
    assert_true(unwrap(json.as_string(unwrap(json.get(j, "msg")))) == "你好HTTP")

    let bin = unwrap(bytes.from_ints([0, 1, 255, 0, 42]))
    let r_post = unwrap(http.post(http_base + "/post", map[string]string{}, bin, 5000))
    assert_true(http.status(r_post) == 201)
    assert_true(unwrap(bytes.to_ints(http.body(r_post))) == [0, 1, 255, 0, 42])

    let r_404 = unwrap(http.get(http_base + "/status404", map[string]string{}, 5000))
    assert_true(http.status(r_404) == 404)
    assert_true(unwrap(http.text(r_404)) == "not here")

    # 4B.3 Redirects: same-origin keeps auth, cross-origin strips auth, loop errors
    let r_same = unwrap(http.get(http_base + "/redir-same", map[string]string{"Authorization": "Bearer secret"}, 5000))
    let jsame = unwrap(json.decode(unwrap(http.text(r_same))))
    assert_true(unwrap(json.as_string(unwrap(json.get(jsame, "auth")))) == "Bearer secret")

    let r_cross = unwrap(http.get(http_base + "/redir-cross", map[string]string{"Authorization": "Bearer secret"}, 5000))
    let jcross = unwrap(json.decode(unwrap(http.text(r_cross))))
    assert_true(unwrap(json.as_string(unwrap(json.get(jcross, "auth")))) == "")

    let r_loop = http.request("GET", http_base + "/redir-loop", map[string]string{}, bytes.from_text(""), map[string]Json{"max_redirects": unwrap(json.from(2))})
    assert_true(is_err(r_loop) && error.code(unwrap_err(r_loop)) == "too_many_redirects")

    # 4B.4 body_too_large + timeout
    let r_big = http.request("GET", http_base + "/big", map[string]string{}, bytes.from_text(""), map[string]Json{"max_response_bytes": unwrap(json.from(1024))})
    assert_true(is_err(r_big) && error.code(unwrap_err(r_big)) == "body_too_large")

    let r_to = http.get(http_base + "/slow", map[string]string{}, 250)
    assert_true(is_err(r_to) && error.code(unwrap_err(r_to)) == "timeout")

    # 4B.5 HTTPS strict TLS: default fails on self-signed, ca_cert_pem succeeds, wrong CA fails
    let r_tls_def = http.get(https_base + "/echo", map[string]string{}, 5000)
    assert_true(is_err(r_tls_def) && error.domain(unwrap_err(r_tls_def)) == "http" && error.code(unwrap_err(r_tls_def)) == "tls")

    let r_tls_ok = unwrap(http.request("GET", https_base + "/echo", map[string]string{}, bytes.from_text(""), map[string]Json{"ca_cert_pem": unwrap(json.from(ca_pem))}))
    assert_true(http.status(r_tls_ok) == 200)

    let r_tls_bad = http.request("GET", https_base + "/echo", map[string]string{}, bytes.from_text(""), map[string]Json{"ca_cert_pem": unwrap(json.from(wrong_ca))})
    assert_true(is_err(r_tls_bad) && error.code(unwrap_err(r_tls_bad)) == "tls")
    print("4B=OK")
}
'''

ASYNC_CANCEL_HUA = r'''import std.bytes as bytes
import std.error as error
import std.http as http
import std.json as json
import std.os as os
import std.sse as sse
import std.task as task
import std.test as test

async fn slow_fetch(u string) string {
    let r = http.get(u, map[string]string{}, 5000)
    if is_err(r) {
        return error.code(unwrap_err(r))
    }
    return "ok"
}

async fn consume_hang(u string, ca string) string {
    let st = unwrap(http.open_stream("GET", u, map[string]string{}, bytes.from_text(""), map[string]Json{"total_deadline_ms": unwrap(json.from(15000)), "ca_cert_pem": unwrap(json.from(ca))}))
    var p = sse.new(http.stream_body(st))
    let first = unwrap(sse.next(p))
    test.assert(is_some(first) && unwrap(first).data == "first_msg", "first sse event")
    let r2 = sse.next(p)
    let _c1 = sse.close(p)
    let _c2 = http.stream_close(st)
    if is_err(r2) {
        return error.code(unwrap_err(r2))
    }
    return "done"
}

fn main() {
    let argv = os.args()
    let http_base = argv[0]
    let https_base = argv[1]
    let ca_pem = argv[2]

    let t1 = spawn slow_fetch(http_base + "/slow")
    let tr1 = task.timeout(t1, 150)
    test.assert(is_err(tr1) && task.done(t1), "http cancel")

    let t2 = spawn consume_hang(https_base + "/sse-hang", ca_pem)
    let tr2 = task.timeout(t2, 200)
    test.assert(is_err(tr2) && task.done(t2), "sse cancel")
    print("CANCEL=OK")
}
'''


AGENT_4C_HUA = r'''import sqlite as db
import std.bytes as bytes
import std.error as error
import std.http as http
import std.json as json
import std.os as os
import std.sse as sse
import std.test as test

fn assert_true(cond bool) {
    test.assert(cond, "assertion failed")
}

fn main() {
    let argv = os.args()
    let https_base = argv[0]
    let ca_pem = argv[1]
    let db_path = argv[2]

    let conn = unwrap(db.open(db_path))
    defer db.close(conn)
    unwrap(db.exec(conn, "CREATE TABLE events (id TEXT PRIMARY KEY, ev TEXT, payload TEXT)"))

    # Stream SSE over HTTPS with strict ca_cert_pem, random 1-7 byte chunks + UTF-8 multi-byte split
    let st = unwrap(http.open_stream("GET", https_base + "/sse", map[string]string{}, bytes.from_text(""), map[string]Json{"ca_cert_pem": unwrap(json.from(ca_pem))}))
    assert_true(http.stream_status(st) == 200)
    var p = sse.new(http.stream_body(st))

    unwrap(db.begin(conn))
    var count = 0
    while true {
        let ev_opt = unwrap(sse.next(p))
        if is_none(ev_opt) { break }
        let ev = unwrap(ev_opt)
        if ev.data == "[DONE]" {
            # Early close on [DONE]
            unwrap(sse.close(p))
            unwrap(http.stream_close(st))
            break
        }
        let _ = unwrap(db.query_exec(conn, "INSERT INTO events(id, ev, payload) VALUES(?, ?, ?)", [
            db.Param.Text(ev.id),
            db.Param.Text(ev.event),
            db.Param.Text(ev.data)
        ]))
        count = count + 1
    }
    unwrap(db.commit(conn))
    assert_true(count == 2)

    # Verify rows persisted in SQLite
    let q = unwrap(db.prepare(conn, "SELECT id, ev, payload FROM events ORDER BY id"))
    defer db.finalize(q)
    assert_true(unwrap(db.step(q)))
    assert_true(unwrap(db.column_text(q, 0)) == "1")
    assert_true(unwrap(db.column_text(q, 2)) == "{\"chunk\":\"你好\"}")
    assert_true(unwrap(db.step(q)))
    assert_true(unwrap(db.column_text(q, 0)) == "2")
    assert_true(unwrap(db.column_text(q, 2)) == "line1\n世界")
    assert_true(!unwrap(db.step(q)))

    print("4C=OK")
}
'''


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hua", default=str(ROOT / "tests/build/hua.exe"))
    ap.add_argument("--sdk", default="")
    ap.add_argument("--host-only", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="hua-step4-e2e-") as td:
        root = Path(td)
        ca_pem, wrong_ca, srv_pem, srv_key = generate_test_pki(root)
        srv_http, srv_other, srv_https, srv_state = start_test_servers(srv_pem, srv_key)
        http_base = f"http://127.0.0.1:{srv_http.server_address[1]}"
        https_base = f"https://127.0.0.1:{srv_https.server_address[1]}"

        try:
            # 1. Core 4A + 4B tests
            app_ab = root / "app_ab"
            app_ab.mkdir()
            (app_ab / "main.hua").write_text(CORE_4AB_HUA, encoding="utf-8")

            if args.host_only:
                hua = Path(args.hua).resolve()
                env = os.environ.copy()
                p1 = run([hua, "run", app_ab / "main.hua", "--", http_base, https_base, str(ca_pem), str(wrong_ca), str(root / "f1.bin")], app_ab, env)
                assert p1.returncode == 0 and p1.stdout == "4A=OK\n4B=OK\n", f"Host 4A/4B run failed:\nOUT={p1.stdout}\nERR={p1.stderr}"
                print("[PASS] Host VM 4A (std.stream) + 4B (std.url + std.http/HTTPS/cancel)", flush=True)

                huab_ab = root / "ab.huab"
                pb = run([hua, "build", app_ab / "main.hua", "-o", huab_ab], app_ab, env)
                assert pb.returncode == 0, f"Host HUAB build failed: {pb.stderr}"
                assert huab_header(huab_ab) == (31, 1), huab_header(huab_ab)
                p2 = run([hua, "run", huab_ab, "--", http_base, https_base, str(ca_pem), str(wrong_ca), str(root / "f2.bin")], root, env)
                assert p2.returncode == 0 and p2.stdout == "4A=OK\n4B=OK\n", f"Host HUAB 31 failed:\nOUT={p2.stdout}\nERR={p2.stderr}"
                print("[PASS] Host HUAB 31 4A + 4B", flush=True)
                return

            sdk = Path(args.sdk).resolve()
            hua = sdk / "bin/hua.exe"
            archive = sdk / "lib/compiler/hua-archive.exe"
            env = clean_env(sdk)

            # 4A + 4B across 4 paths
            p1 = run([hua, "run", app_ab / "main.hua", "--", http_base, https_base, str(ca_pem), str(wrong_ca), str(root / "f1.bin")], app_ab, env)
            assert p1.returncode == 0 and p1.stdout == "4A=OK\n4B=OK\n", f"Path1 4AB failed:\n{p1.stdout}\n{p1.stderr}"

            h_ab = root / "ab_host.huab"
            assert run([hua, "build", app_ab / "main.hua", "-o", h_ab], app_ab, env).returncode == 0
            assert huab_header(h_ab) == (31, 1)
            p2 = run([hua, "run", h_ab, "--", http_base, https_base, str(ca_pem), str(wrong_ca), str(root / "f2.bin")], root, env)
            assert p2.returncode == 0 and p2.stdout == "4A=OK\n4B=OK\n", f"Path2 4AB failed:\n{p2.stdout}\n{p2.stderr}"

            s_ab = root / "ab_sh.huab"
            ps_ab = run([archive, app_ab / "main.hua", s_ab], app_ab, env)
            assert ps_ab.returncode == 0, f"Selfhost archive 4AB failed: {ps_ab.stdout}\n{ps_ab.stderr}"
            rec_ab = json.loads(ps_ab.stdout)
            assert rec_ab.get("status") == "ok", f"Selfhost archive 4AB error: {rec_ab}"
            assert huab_header(s_ab) == (31, 1)
            p3 = run([hua, "run", s_ab, "--", http_base, https_base, str(ca_pem), str(wrong_ca), str(root / "f3.bin")], root, env)
            assert p3.returncode == 0 and p3.stdout == "4A=OK\n4B=OK\n", f"Path3 4AB failed:\n{p3.stdout}\n{p3.stderr}"

            exe_ab = root / "dist_ab/ab.exe"
            exe_ab.parent.mkdir()
            pr_ab = run([hua, "build", "--release", app_ab / "main.hua", "-o", exe_ab], app_ab, env)
            assert pr_ab.returncode == 0, f"Release 4AB failed: {pr_ab.stdout}\n{pr_ab.stderr}"
            p4 = run([exe_ab, http_base, https_base, str(ca_pem), str(wrong_ca), str(root / "f4.bin")], exe_ab.parent, clean_env(None))
            assert p4.returncode == 0 and p4.stdout == "4A=OK\n4B=OK\n", f"Path4 4AB failed:\n{p4.stdout}\n{p4.stderr}"
            print("[PASS] 4A + 4B (std.stream + std.url + std.http/HTTPS/cancel) across all 4 paths", flush=True)

            # 2. 4C Agent E2E: std.sse (pure Hua SDK module) + HTTPS + packages/sqlite across all 4 paths
            sqlite_pkg = root / "packages/sqlite"
            sqt.build_sqlite_package(sqt.DEFAULT_CLANG, sqlite_pkg)
            app_4c = root / "agent_app"
            app_4c.mkdir()
            (app_4c / "hua.package.json").write_text(
                json.dumps(
                    {
                        "schema": "hua.package.v1",
                        "name": "agent_app",
                        "version": "0.1.0",
                        "deps": {"sqlite": "../packages/sqlite"},
                    }
                ),
                encoding="utf-8",
            )
            (app_4c / "main.hua").write_text(AGENT_4C_HUA, encoding="utf-8")

            c1 = run([hua, "run", app_4c / "main.hua", "--", https_base, str(ca_pem), str(root / "c1.db")], app_4c, env)
            assert c1.returncode == 0 and c1.stdout == "4C=OK\n", f"Path1 4C failed:\n{c1.stdout}\n{c1.stderr}"
            print("[PASS] Path 1 (hua run): 4C HTTPS SSE -> SQLite + cancel", flush=True)

            b_h = root / "b_4c_host"
            (b_h / "native/sqlite").mkdir(parents=True)
            shutil.copy2(sqlite_pkg / "sqlite_native.dll", b_h / "native/sqlite/sqlite_native.dll")
            shutil.copy2(sqlite_pkg / "sqlite3.dll", b_h / "native/sqlite/sqlite3.dll")
            h_4c = b_h / "agent.huab"
            assert run([hua, "build", app_4c / "main.hua", "-o", h_4c], app_4c, env).returncode == 0
            assert huab_header(h_4c) == (31, 2)
            c2 = run([hua, "run", h_4c, "--", https_base, str(ca_pem), str(root / "c2.db")], b_h, env)
            assert c2.returncode == 0 and c2.stdout == "4C=OK\n", f"Path2 4C failed:\n{c2.stdout}\n{c2.stderr}"
            print("[PASS] Path 2 (Host HUAB 31 / native_abi 2): 4C HTTPS SSE -> SQLite + cancel", flush=True)

            b_s = root / "b_4c_sh"
            (b_s / "native/sqlite").mkdir(parents=True)
            shutil.copy2(sqlite_pkg / "sqlite_native.dll", b_s / "native/sqlite/sqlite_native.dll")
            shutil.copy2(sqlite_pkg / "sqlite3.dll", b_s / "native/sqlite/sqlite3.dll")
            s_4c = b_s / "agent.huab"
            ps_4c = run([archive, app_4c / "main.hua", s_4c], app_4c, env)
            assert ps_4c.returncode == 0, f"Selfhost archive 4C failed:\n{ps_4c.stdout}\n{ps_4c.stderr}"
            assert huab_header(s_4c) == (31, 2)
            c3 = run([hua, "run", s_4c, "--", https_base, str(ca_pem), str(root / "c3.db")], b_s, env)
            assert c3.returncode == 0 and c3.stdout == "4C=OK\n", f"Path3 4C failed:\n{c3.stdout}\n{c3.stderr}"
            print("[PASS] Path 3 (Selfhost HUAB 31 / native_abi 2): 4C HTTPS SSE -> SQLite + cancel", flush=True)

            dist_4c = root / "dist_4c"
            dist_4c.mkdir()
            pr_4c = run([hua, "build", "--release", app_4c / "main.hua", "-o", dist_4c / "agent.exe"], app_4c, env)
            assert pr_4c.returncode == 0, f"Release 4C failed:\n{pr_4c.stdout}\n{pr_4c.stderr}"
            clean_4c = root / "clean_4c"
            shutil.copytree(dist_4c, clean_4c)
            c4 = run([clean_4c / "agent.exe", https_base, str(ca_pem), str(clean_4c / "c4.db")], clean_4c, clean_env(None))
            assert c4.returncode == 0 and c4.stdout == "4C=OK\n", f"Path4 4C failed:\n{c4.stdout}\n{c4.stderr}"
            print("[PASS] Path 4 (Release EXE clean dir): 4C HTTPS SSE -> SQLite + cancel", flush=True)

            # 3. Async Task cancellation/timeout on in-flight HTTP & HTTPS SSE across Path 1/2/3
            app_cancel = root / "app_cancel"
            app_cancel.mkdir()
            (app_cancel / "main.hua").write_text(ASYNC_CANCEL_HUA, encoding="utf-8")
            ac1 = run([hua, "run", app_cancel / "main.hua", "--", http_base, https_base, str(ca_pem)], app_cancel, env)
            assert ac1.returncode == 0 and ac1.stdout == "CANCEL=OK\n", f"Path1 cancel failed:\n{ac1.stdout}\n{ac1.stderr}"
            h_ac = root / "cancel_host.huab"
            assert run([hua, "build", app_cancel / "main.hua", "-o", h_ac], app_cancel, env).returncode == 0
            assert huab_header(h_ac) == (31, 1)
            ac2 = run([hua, "run", h_ac, "--", http_base, https_base, str(ca_pem)], root, env)
            assert ac2.returncode == 0 and ac2.stdout == "CANCEL=OK\n", f"Path2 cancel failed:\n{ac2.stdout}\n{ac2.stderr}"
            s_ac = root / "cancel_sh.huab"
            ps_ac = run([archive, app_cancel / "main.hua", s_ac], app_cancel, env)
            assert ps_ac.returncode == 0 and json.loads(ps_ac.stdout).get("status") == "ok", ps_ac.stdout
            assert huab_header(s_ac) == (31, 1)
            ac3 = run([hua, "run", s_ac, "--", http_base, https_base, str(ca_pem)], root, env)
            assert ac3.returncode == 0 and ac3.stdout == "CANCEL=OK\n", f"Path3 cancel failed:\n{ac3.stdout}\n{ac3.stderr}"
            print("[PASS] Path 1/2/3 (VM + Host/Selfhost HUAB 31): Async Task cancel/timeout on HTTP & HTTPS SSE", flush=True)

            # 4. Negative: clone(Reader) rejected with E3008
            neg = root / "neg_clone.hua"
            neg.write_text(
                'import std.bytes as bytes\nimport std.stream as stream\nfn main() {\n    let r = stream.from_bytes(bytes.from_text("a"))\n    let _c = clone(r)\n}\n',
                encoding="utf-8",
            )
            pn = run([hua, "check", neg], root, env)
            assert pn.returncode != 0 and "E3008" in pn.stderr, f"Expected E3008: {pn.stderr}"
            print("[PASS] Negative: clone(std.stream.Reader) rejected with E3008", flush=True)

            print("ALL STEP 4 STREAM / HTTP / HTTPS / SSE / AGENT TESTS PASSED")
        finally:
            for s in (srv_http, srv_other, srv_https):
                s.shutdown()
                s.server_close()


if __name__ == "__main__":
    main()
