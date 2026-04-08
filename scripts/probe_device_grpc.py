#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Probe AcquisitionDevice gRPC endpoint with device.proto flow.

Flow:
  1) ListDevices
  2) OpenDevice(first)
  3) StartSampling
  4) SubscribeProcessedFrames (read N frames)
  5) StopSampling + CloseDevice (best effort)

Usage:
  python scripts/probe_device_grpc.py
  python scripts/probe_device_grpc.py --endpoint https://andres-unpecked-gary.ngrok-free.dev --frames 5
  python scripts/probe_device_grpc.py --endpoint host:port --mode auto
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass
from typing import Optional
from urllib.parse import urlparse

import grpc
from google.protobuf import empty_pb2


SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
GEN_PY_DIR = os.path.join(ROOT_DIR, "proto", "generated_py")
if GEN_PY_DIR not in sys.path:
    sys.path.insert(0, GEN_PY_DIR)

import device_pb2  # noqa: E402
import device_pb2_grpc  # noqa: E402


@dataclass
class Endpoint:
    target: str
    tls_hint: bool


def parse_endpoint(raw: str) -> Endpoint:
    text = (raw or "").strip()
    if not text:
        raise ValueError("empty endpoint")

    if "://" in text:
        u = urlparse(text)
        if not u.hostname:
            raise ValueError("invalid URL endpoint")
        scheme = (u.scheme or "").lower()
        if scheme not in ("http", "https"):
            raise ValueError(f"unsupported scheme: {scheme}")
        port = u.port or (443 if scheme == "https" else 80)
        host = u.hostname
        target = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
        return Endpoint(target=target, tls_hint=(scheme == "https"))

    # host:port or [ipv6]:port
    if text.startswith("["):
        end = text.find("]")
        if end < 0 or end + 1 >= len(text) or text[end + 1] != ":":
            raise ValueError("invalid [ipv6]:port")
        host = text[1:end]
        port = int(text[end + 2 :])
        return Endpoint(target=f"[{host}]:{port}", tls_hint=(port == 443))

    if ":" not in text:
        raise ValueError("missing port, use host:port or https://host")

    host, port_str = text.rsplit(":", 1)
    port = int(port_str)
    target = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    return Endpoint(target=target, tls_hint=(port == 443))


def make_channel(target: str, tls: bool) -> grpc.Channel:
    if tls:
        return grpc.secure_channel(target, grpc.ssl_channel_credentials())
    return grpc.insecure_channel(target)


def call_with_log(name: str, fn, *args, timeout: float):
    started = time.perf_counter()
    try:
        resp = fn(*args, timeout=timeout)
        elapsed = (time.perf_counter() - started) * 1000.0
        print(f"[OK]   {name} ({elapsed:.1f} ms)")
        return resp
    except grpc.RpcError as e:
        elapsed = (time.perf_counter() - started) * 1000.0
        code = e.code().name if hasattr(e, "code") else "UNKNOWN"
        detail = e.details() if hasattr(e, "details") else str(e)
        raise RuntimeError(f"{name} failed ({elapsed:.1f} ms): {code} {detail}") from e


def run_flow(endpoint: Endpoint, tls: bool, frames: int, timeout: float, stream_timeout: float) -> bool:
    mode = "TLS" if tls else "Insecure"
    print(f"[INFO] trying {mode} channel -> {endpoint.target}")
    channel = make_channel(endpoint.target, tls)
    try:
        grpc.channel_ready_future(channel).result(timeout=timeout)
        print("[OK]   channel ready")
        stub = device_pb2_grpc.AcquisitionDeviceStub(channel)

        empty = empty_pb2.Empty()
        list_reply = call_with_log("ListDevices", stub.ListDevices, empty, timeout=timeout)
        if len(list_reply.devices) <= 0:
            raise RuntimeError("ListDevices returned 0 devices")

        first = list_reply.devices[0]
        device_id = first.device_id or ""
        print(f"[INFO] first device id={device_id} opened={first.opened} name={first.display_name}")

        open_req = device_pb2.OpenDeviceRequest(device_id=device_id)
        open_reply = call_with_log("OpenDevice", stub.OpenDevice, open_req, timeout=timeout)
        print(f"[INFO] OpenDevice ok={open_reply.ok} msg={open_reply.message}")
        if not open_reply.ok:
            raise RuntimeError(f"OpenDevice returned ok=false: {open_reply.message}")

        start_reply = call_with_log("StartSampling", stub.StartSampling, empty, timeout=timeout)
        print(f"[INFO] StartSampling ok={start_reply.ok} msg={start_reply.message}")
        if not start_reply.ok:
            raise RuntimeError(f"StartSampling returned ok=false: {start_reply.message}")

        status_reply = call_with_log("GetStatus", stub.GetStatus, empty, timeout=timeout)
        print(
            f"[INFO] Status opened={status_reply.opened} sampling={status_reply.sampling} "
            f"device_id={status_reply.current_device_id} cell_count={status_reply.cell_count}"
        )

        if stream_timeout > 0:
            stream = stub.SubscribeProcessedFrames(empty, timeout=stream_timeout)
        else:
            stream = stub.SubscribeProcessedFrames(empty)
        got = 0
        started_stream = time.perf_counter()
        for msg in stream:
            got += 1
            sample_count = len(msg.samples)
            print(
                f"[DATA] seq={msg.sequence} ts={msg.timestamp_unix_ms} cells={msg.cell_count} samples={sample_count}"
            )
            if got >= frames:
                break

        elapsed = (time.perf_counter() - started_stream) * 1000.0
        if got <= 0:
            raise RuntimeError(f"SubscribeProcessedFrames got 0 frames in {elapsed:.1f} ms")
        print(f"[OK]   SubscribeProcessedFrames got {got} frame(s) in {elapsed:.1f} ms")

        # best effort stop/close
        try:
            stop_reply = stub.StopSampling(empty, timeout=timeout)
            print(f"[INFO] StopSampling ok={stop_reply.ok} msg={stop_reply.message}")
        except Exception as e:
            print(f"[WARN] StopSampling failed: {e}")
        try:
            close_reply = stub.CloseDevice(empty, timeout=timeout)
            print(f"[INFO] CloseDevice ok={close_reply.ok} msg={close_reply.message}")
        except Exception as e:
            print(f"[WARN] CloseDevice failed: {e}")

        return True
    finally:
        channel.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Probe device.proto AcquisitionDevice endpoint")
    parser.add_argument(
        "--endpoint",
        default="https://andres-unpecked-gary.ngrok-free.dev",
        help="Endpoint: https://host or host:port",
    )
    parser.add_argument("--frames", type=int, default=3, help="How many stream frames to read")
    parser.add_argument("--timeout", type=float, default=6.0, help="RPC/channel timeout seconds")
    parser.add_argument(
        "--stream-timeout",
        type=float,
        default=20.0,
        help="SubscribeProcessedFrames timeout seconds (0 = no deadline)",
    )
    parser.add_argument("--mode", choices=["auto", "tls", "insecure"], default="auto")
    args = parser.parse_args()

    try:
        ep = parse_endpoint(args.endpoint)
    except Exception as e:
        print(f"[ERROR] endpoint parse failed: {e}")
        return 2

    print(f"[INFO] endpoint input : {args.endpoint}")
    print(f"[INFO] grpc target    : {ep.target}")

    attempts = []
    if args.mode == "tls":
        attempts = [True]
    elif args.mode == "insecure":
        attempts = [False]
    else:
        attempts = [ep.tls_hint, not ep.tls_hint]

    seen = set()
    ordered = []
    for a in attempts:
        if a in seen:
            continue
        seen.add(a)
        ordered.append(a)

    errors = []
    for tls in ordered:
        try:
            if run_flow(
                ep,
                tls=tls,
                frames=max(1, args.frames),
                timeout=max(1.0, args.timeout),
                stream_timeout=max(0.0, args.stream_timeout),
            ):
                print("[RESULT] PASS")
                return 0
        except Exception as e:
            label = "TLS" if tls else "Insecure"
            msg = f"{label}: {e}"
            errors.append(msg)
            print(f"[FAIL] {msg}")

    print("[RESULT] FAIL")
    print("[DETAIL] " + " | ".join(errors))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
