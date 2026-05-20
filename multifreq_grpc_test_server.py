#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# multifreq_grpc_test_server.py 使用说明
# =========================================
# 作用：
# - 基于 proto/multifreq_eddy.proto 提供 MultiFreqEddyCurrent gRPC 模拟服务
# - 生成含多频点阻抗数据的 DetectionFrame（Mock 纯模拟，无需外部数据源）
# - 配合主程序测试多频涡流后端（GrpcMultiFreqBackend）
#
# 常用启动：
#   python multifreq_grpc_test_server.py
#   python multifreq_grpc_test_server.py --port 50051
#   python multifreq_grpc_test_server.py --base-freq 100 --factors 1,2,4,8 --interval 200
#
# 参数摘要：
#   --host / --port         : 监听地址（默认 0.0.0.0:50051）
#   --base-freq             : 基频 Hz（默认 100，可选 1/2/5/10/20/50/100/200/500/1000）
#   --avg-cycle             : 平均周期数（默认 10）
#   --norm-scale            : 归一化系数（默认 1.0）
#   --factors               : 逗号分隔倍频系数（默认 "1,2,4,8"）
#   --interval              : 帧间隔 ms（默认 100）
#   --noise                 : 阻抗噪声幅度（默认 0.005）
#

import argparse
import math
import os
import random
import signal
import sys
import threading
import time
from concurrent import futures

# PyInstaller support
if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
    SCRIPT_DIR = sys._MEIPASS
else:
    SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for candidate in [
    os.path.join(SCRIPT_DIR, "proto", "generated_py"),
    SCRIPT_DIR,
]:
    if os.path.isfile(os.path.join(candidate, "multifreq_eddy_pb2.py")):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
        break

import grpc
from google.protobuf import empty_pb2
import multifreq_eddy_pb2 as mf_pb2
import multifreq_eddy_pb2_grpc as mf_grpc


VALID_BASE_FREQUENCIES = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000}


def _base_freq_enum(hz: int):
    """将整数 Hz 映射到 proto BaseFrequency 枚举值。"""
    name = f"BASE_FREQUENCY_HZ_{hz}"
    try:
        return mf_pb2.BaseFrequency.Value(name)
    except ValueError:
        return mf_pb2.BASE_FREQUENCY_HZ_100


class MultiFreqGenerator:
    """多频涡流模拟数据生成器。"""

    def __init__(
        self,
        base_freq_hz: int = 100,
        avg_cycle: int = 10,
        norm_scale: float = 1.0,
        factors: list = None,
        noise: float = 0.005,
    ):
        self.base_freq_hz = base_freq_hz if base_freq_hz in VALID_BASE_FREQUENCIES else 100
        self.avg_cycle_count = max(1, avg_cycle)
        self.norm_scale = max(0.01, norm_scale)
        self.factors = factors if factors else [1, 2, 4, 8]
        self.noise = max(0.0, noise)
        self.frame_index = 0
        self._lock = threading.Lock()
        self._running = False
        self._selected_serial = ""
        self._selected_desc = ""
        self._start_time = 0.0

    def _compute_sample_rate(self) -> int:
        """采样率 = 基频 * 最高倍频 * 平均周期数 * 4（每周期4点）"""
        max_factor = max(self.factors) if self.factors else 1
        return self.base_freq_hz * max_factor * self.avg_cycle_count * 4

    def _compute_samples_per_frame(self) -> int:
        """每帧采样数 = 采样率 / 最低倍频（至少一个完整周期）"""
        min_factor = min(self.factors) if self.factors else 1
        return max(256, self._compute_sample_rate() // (self.base_freq_hz * min_factor))

    def list_devices(self):
        return [
            mf_pb2.DeviceInfo(
                index=0,
                description="多频涡流 Mock Device 001",
                serial_number="MF-MOCK-001",
                type="multi_freq_eddy_current",
            )
        ]

    def start_detection(self, serial: str, device_index: int, config):
        with self._lock:
            if config.base_frequency and config.base_frequency != mf_pb2.BASE_FREQUENCY_UNSPECIFIED:
                for hz in VALID_BASE_FREQUENCIES:
                    if _base_freq_enum(hz) == config.base_frequency:
                        self.base_freq_hz = hz
                        break
            if config.average_cycle_count > 0:
                self.avg_cycle_count = config.average_cycle_count
            if config.normalize_scale > 0:
                self.norm_scale = config.normalize_scale
            if config.frequency_factors:
                self.factors = list(config.frequency_factors)

            self._running = True
            self._selected_serial = serial or "MF-MOCK-001"
            self._selected_desc = f"多频涡流 BaseFreq={self.base_freq_hz}Hz Factors={self.factors}"
            self._start_time = time.time()
            return True, f"started: {self._selected_desc}"
        return True, "started"

    def stop_detection(self):
        with self._lock:
            self._running = False
            return True, "stopped"

    def status(self):
        with self._lock:
            sr = self._compute_sample_rate()
            spf = self._compute_samples_per_frame()
            return mf_pb2.RuntimeStatus(
                running=self._running,
                selected_device_serial_number=self._selected_serial,
                selected_device_description=self._selected_desc,
                base_frequency_hz=self.base_freq_hz,
                sample_rate_hz=sr,
                sample_count_per_frame=spf,
                average_cycle_count=self.avg_cycle_count,
                queue_capacity=1024,
                max_queue_len=32,
                overrun_count=0,
                frame_index=self.frame_index,
            )

    def next_frame(self):
        with self._lock:
            if not self._running:
                return None
            self.frame_index += 1
            fid = self.frame_index
            bf = self.base_freq_hz
            sr = self._compute_sample_rate()
            spf = self._compute_samples_per_frame()
            factors = list(self.factors)
            norm = self.norm_scale

        now_ms = int(time.time() * 1000)
        elapsed = time.time() - self._start_time

        frame = mf_pb2.DetectionFrame()
        frame.timestamp_unix_ms = now_ms
        frame.frame_index = fid
        frame.base_frequency_hz = bf
        frame.sample_rate_hz = sr
        frame.sample_count_per_frame = spf

        # include_curve=false, include_spectrum=false → 不填充 curve_channels 和 amplitude_spectrum_channels

        # 生成每个频点的阻抗数据
        for factor in factors:
            freq_hz = float(bf * factor)
            pt = frame.point_results.add()
            pt.frequency_factor = factor
            pt.frequency_hz = freq_hz

            # 模拟多频涡流信号特征：
            # - 基频信号最强，谐波逐渐衰减
            # - 阻抗幅值 ~ 0.01 - 0.1 Ω
            # - 相位随频率和材料特性变化
            base_mag = 0.03 / math.sqrt(factor)  # 谐波衰减
            phase_base = (factor - 1) * 25.0 + random.gauss(0, 5.0)  # 谐波相移
            noise_mag = random.gauss(0, self.noise)
            noise_phase = random.gauss(0, self.noise * 0.3)

            mag = base_mag + noise_mag
            phase_deg = phase_base + noise_phase + 180.0 * math.sin(elapsed * 2.0 * math.pi * 0.1)
            phase_rad = math.radians(phase_deg)

            z_real = mag * math.cos(phase_rad)
            z_imag = mag * math.sin(phase_rad)

            pt.impedance_real = z_real
            pt.impedance_imag = z_imag
            pt.impedance_magnitude = mag
            pt.impedance_phase_deg = phase_deg

            # 归一化阻抗: Z/(2*pi*f)*1e6
            denom = 2.0 * math.pi * freq_hz
            pt.normalized_impedance_real = (z_real / denom) * 1e6 if denom > 0 else 0.0
            pt.normalized_impedance_imag = (z_imag / denom) * 1e6 if denom > 0 else 0.0

            # 电压/电流（ComplexValue 子消息）
            v_mag = 1.0 + 0.1 * math.sin(elapsed * 1.5 * math.pi)
            i_mag = v_mag / max(mag, 1e-9) if mag > 1e-9 else 10.0
            pt.voltage.real = v_mag * math.cos(phase_rad * 0.5)
            pt.voltage.imag = v_mag * math.sin(phase_rad * 0.5)
            pt.current.real = i_mag * math.cos(-phase_rad * 0.3)
            pt.current.imag = i_mag * math.sin(-phase_rad * 0.3)
            pt.voltage_magnitude = v_mag
            pt.current_magnitude = i_mag
            pt.valid = True

        frame.status.CopyFrom(self.status())
        return frame


class MultiFreqEddyCurrentServicer(mf_grpc.MultiFreqEddyCurrentServicer):
    def __init__(self, generator: MultiFreqGenerator, interval_ms: int = 100):
        self.generator = generator
        self.interval_ms = max(1, interval_ms)

    def ListDevices(self, request, context):
        reply = mf_pb2.ListDevicesResponse()
        reply.devices.extend(self.generator.list_devices())
        return reply

    def StartDetection(self, request, context):
        serial = request.device_serial_number or ""
        device_index = request.device_index or 0
        config = request.config
        ok, msg = self.generator.start_detection(serial, device_index, config)
        return mf_pb2.OperationReply(ok=ok, message=msg)

    def StopDetection(self, request, context):
        ok, msg = self.generator.stop_detection()
        return mf_pb2.OperationReply(ok=ok, message=msg)

    def GetStatus(self, request, context):
        return self.generator.status()

    def GetLatestFrame(self, request, context):
        frame = self.generator.next_frame()
        if frame is not None:
            return frame
        context.set_code(grpc.StatusCode.UNAVAILABLE)
        context.set_details("detection not running")
        return mf_pb2.DetectionFrame()

    def StreamFrames(self, request, context):
        interval_sec = self.interval_ms / 1000.0
        while context.is_active():
            frame = self.generator.next_frame()
            if frame is not None:
                yield frame
            time.sleep(interval_sec)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Mock gRPC server for multifreq_eddy.proto MultiFreqEddyCurrent",
    )
    parser.add_argument("--host", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    parser.add_argument("--port", type=int, default=50051, help="监听端口（默认 50051）")
    parser.add_argument("--base-freq", type=int, default=100,
                        choices=sorted(VALID_BASE_FREQUENCIES),
                        help=f"基频 Hz（默认 100）")
    parser.add_argument("--avg-cycle", type=int, default=10, help="平均周期数（默认 10）")
    parser.add_argument("--norm-scale", type=float, default=1.0, help="归一化系数（默认 1.0）")
    parser.add_argument("--factors", type=str, default="1,2,4,8",
                        help="逗号分隔倍频系数（默认 1,2,4,8）")
    parser.add_argument("--interval", type=int, default=100, help="帧间隔 ms（默认 100）")
    parser.add_argument("--noise", type=float, default=0.005, help="噪声幅度（默认 0.005）")
    return parser.parse_args()


def main():
    args = parse_args()

    factors = [int(x.strip()) for x in args.factors.split(",") if x.strip().isdigit()]
    if not factors:
        factors = [1, 2, 4, 8]

    generator = MultiFreqGenerator(
        base_freq_hz=args.base_freq,
        avg_cycle=args.avg_cycle,
        norm_scale=args.norm_scale,
        factors=factors,
        noise=args.noise,
    )

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    mf_grpc.add_MultiFreqEddyCurrentServicer_to_server(
        MultiFreqEddyCurrentServicer(generator, interval_ms=args.interval), server
    )

    bind_addr = f"{args.host}:{args.port}"
    server.add_insecure_port(bind_addr)
    server.start()

    print(f"[multifreq_grpc_test_server] MultiFreqEddyCurrent listening on {bind_addr}")
    print(f"  base_freq      = {args.base_freq} Hz")
    print(f"  avg_cycle      = {args.avg_cycle}")
    print(f"  norm_scale     = {args.norm_scale}")
    print(f"  factors        = {factors}")
    print(f"  frame_interval = {args.interval} ms")
    print(f"  noise          = {args.noise}")
    print("  methods: ListDevices/StartDetection/StopDetection/GetStatus/GetLatestFrame/StreamFrames")

    stop_event = threading.Event()

    def _stop_handler(signum, frame):
        stop_event.set()

    signal.signal(signal.SIGINT, _stop_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _stop_handler)

    try:
        while not stop_event.is_set():
            time.sleep(0.2)
    finally:
        server.stop(grace=1)


if __name__ == "__main__":
    main()
