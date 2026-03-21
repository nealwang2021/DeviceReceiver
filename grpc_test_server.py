#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gRPC 测试服务器 — DeviceDataService（被测设备数据）
==================================================
**命名区分：** 三轴台工装 StageService 请使用同目录下的 `stage_grpc_test_server.py`
（默认端口 50052），勿与本脚本混淆。

功能：
  1. Subscribe (Server-Streaming): 按客户端请求的间隔定时推送 DataFrame
     - 支持 MultiChannelReal / MultiChannelComplex / Legacy 三种模式
    - 字段语义：Real(comp0=幅值, comp1=相位)，Complex(comp0=实部, comp1=虚部)
     - 数据为正弦波 + 噪声，便于在波形图上观察
  2. SendCommand (Unary): 接收客户端控制指令并返回确认响应
     - 支持运行时切换模式、通道数、频率等参数

用法：
  python grpc_test_server.py [--port 50051] [--channels 8] [--mode complex]
                             [--interval 100] [--noise 0.3]

默认监听 0.0.0.0:50051，与 config.ini 中 GrpcEndpoint=127.0.0.1:50051 匹配。
"""

import sys
import os
import time
import math
import random
import signal
import struct
import argparse
import threading
from concurrent import futures

# ── 将 generated_py 加入搜索路径 ──────────────────────────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RUNTIME_BASE_DIR = getattr(sys, "_MEIPASS", SCRIPT_DIR)
# 支持脚本放在项目根目录或 proto/generated_py 目录下
for _candidate in [
    os.path.join(RUNTIME_BASE_DIR, "proto", "generated_py"),
    os.path.join(SCRIPT_DIR, "proto", "generated_py"),
    SCRIPT_DIR,
]:
    if os.path.isfile(os.path.join(_candidate, "device_data_pb2.py")):
        if _candidate not in sys.path:
            sys.path.insert(0, _candidate)
        break

import grpc
import device_data_pb2
import device_data_pb2_grpc


# ── 检测模式枚举（与 proto detect_mode 字段对应）─────────────
MODE_LEGACY = 0
MODE_REAL = 1
MODE_COMPLEX = 2

MODE_MAP = {
    "legacy": MODE_LEGACY,
    "real": MODE_REAL,
    "complex": MODE_COMPLEX,
}

# ── 固化的自检命令协议（客户端建议优先使用）──────────────
SELFTEST_CMD_PING = "selftest_ping"
SELFTEST_CMD_SET_MODE = "selftest_set_mode"   # 用法: selftest_set_mode complex|real|legacy
SELFTEST_CMD_STATUS = "selftest_status"

SELFTEST_ACK_PING = "SELFTEST_ACK:PING"


def build_selftest_mode_ack(mode_str: str) -> str:
    return f"SELFTEST_ACK:MODE:{mode_str}:{MODE_MAP[mode_str]}"


def build_selftest_status_ack(generator) -> str:
    return (
        f"SELFTEST_ACK:STATUS:channels={generator.channels},"
        f"mode={generator.mode},noise={generator.noise:.2f},frame_id={generator.frame_id}"
    )


class DataGenerator:
    """生成模拟传感器数据（正弦波 + 可配噪声）"""

    def __init__(self, channels: int = 8, mode: int = MODE_COMPLEX,
                 noise: float = 0.3):
        self.channels = channels
        self.mode = mode
        self.noise = noise
        self.frame_id = 0
        self._lock = threading.Lock()

        # 每个通道不同的基频，避免波形重叠
        self._freq = [0.5 + 0.3 * i for i in range(200)]       # Hz
        self._phase = [random.uniform(0, 2 * math.pi) for _ in range(200)]

    # ── 运行时参数修改（线程安全）──────────────────────────
    def set_channels(self, n: int):
        with self._lock:
            self.channels = max(1, min(n, 200))

    def set_mode(self, m: int):
        with self._lock:
            self.mode = m

    def set_noise(self, n: float):
        with self._lock:
            self.noise = max(0.0, n)

    # ── 生成一帧 ──────────────────────────────────────────
    def next_frame(self) -> device_data_pb2.DataFrame:
        with self._lock:
            ch = self.channels
            mode = self.mode
            noise = self.noise

        now_ms = int(time.time() * 1000)
        t = now_ms / 1000.0  # 秒

        self.frame_id += 1

        frame = device_data_pb2.DataFrame()
        frame.timestamp = now_ms
        frame.frame_id = self.frame_id & 0xFFFF
        frame.detect_mode = mode
        frame.channel_count = ch

        if mode == MODE_LEGACY:
            # Legacy 模式：空数据帧
            pass
        elif mode == MODE_REAL:
            # Real 模式：comp0 = 幅值, comp1 = 相位
            for i in range(ch):
                angle = 2 * math.pi * self._freq[i] * t + self._phase[i]
                amplitude = 8.0 + 2.0 * math.sin(angle) + random.gauss(0, noise)
                phase = math.atan2(math.sin(angle), math.cos(angle)) + random.gauss(0, noise * 0.05)
                frame.channels_comp0.append(amplitude)
                frame.channels_comp1.append(phase)
        elif mode == MODE_COMPLEX:
            # 复数模式：comp0 = 实部, comp1 = 虚部
            for i in range(ch):
                re = math.sin(2 * math.pi * self._freq[i] * t + self._phase[i])
                im = math.cos(2 * math.pi * self._freq[i] * t + self._phase[i])
                re += random.gauss(0, noise)
                im += random.gauss(0, noise)
                frame.channels_comp0.append(re)
                frame.channels_comp1.append(im)

        return frame


class DeviceDataServicer(device_data_pb2_grpc.DeviceDataServiceServicer):
    """gRPC 服务实现"""

    def __init__(self, generator: DataGenerator, default_interval_ms: int = 100):
        self.generator = generator
        self.default_interval_ms = default_interval_ms
        self._subscribers = 0
        self._lock = threading.Lock()

    def Subscribe(self, request, context):
        """服务端流式 RPC：持续推送 DataFrame"""
        interval_ms = request.interval_ms if request.interval_ms > 0 else self.default_interval_ms
        interval_ms = max(10, interval_ms)  # 最小 10ms
        interval_s = interval_ms / 1000.0

        # 如果客户端请求了特定通道数，临时覆盖
        if request.channel_count > 0:
            self.generator.set_channels(request.channel_count)

        with self._lock:
            self._subscribers += 1
            sub_id = self._subscribers

        peer = context.peer() or "unknown"
        print(f"[Subscribe] 客户端 #{sub_id} 已连接 ({peer})，"
              f"间隔={interval_ms}ms, 通道={self.generator.channels}, "
              f"模式={self.generator.mode}")

        try:
            while context.is_active():
                frame = self.generator.next_frame()
                yield frame
                time.sleep(interval_s)
        except Exception as e:
            print(f"[Subscribe] 客户端 #{sub_id} 流异常: {e}")
        finally:
            print(f"[Subscribe] 客户端 #{sub_id} 已断开 ({peer})")

    def SendCommand(self, request, context):
        """一元 RPC：接收控制指令"""
        payload = request.payload
        cmd_id = request.command_id
        peer = context.peer() or "unknown"

        print(f"[SendCommand] 来自 {peer} 的指令 (id={cmd_id})")
        print(f"  payload ({len(payload)} bytes): {payload[:64]!r}"
              + ("..." if len(payload) > 64 else ""))

        # ── 解析内置控制指令 ──────────────────────────────
        # 约定：payload 为 UTF-8 文本时尝试解析简单命令
        success = True
        message = "OK"

        try:
            text = payload.decode("utf-8", errors="replace").strip()
            parts = text.split()
            cmd = parts[0].lower() if parts else ""

            # -----------------------------------------------------------------
            # 1) 固化自检协议命令（优先匹配）
            # -----------------------------------------------------------------
            if cmd == SELFTEST_CMD_PING:
                message = SELFTEST_ACK_PING
                print(f"  → {message}")

            elif cmd == SELFTEST_CMD_SET_MODE:
                mode_str = parts[1].lower() if len(parts) > 1 else "complex"
                if mode_str in MODE_MAP:
                    self.generator.set_mode(MODE_MAP[mode_str])
                    message = build_selftest_mode_ack(mode_str)
                    print(f"  → {message}")
                else:
                    success = False
                    message = f"SELFTEST_ERR:UNKNOWN_MODE:{mode_str}"
                    print(f"  → {message}")

            elif cmd == SELFTEST_CMD_STATUS:
                message = build_selftest_status_ack(self.generator)
                print(f"  → {message}")

            # -----------------------------------------------------------------
            # 2) 兼容历史命令（保留）
            # -----------------------------------------------------------------
            elif cmd == "set_mode":
                # set_mode real / complex / legacy
                mode_str = parts[1].lower() if len(parts) > 1 else ""
                if mode_str in MODE_MAP:
                    self.generator.set_mode(MODE_MAP[mode_str])
                    message = f"模式已切换为 {mode_str} ({MODE_MAP[mode_str]})"
                    print(f"  → {message}")
                else:
                    success = False
                    message = f"未知模式: {mode_str}，可用: legacy/real/complex"

            elif cmd == "set_channels":
                # set_channels 16
                n = int(parts[1]) if len(parts) > 1 else 8
                self.generator.set_channels(n)
                message = f"通道数已设置为 {self.generator.channels}"
                print(f"  → {message}")

            elif cmd == "set_noise":
                # set_noise 0.5
                n = float(parts[1]) if len(parts) > 1 else 0.3
                self.generator.set_noise(n)
                message = f"噪声幅度已设置为 {n}"
                print(f"  → {message}")

            elif cmd == "ping":
                message = "pong"
                print(f"  → pong")

            elif cmd == "status":
                message = (f"channels={self.generator.channels}, "
                           f"mode={self.generator.mode}, "
                           f"noise={self.generator.noise:.2f}, "
                           f"frame_id={self.generator.frame_id}")
                print(f"  → {message}")

            else:
                # 未知命令 — 仍返回成功（模拟设备 ACK）
                message = f"已接收指令 ({len(payload)} bytes)"
                print(f"  → 未识别命令，原样确认")

        except Exception as e:
            success = False
            message = f"指令处理异常: {e}"
            print(f"  → 异常: {e}")

        return device_data_pb2.CommandResponse(
            success=success,
            message=message,
            command_id=cmd_id,
        )


def serve(port: int, generator: DataGenerator, interval_ms: int):
    """启动 gRPC 服务器"""
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    servicer = DeviceDataServicer(generator, default_interval_ms=interval_ms)
    device_data_pb2_grpc.add_DeviceDataServiceServicer_to_server(servicer, server)

    listen_addr = f"0.0.0.0:{port}"
    server.add_insecure_port(listen_addr)
    server.start()

    print("=" * 60)
    print(f"  gRPC 测试服务器已启动")
    print(f"  监听地址: {listen_addr}")
    print(f"  通道数:   {generator.channels}")
    print(f"  模式:     {generator.mode} "
          f"({'legacy' if generator.mode == 0 else 'real' if generator.mode == 1 else 'complex'})")
    print(f"  帧间隔:   {interval_ms} ms")
    print(f"  噪声幅度: {generator.noise}")
    print(f"  ─────────────────────────────────────────────")
    print(f"  运行时指令（客户端 SendCommand）:")
    print(f"    [固化自检协议] {SELFTEST_CMD_PING}")
    print(f"    [固化自检协议] {SELFTEST_CMD_SET_MODE} <complex|real|legacy>")
    print(f"    [固化自检协议] {SELFTEST_CMD_STATUS}")
    print(f"    set_mode real|complex|legacy")
    print(f"    set_channels <N>")
    print(f"    set_noise <F>")
    print(f"    ping / status")
    print("=" * 60)

    # 优雅退出
    stop_event = threading.Event()

    def on_signal(sig, frame):
        print("\n[信号] 正在关闭服务器...")
        stop_event.set()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    try:
        stop_event.wait()
    except KeyboardInterrupt:
        pass

    server.stop(grace=2)
    print("服务器已停止。")


def main():
    parser = argparse.ArgumentParser(
        description="DeviceDataService gRPC 测试服务器",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例：
  python grpc_test_server.py                        # 默认: 8通道 complex 100ms
  python grpc_test_server.py --channels 4 --mode real --interval 50
  python grpc_test_server.py --port 50052 --noise 0.1
        """,
    )
    parser.add_argument("--port", type=int, default=50051,
                        help="监听端口 (默认 50051)")
    parser.add_argument("--channels", type=int, default=8,
                        help="初始通道数 (默认 8, 最大 200)")
    parser.add_argument("--mode", type=str, default="complex",
                        choices=["legacy", "real", "complex"],
                        help="检测模式 (默认 complex)")
    parser.add_argument("--interval", type=int, default=100,
                        help="默认帧间隔毫秒数 (默认 100)")
    parser.add_argument("--noise", type=float, default=0.3,
                        help="噪声幅度 (默认 0.3)")

    args = parser.parse_args()

    generator = DataGenerator(
        channels=args.channels,
        mode=MODE_MAP[args.mode],
        noise=args.noise,
    )

    serve(args.port, generator, args.interval)


if __name__ == "__main__":
    main()
