#!/usr/bin/env python3
"""Quick gRPC endpoint connectivity checker.

Examples:
  python scripts/check_grpc_endpoint.py --endpoint https://andres-unpecked-gary.ngrok-free.dev
  python scripts/check_grpc_endpoint.py --endpoint andres-unpecked-gary.ngrok-free.dev:443 --mode auto
  python scripts/check_grpc_endpoint.py --endpoint 127.0.0.1:50051 --mode insecure
"""

from __future__ import annotations

import argparse
import socket
import ssl
import time
from dataclasses import dataclass
from typing import Optional, Tuple
from urllib.parse import urlparse

import grpc


@dataclass
class ParsedEndpoint:
    target: str
    host: str
    port: int
    suggested_tls: bool


def parse_endpoint(raw: str) -> ParsedEndpoint:
    text = raw.strip()
    if not text:
        raise ValueError("empty endpoint")

    # URL form: http(s)://host[:port][/path]
    if "://" in text:
        parsed = urlparse(text)
        if parsed.scheme not in ("http", "https"):
            raise ValueError(f"unsupported URL scheme: {parsed.scheme}")
        if not parsed.hostname:
            raise ValueError("missing host in URL")
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        host = parsed.hostname
        target = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
        return ParsedEndpoint(target=target, host=host, port=port, suggested_tls=(parsed.scheme == "https"))

    # host:port or [ipv6]:port
    if text.startswith("["):
        end = text.find("]")
        if end < 0 or end + 1 >= len(text) or text[end + 1] != ":":
            raise ValueError("invalid [ipv6]:port format")
        host = text[1:end]
        port = int(text[end + 2 :])
        target = f"[{host}]:{port}"
        return ParsedEndpoint(target=target, host=host, port=port, suggested_tls=(port == 443))

    if text.count(":") < 1:
        raise ValueError("missing port; use host:port or https://host")

    host, port_str = text.rsplit(":", 1)
    if not host:
        raise ValueError("empty host")
    port = int(port_str)
    target = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    return ParsedEndpoint(target=target, host=host, port=port, suggested_tls=(port == 443))


def check_dns(host: str) -> Tuple[bool, str]:
    try:
        infos = socket.getaddrinfo(host, None)
        addrs = sorted({item[4][0] for item in infos})
        preview = ", ".join(addrs[:4])
        return True, preview or "resolved"
    except Exception as exc:
        return False, str(exc)


def check_tls_handshake(host: str, port: int, timeout_s: float) -> Tuple[bool, str]:
    try:
        context = ssl.create_default_context()
        with socket.create_connection((host, port), timeout=timeout_s) as sock:
            with context.wrap_socket(sock, server_hostname=host) as tls_sock:
                proto = tls_sock.version() or "unknown"
                cert = tls_sock.getpeercert() or {}
                subject = cert.get("subject", "-")
                return True, f"TLS OK ({proto}), cert_subject={subject}"
    except Exception as exc:
        return False, str(exc)


def check_grpc_channel(target: str, use_tls: bool, timeout_s: float) -> Tuple[bool, str, float]:
    started = time.perf_counter()
    if use_tls:
        channel = grpc.secure_channel(target, grpc.ssl_channel_credentials())
    else:
        channel = grpc.insecure_channel(target)

    try:
        grpc.channel_ready_future(channel).result(timeout=timeout_s)
        elapsed = (time.perf_counter() - started) * 1000.0
        return True, "channel ready", elapsed
    except Exception as exc:
        elapsed = (time.perf_counter() - started) * 1000.0
        return False, f"{type(exc).__name__}: {exc}", elapsed
    finally:
        channel.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate gRPC endpoint reachability and TLS mode.")
    parser.add_argument("--endpoint", required=True, help="Endpoint: host:port, [ipv6]:port, or https://host[:port]")
    parser.add_argument("--mode", choices=["auto", "tls", "insecure"], default="auto",
                        help="Connection mode. auto follows scheme/port and falls back when useful.")
    parser.add_argument("--timeout", type=float, default=4.0, help="Per-attempt timeout seconds (default: 4)")
    args = parser.parse_args()

    try:
        parsed = parse_endpoint(args.endpoint)
    except Exception as exc:
        print(f"[ERROR] endpoint parse failed: {exc}")
        return 2

    print(f"[INFO] endpoint input   : {args.endpoint}")
    print(f"[INFO] grpc target      : {parsed.target}")
    print(f"[INFO] host/port        : {parsed.host}:{parsed.port}")
    print(f"[INFO] suggested TLS    : {parsed.suggested_tls}")

    dns_ok, dns_msg = check_dns(parsed.host)
    if dns_ok:
        print(f"[OK]   DNS              : {dns_msg}")
    else:
        print(f"[WARN] DNS              : {dns_msg}")

    if args.mode in ("auto", "tls"):
        tls_ok, tls_msg = check_tls_handshake(parsed.host, parsed.port, args.timeout)
        if tls_ok:
            print(f"[OK]   TLS handshake    : {tls_msg}")
        else:
            print(f"[WARN] TLS handshake    : {tls_msg}")

    attempts = []
    if args.mode == "tls":
        attempts = [True]
    elif args.mode == "insecure":
        attempts = [False]
    else:
        # auto
        if parsed.suggested_tls:
            attempts = [True, False]
        else:
            attempts = [False, True] if parsed.port == 443 else [False]

    for use_tls in attempts:
        label = "TLS" if use_tls else "Insecure"
        ok, msg, elapsed_ms = check_grpc_channel(parsed.target, use_tls, args.timeout)
        if ok:
            print(f"[OK]   gRPC {label:<8}: {msg} ({elapsed_ms:.1f} ms)")
            print("[RESULT] PASS")
            return 0
        print(f"[FAIL] gRPC {label:<8}: {msg} ({elapsed_ms:.1f} ms)")

    print("[RESULT] FAIL")
    print("[HINT] Ensure ngrok tunnel type supports raw gRPC over HTTP/2 (not only grpc-web).")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
