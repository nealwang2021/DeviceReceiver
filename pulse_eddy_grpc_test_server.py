#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
脉冲涡流 (PulseEddy) gRPC 测试服务器
用法: python pulse_eddy_grpc_test_server.py [--port 50055]
"""

import argparse, base64, csv, math, os, random, signal, struct, sys, threading, time
from concurrent import futures
from collections import OrderedDict
from datetime import datetime

if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
    SCRIPT_DIR = sys._MEIPASS
else:
    SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for candidate in [os.path.join(SCRIPT_DIR, "proto", "generated_py"), SCRIPT_DIR]:
    if os.path.isfile(os.path.join(candidate, "pulse_eddy_pb2.py")):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
        break

import grpc
from google.protobuf import empty_pb2
import pulse_eddy_pb2 as pe_pb2
import pulse_eddy_pb2_grpc as pe_grpc

SAMPLE_COUNT = 64000  # 每帧采样点数

# ---- 日志 ----
_LOG_LOCK = threading.Lock()

def _log(prefix, msg):
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    with _LOG_LOCK:
        print(f"{ts}[{prefix}]{msg}", flush=True)


# ---- CSV 数据加载 ----

def _b64_to_doubles(b64_str: str) -> list:
    """将 base64 字符串解码为 double 列表。"""
    if not b64_str or not b64_str.strip():
        return []
    raw_bytes = base64.b64decode(b64_str)
    n = len(raw_bytes) // 8
    return list(struct.unpack(f"<{n}d", raw_bytes)) if n > 0 else []


def load_pulse_eddy_csv(path: str) -> list:
    """从 CSV 文件加载脉冲涡流帧数据。
    返回 list[dict]: [{raw_values, ref_values, has_ref, sample_count, sample_rate_hz}, ...]
    """
    csv.field_size_limit(10 * 1024 * 1024)  # 10MB，脉冲涡流 base64 字段约 683KB
    frames = []
    with open(path, "r", encoding="utf-8") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row:
                continue
            if row[0].startswith("#"):
                continue
            if row[0].startswith("timestamp_ms_utc"):
                continue
            if len(row) < 9:
                continue
            try:
                raw_vals = _b64_to_doubles(row[4])
                has_ref = int(row[5]) != 0
                ref_vals = _b64_to_doubles(row[6]) if has_ref and row[6] else []
                frames.append({
                    "sample_count": int(row[2]),
                    "sample_rate_hz": float(row[3]),
                    "raw_values": raw_vals,
                    "has_ref": has_ref,
                    "ref_values": ref_vals,
                    "min_value": float(row[7]) if row[7] else 0.0,
                    "max_value": float(row[8]) if row[8] else 0.0,
                })
            except (ValueError, IndexError, struct.error, base64.binascii.Error):
                continue
    return frames


class PulseEddyGenerator:
    def __init__(self, noise=0.01, csv_frames=None):
        self.noise = noise
        self.csv_frames = csv_frames or []
        self._csv_idx = 0
        self.frame_index = 0
        self._lock = threading.Lock()
        self._running = False
        self._device_opened = False
        self._device_index = -1
        self._baudrate = 0

        # 参考线
        self._ref_target = 0
        self._ref_collected = 0
        self._ref_collecting = False
        self._ref_buffer = []  # 攒帧求均值

        if self.csv_frames:
            _log("CSV", f"已加载 {len(self.csv_frames)} 帧, 采样点={len(self.csv_frames[0]['raw_values']) if self.csv_frames else 0}")

    def list_devices(self):
        return [
            pe_pb2.DeviceInfo(index=0, description="脉冲涡流探头A", serial_number="PE-2024-001", device_type="PULSE_EDDY"),
            pe_pb2.DeviceInfo(index=1, description="脉冲涡流探头B", serial_number="PE-2024-002", device_type="PULSE_EDDY"),
        ]

    def open_device(self, device_index, baudrate):
        with self._lock:
            self._device_index = device_index
            self._baudrate = baudrate or 1000000
            self._device_opened = True
            return True, f"opened device {device_index}"

    def start_acquisition(self, device_index, baudrate, clear_reference):
        with self._lock:
            if not self._device_opened:
                return False, "device not opened"
            self._running = True
            if clear_reference:
                self.clear_reference()
            return True, f"acquiring on device {self._device_index}@{self._baudrate}"

    def stop_acquisition(self):
        with self._lock:
            self._running = False
            return True, "stopped"

    def close_device(self):
        with self._lock:
            self._device_opened = False
            return True, "closed"

    def start_reference_capture(self, frame_count):
        with self._lock:
            self._ref_target = frame_count
            self._ref_collected = 0
            self._ref_collecting = True
            self._ref_buffer.clear()
            return True, f"reference capture started, target={frame_count} frames"

    def clear_reference(self):
        with self._lock:
            self._ref_collecting = False
            self._ref_target = 0
            self._ref_collected = 0
            self._ref_buffer.clear()
            return True, "reference cleared"

    def status(self):
        with self._lock:
            return pe_pb2.PulseStatus(
                opened=self._device_opened,
                running=self._running,
                device_description=f"Probe {self._device_index}",
                serial_number=f"SIM-{self._device_index}",
                baudrate=self._baudrate,
                group=1,
                channel_count=1,
                sample_count_per_frame=SAMPLE_COUNT,
                sample_rate_hz=1000000,
                raw_data_type="double",
                frame_index=self.frame_index,
                has_reference=len(self._ref_buffer) > 0,
                reference_collecting=self._ref_collecting,
                reference_target_count=self._ref_target,
                reference_collected_count=self._ref_collected,
            )

    def next_frame(self):
        """生成一帧脉冲曲线。CSV 模式回放实际数据，否则生成模拟衰减正弦波。"""
        with self._lock:
            if not self._running:
                return None
            self.frame_index += 1
            fid = self.frame_index

        now_ms = int(time.time() * 1000)

        frame = pe_pb2.PulseFrame()
        frame.frame_index = fid
        frame.timestamp_unix_ms = now_ms

        # CSV 回放模式
        if self.csv_frames:
            data = self.csv_frames[self._csv_idx]
            self._csv_idx = (self._csv_idx + 1) % len(self.csv_frames)
            frame.sample_rate_hz = int(data["sample_rate_hz"])
            frame.sample_count = data["sample_count"]
            frame.raw_values.extend(data["raw_values"])
            raw = data["raw_values"]  # for reference
        else:
            frame.sample_rate_hz = 1000000
            frame.sample_count = SAMPLE_COUNT
            raw = []
            for i in range(SAMPLE_COUNT):
                x = i / SAMPLE_COUNT * 10.0
                val = (math.sin(x * 20.0 + fid * 0.1) * math.exp(-x * 0.8) * 5.0
                       + math.sin(x * 47.0 + fid * 0.07) * math.exp(-x * 1.5) * 2.0
                       + random.gauss(0, self.noise))
                raw.append(val)
            frame.raw_values.extend(raw)

        # 参考线采集
        with self._lock:
            if self._ref_collecting and self._ref_collected < self._ref_target:
                self._ref_buffer.append(raw)
                self._ref_collected += 1

            if self._ref_collecting and self._ref_collected >= self._ref_target:
                self._ref_collecting = False

            has_ref = len(self._ref_buffer) > 0
            frame.has_reference = has_ref
            frame.reference_collecting = self._ref_collecting
            frame.reference_target_count = self._ref_target
            frame.reference_collected_count = self._ref_collected

            if has_ref:
                # 计算均值参考线
                n = len(self._ref_buffer)
                ref = [0.0] * SAMPLE_COUNT
                for buf in self._ref_buffer:
                    for i in range(SAMPLE_COUNT):
                        ref[i] += buf[i]
                for i in range(SAMPLE_COUNT):
                    ref[i] /= n
                frame.reference_values.extend(ref)

        return frame


class PulseEddyServicer(pe_grpc.PulseEddyServicer):
    def __init__(self, generator, interval_ms=500):
        self.generator = generator
        self.interval_ms = max(1, interval_ms)
        self._stream_count = 0

    def ListDevices(self, request, context):
        _log("RPC", "ListDevices")
        reply = pe_pb2.ListDevicesResponse()
        devices = self.generator.list_devices()
        reply.devices.extend(devices)
        names = [f"Device {d.index}: {d.description}" for d in devices]
        _log("RPC", f"ListDevices 完成, 数量={len(devices)}, 设备列表={names}")
        return reply

    def OpenDevice(self, request, context):
        _log("RPC", f"OpenDevice index={request.device_index}, baudrate={request.baudrate}")
        ok, msg = self.generator.open_device(request.device_index, request.baudrate)
        _log("RPC", f"OpenDevice 返回 Ok={ok}, Message={msg}")
        return pe_pb2.OperationReply(success=ok, message=msg)

    def StartAcquisition(self, request, context):
        _log("RPC", f"StartAcquisition index={request.device_index}, baudrate={request.baudrate}, clear_ref={request.clear_reference}")
        ok, msg = self.generator.start_acquisition(
            request.device_index, request.baudrate, request.clear_reference)
        _log("RPC", f"StartAcquisition 返回 Ok={ok}, Message={msg}")
        return pe_pb2.OperationReply(success=ok, message=msg)

    def StopAcquisition(self, request, context):
        _log("RPC", "StopAcquisition")
        ok, msg = self.generator.stop_acquisition()
        _log("RPC", f"StopAcquisition 返回 Ok={ok}, Message={msg}")
        return pe_pb2.OperationReply(success=ok, message=msg)

    def CloseDevice(self, request, context):
        _log("RPC", "CloseDevice")
        ok, msg = self.generator.close_device()
        _log("RPC", f"CloseDevice 返回 Ok={ok}, Message={msg}")
        return pe_pb2.OperationReply(success=ok, message=msg)

    def GetStatus(self, request, context):
        return self.generator.status()

    def GetLatestFrame(self, request, context):
        frame = self.generator.next_frame()
        if frame:
            return frame
        return pe_pb2.PulseFrame()

    def StartReferenceCapture(self, request, context):
        _log("RPC", f"StartReferenceCapture frame_count={request.frame_count}")
        ok, msg = self.generator.start_reference_capture(request.frame_count)
        _log("RPC", f"StartReferenceCapture 返回 Ok={ok}, Message={msg}")
        return pe_pb2.OperationReply(success=ok, message=msg)

    def ClearReference(self, request, context):
        _log("RPC", "ClearReference")
        ok, msg = self.generator.clear_reference()
        _log("RPC", f"ClearReference 返回 Ok={ok}, Message={msg}")
        return pe_pb2.OperationReply(success=ok, message=msg)

    def StreamFrames(self, request, context):
        self._stream_count += 1
        stream_id = self._stream_count
        _log("RPC", f"StreamFrames#{stream_id} 开始推流, 间隔={self.interval_ms}ms")
        frame_count = 0
        interval = self.interval_ms / 1000.0
        try:
            while context.is_active():
                frame = self.generator.next_frame()
                if frame:
                    yield frame
                    frame_count += 1
                time.sleep(interval)
        finally:
            _log("RPC", f"StreamFrames#{stream_id} 结束, 共推送 {frame_count} 帧")


def main():
    parser = argparse.ArgumentParser(description="Mock gRPC server for pulse_eddy.proto")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50055)
    parser.add_argument("--interval", type=int, default=500, help="帧间隔 ms (默认 500ms ~2fps)")
    parser.add_argument("--noise", type=float, default=0.01)
    parser.add_argument("--csv", type=str,
                        default=os.path.join(SCRIPT_DIR, "build_cmake", "20260626",
                                             "pulse_eddy_db_export_20260626_120324.csv"),
                        help="CSV 数据文件路径（默认使用实际采集数据）")
    args = parser.parse_args()

    csv_frames = None
    csv_path = args.csv.strip().strip('"').strip("'") if args.csv else ""
    if csv_path:
        if not os.path.isabs(csv_path):
            csv_path = os.path.join(os.getcwd(), csv_path)
        if os.path.isfile(csv_path):
            csv_frames = load_pulse_eddy_csv(csv_path)
        else:
            _log("WARN", f"CSV 文件不存在，回退到纯模拟: {csv_path}")

    gen = PulseEddyGenerator(noise=args.noise, csv_frames=csv_frames)
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    pe_grpc.add_PulseEddyServicer_to_server(
        PulseEddyServicer(gen, interval_ms=args.interval), server)

    bind = f"{args.host}:{args.port}"
    server.add_insecure_port(bind)
    server.start()

    _log("SERVER", f"脉冲涡流gRPC服务已启动, 监听 {bind}")
    _log("SERVER", f"当前运行目录: {os.getcwd()}")
    _log("SERVER", f"采样点/帧={SAMPLE_COUNT} 帧间隔={args.interval}ms 噪声={args.noise}")
    _log("SERVER", f"ListDevices 返回: Device 0 (探头A) + Device 1 (探头B)")
    _log("SERVER", f"参考线: StartReferenceCapture(N) 攒 N 帧均值, ClearReference 清除")

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *a: stop.set())
    try:
        while not stop.is_set():
            time.sleep(0.2)
    finally:
        _log("SERVER", "服务正在停止...")
        server.stop(grace=1)
        _log("SERVER", "服务已停止")


if __name__ == "__main__":
    main()
