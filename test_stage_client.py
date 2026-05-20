#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Stage gRPC 客户端验证脚本
用于测试 Stage 服务（三轴台）的连接和基本控制 RPC
用法:
  python test_stage_client.py
  python test_stage_client.py --host https://andres-unpecked-gary.ngrok-free.dev
  python test_stage_client.py --host 127.0.0.1 --port 50052
"""

import argparse
import sys
import os
import time
import grpc

# 添加 proto 生成路径
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(SCRIPT_DIR, 'proto', 'generated_py'))

import stage_pb2
import stage_pb2_grpc
from google.protobuf import empty_pb2


def parse_host_port(host_str: str, default_port: int = 50052):
    """解析 host:port 或 https://host 格式"""
    host_str = host_str.strip()
    use_tls = False
    if host_str.startswith("https://"):
        use_tls = True
        host_str = host_str[8:]
    elif host_str.startswith("http://"):
        host_str = host_str[7:]
    # 去掉尾随斜杠
    host_str = host_str.rstrip('/')
    if ':' in host_str:
        parts = host_str.rsplit(':', 1)
        return parts[0], int(parts[1]), use_tls
    return host_str, default_port, use_tls


def create_channel(host: str, port: int, use_tls: bool):
    """创建 gRPC channel"""
    target = f"{host}:{port}"
    if use_tls:
        creds = grpc.ssl_channel_credentials()
        # ngrok 需要设置 authority header
        options = (('grpc.ssl_target_name_override', host),)
        print(f"  创建 TLS channel → {target}")
        return grpc.secure_channel(target, creds, options=options)
    else:
        print(f"  创建 insecure channel → {target}")
        return grpc.insecure_channel(target)


def test_connect(stub, stage_ip: str, stage_port: int):
    """测试 Connect RPC"""
    print("\n[Connect] 调用 Connect RPC...")
    try:
        req = stage_pb2.ConnectRequest(ip=stage_ip, port=stage_port)
        resp = stub.Connect(req, timeout=5)
        assert resp.ok, f"Connect 失败: {resp.message}"
        print(f"  [OK] Connect 成功: {resp.message}")
        return True
    except grpc.RpcError as e:
        print(f"  [FAIL] Connect 失败: [{e.code()}] {e.details()}")
        return False


def test_get_positions(stub):
    """测试 GetPositions RPC"""
    print("\n[GetPositions] 调用 GetPositions RPC...")
    try:
        resp = stub.GetPositions(empty_pb2.Empty(), timeout=3)
        x_mm = resp.x.mm if resp.HasField('x') else 0
        y_mm = resp.y.mm if resp.HasField('y') else 0
        z_mm = resp.z.mm if resp.HasField('z') else 0
        print(f"  [OK] GetPositions 成功: X={x_mm:.2f}mm Y={y_mm:.2f}mm Z={z_mm:.2f}mm")
        print(f"     Pulse: X={resp.x.pulse} Y={resp.y.pulse} Z={resp.z.pulse}")
        return True
    except grpc.RpcError as e:
        print(f"  [FAIL] GetPositions 失败: [{e.code()}] {e.details()}")
        return False


def test_jog(stub, axis: str, plus: bool, enable: bool):
    """测试 Jog RPC"""
    axis_map = {'x': stage_pb2.X, 'y': stage_pb2.Y, 'z': stage_pb2.Z}
    ax = axis_map.get(axis.lower(), stage_pb2.X)
    direction = "+" if plus else "-"
    state = "on" if enable else "off"
    print(f"\n[Jog] {axis.upper()} {direction}{state}...")
    try:
        req = stage_pb2.JogRequest(axis=ax, plus=plus, enable=enable)
        resp = stub.Jog(req, timeout=5)
        assert resp.ok, f"Jog 失败: {resp.message}"
        print(f"  [OK] Jog 成功: {resp.message}")
        return True
    except grpc.RpcError as e:
        print(f"  [FAIL] Jog 失败: [{e.code()}] {e.details()}")
        return False


def test_move_abs(stub, x_mm: float, y_mm: float, z_mm: float, timeout_ms: int = 10000):
    """测试 MoveAbs RPC"""
    print(f"\n[MoveAbs] X={x_mm} Y={y_mm} Z={z_mm} timeout={timeout_ms}ms...")
    try:
        req = stage_pb2.MoveAbsRequest(xMm=x_mm, yMm=y_mm, zMm=z_mm, timeoutMs=timeout_ms)
        resp = stub.MoveAbs(req, timeout=timeout_ms / 1000.0 + 5)
        assert resp.ok, f"MoveAbs 失败: {resp.message}"
        print(f"  [OK] MoveAbs 成功: {resp.message}")
        return True
    except grpc.RpcError as e:
        print(f"  [FAIL] MoveAbs 失败: [{e.code()}] {e.details()}")
        return False


def test_move_rel(stub, axis: str, delta_mm: float, timeout_ms: int = 5000):
    """测试 MoveRel RPC"""
    axis_map = {'x': stage_pb2.X, 'y': stage_pb2.Y, 'z': stage_pb2.Z}
    ax = axis_map.get(axis.lower(), stage_pb2.X)
    print(f"\n[MoveRel] {axis.upper()} delta={delta_mm}mm timeout={timeout_ms}ms...")
    try:
        req = stage_pb2.MoveRelRequest(axis=ax, deltaMm=delta_mm, timeoutMs=timeout_ms)
        resp = stub.MoveRel(req, timeout=timeout_ms / 1000.0 + 5)
        assert resp.ok, f"MoveRel 失败: {resp.message}"
        print(f"  [OK] MoveRel 成功: {resp.message}")
        return True
    except grpc.RpcError as e:
        print(f"  [FAIL] MoveRel 失败: [{e.code()}] {e.details()}")
        return False


def test_status(stub):
    """测试 GetStatus RPC (如果有的话)"""
    print("\n[GetStatus] 调用 GetStatus...")
    try:
        resp = stub.GetPositions(empty_pb2.Empty(), timeout=3)
        # 成功获取位置就等于状态正常
        print(f"  [OK] GetStatus (via GetPositions) OK")
        return True
    except grpc.RpcError as e:
        print(f"  [WARN] GetPositions 失败: [{e.code()}] {e.details()}")
        return False


def test_position_stream(stub, count: int = 3, interval_ms: int = 500):
    """测试 PositionStream"""
    print(f"\n[PositionStream] 读取 {count} 帧 (间隔 {interval_ms}ms)...")
    try:
        req = stage_pb2.PositionStreamRequest(intervalMs=interval_ms)
        n = 0
        for reply in stub.PositionStream(req, timeout=interval_ms / 1000.0 * count + 5):
            n += 1
            x_mm = reply.x.mm if reply.HasField('x') else 0
            y_mm = reply.y.mm if reply.HasField('y') else 0
            z_mm = reply.z.mm if reply.HasField('z') else 0
            print(f"  Frame#{n}: X={x_mm:.2f} Y={y_mm:.2f} Z={z_mm:.2f}")
            if n >= count:
                break
        if n > 0:
            print(f"  [OK] PositionStream 收到 {n} 帧")
            return True
        else:
            print(f"  [WARN] PositionStream 未收到数据")
            return False
    except grpc.RpcError as e:
        print(f"  [FAIL] PositionStream 失败: [{e.code()}] {e.details()}")
        return False


def test_disconnect(stub):
    """测试 Disconnect RPC"""
    print("\n[Disconnect] 调用 Disconnect RPC...")
    try:
        resp = stub.Disconnect(empty_pb2.Empty(), timeout=3)
        print(f"  [OK] Disconnect 成功: {resp.message}")
        return True
    except grpc.RpcError as e:
        print(f"  [WARN] Disconnect 失败: [{e.code()}] {e.details()}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Stage gRPC 客户端验证脚本")
    parser.add_argument("--host", default="127.0.0.1", help="Stage 服务地址 (默认 127.0.0.1)")
    parser.add_argument("--port", type=int, default=None, help="端口 (默认 50052, https 时默认 443)")
    parser.add_argument("--stage-ip", default="", help="Connect RPC 用的台下位机 IP（空则跳过）")
    parser.add_argument("--stage-port", type=int, default=9000, help="Connect RPC 用的台下位机端口")
    parser.add_argument("--skip-jog", action="store_true", help="跳过 Jog/MoveAbs 等运动指令")
    parser.add_argument("--skip-stream", action="store_true", help="跳过 PositionStream 测试")
    args = parser.parse_args()

    default_port = 443 if args.host.startswith("https://") else 50052
    host, port, use_tls = parse_host_port(args.host, args.port or default_port)
    if args.port:
        port = args.port

    print("=" * 60)
    print(f"Stage gRPC 客户端测试")
    print(f"  目标: {host}:{port} (TLS={use_tls})")
    print(f"  台下位机: {args.stage_ip}:{args.stage_port}")
    print("=" * 60)

    results = {}

    # 1. 创建 channel 并验证连通性
    channel = create_channel(host, port, use_tls)
    try:
        grpc.channel_ready_future(channel).result(timeout=10)
        print("  [OK] gRPC channel 已就绪")
    except grpc.FutureTimeoutError:
        print("  [FAIL] gRPC channel 连接超时")
        return 1

    stub = stage_pb2_grpc.StageServiceStub(channel)

    # 2. Connect (optional for ngrok-style connections)
    if args.stage_ip:
        results['Connect'] = test_connect(stub, args.stage_ip, args.stage_port)
    else:
        print("\n[Connect] 跳过（直接 RPC 模式）")

    # 3. GetPositions
    results['GetPositions'] = test_get_positions(stub)

    # 4. PositionStream (可选)
    if not args.skip_stream:
        results['PositionStream'] = test_position_stream(stub, count=3)

    # 5. 运动控制 (可选)
    if not args.skip_jog:
        results['Jog_X+'] = test_jog(stub, 'x', plus=True, enable=True)
        time.sleep(0.3)
        results['Jog_off'] = test_jog(stub, 'x', plus=True, enable=False)

        results['MoveRel_X'] = test_move_rel(stub, 'x', 1.0)

    # 6. Disconnect
    results['Disconnect'] = test_disconnect(stub)

    # 汇总
    print("\n" + "=" * 60)
    print("测试结果汇总:")
    passed = sum(1 for v in results.values() if v)
    total = len(results)
    for name, ok in results.items():
        print(f"  {'[OK]' if ok else '[FAIL]'} {name}")
    print(f"\n通过: {passed}/{total}")
    print("=" * 60)

    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
