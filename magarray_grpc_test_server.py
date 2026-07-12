#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
漏磁检测 (MagArray) gRPC 测试服务器
用法: python magarray_grpc_test_server.py [--port 50054]
"""

import argparse, csv, math, os, random, signal, sys, threading, time
from collections import OrderedDict
from concurrent import futures
from datetime import datetime

if getattr(sys, "frozen", False) and hasattr(sys, "_MEIPASS"):
    SCRIPT_DIR = sys._MEIPASS
else:
    SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for candidate in [os.path.join(SCRIPT_DIR, "proto", "generated_py"), SCRIPT_DIR]:
    if os.path.isfile(os.path.join(candidate, "mag_array_pb2.py")):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
        break

import grpc
from google.protobuf import empty_pb2
import mag_array_pb2 as ma_pb2
import mag_array_pb2_grpc as ma_grpc

SENSOR_COUNT = 20
AXIS_COUNT = 3
CHANNEL_COUNT = SENSOR_COUNT * AXIS_COUNT  # 60

# ---- 日志工具 ----
_LOG_LOCK = threading.Lock()

def _log(prefix, msg):
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    with _LOG_LOCK:
        print(f"{ts}[{prefix}]{msg}", flush=True)


# ---- CSV 数据加载 ----

def load_magarray_csv(path: str) -> list:
    """从 CSV 文件加载漏磁检测帧数据，按 frame_index 分组。
    返回 list[list[dict]]: 每个元素是一帧的传感器数据列表（sensor_index 0-19）。
    """
    csv.field_size_limit(10 * 1024 * 1024)
    frames_map = OrderedDict()
    with open(path, "r", encoding="utf-8") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row:
                continue
            if row[0].startswith("#"):
                continue
            if row[0].startswith("timestamp_ms_utc"):
                continue
            if len(row) < 14:
                continue
            try:
                fi = int(row[1])
                si = int(row[2])
                sensor = {
                    "sensor_index": si,
                    "x_mean": float(row[3]) if row[3] else 0.0,
                    "y_mean": float(row[4]) if row[4] else 0.0,
                    "z_mean": float(row[5]) if row[5] else 0.0,
                    "x_latest": float(row[6]) if row[6] else 0.0,
                    "y_latest": float(row[7]) if row[7] else 0.0,
                    "z_latest": float(row[8]) if row[8] else 0.0,
                    "magnitude_mean": float(row[9]) if row[9] else 0.0,
                    "magnitude_latest": float(row[10]) if row[10] else 0.0,
                    "processed_value_x": float(row[11]) if row[11] else 0.0,
                    "processed_value_y": float(row[12]) if row[12] else 0.0,
                    "processed_value_z": float(row[13]) if row[13] else 0.0,
                }
            except (ValueError, IndexError):
                continue
            if fi not in frames_map:
                frames_map[fi] = []
            frames_map[fi].append(sensor)
    # 转换为帧列表（保持 frame_index 顺序）
    return list(frames_map.values())


class MagArrayGenerator:
    def __init__(self, noise=0.02, csv_frames=None):
        self.noise = noise
        self.csv_frames = csv_frames or []
        self._csv_idx = 0
        self.frame_index = 0
        self._lock = threading.Lock()
        if self.csv_frames:
            _log("CSV", f"已加载 {len(self.csv_frames)} 帧, 每帧 {len(self.csv_frames[0]) if self.csv_frames else 0} 传感器")
        self._running = False
        self._port_name = ""
        self._baudrate = 0
        self._preprocess_mode = ma_pb2.PREPROCESS_MODE_UNSPECIFIED
        self._phase = [random.uniform(0, 2 * math.pi) for _ in range(CHANNEL_COUNT)]
        self._freq = [0.3 + (i % SENSOR_COUNT) * 0.05 for i in range(CHANNEL_COUNT)]
        self._amp = [1.5 + 0.5 * math.sin(i * 0.3) for i in range(CHANNEL_COUNT)]

    def list_serial_ports(self):
        # 模拟返回 COM1 到 COM5
        return [ma_pb2.SerialPortInfo(name=f"COM{i}") for i in range(1, 6)]

    def start_detection(self, port_name, baudrate, preprocess):
        with self._lock:
            # 校验：串口号不能为空
            if not port_name or not port_name.strip():
                _log("RPC", f"StartDetection Port=, Baudrate={baudrate}")
                return False, "串口号不能为空，请先选择设备串口"

            self._port_name = port_name.strip()
            self._baudrate = baudrate or 1000000
            self._preprocess_mode = preprocess.mode if preprocess else ma_pb2.PREPROCESS_MODE_UNSPECIFIED
            self._running = True

            mode_name = ma_pb2.PreprocessMode.Name(self._preprocess_mode)
            _log("RPC", f"StartDetection Port={self._port_name}, Baudrate={self._baudrate}, "
                        f"PreprocessMode={mode_name}, BaselineFrames={preprocess.baseline_frames}, "
                        f"FixedMidpoint={preprocess.fixed_midpoint}, TrackingFactor={preprocess.tracking_factor}")
            return True, f"started on {self._port_name}@{self._baudrate}, mode={mode_name}"

    def stop_detection(self):
        with self._lock:
            if not self._running:
                return False, "漏磁检测未启动，无需停止"
            prev_port = self._port_name
            self._running = False
            self._port_name = ""
            return True, f"已停止 {prev_port} 检测"

    def status(self):
        with self._lock:
            return ma_pb2.RuntimeStatus(
                running=self._running,
                port_name=self._port_name,
                baudrate=self._baudrate,
                sensor_count=SENSOR_COUNT,
                axis_count=AXIS_COUNT,
                channel_count=CHANNEL_COUNT,
                samples_per_channel=1000,
                data_group=1,
                queue_capacity=1024,
                max_queue_len=32,
                overrun_count=0,
                frame_index=self.frame_index,
                baseline_ready=True,
                baseline_frame_count=50,
                baseline_frames=50,
                preprocess_mode=self._preprocess_mode,
            )

    def next_frame(self):
        with self._lock:
            if not self._running:
                return None
            self.frame_index += 1
            fid = self.frame_index

        now_ms = int(time.time() * 1000)

        frame = ma_pb2.MagArrayFrame()
        frame.timestamp_unix_ms = now_ms
        frame.frame_index = fid
        frame.sensor_count = SENSOR_COUNT
        frame.axis_count = AXIS_COUNT
        frame.channel_count = CHANNEL_COUNT
        frame.samples_per_channel = 1000

        # CSV 回放模式：从实际数据填充 sensor_results
        if self.csv_frames:
            sensors = self.csv_frames[self._csv_idx]
            self._csv_idx = (self._csv_idx + 1) % len(self.csv_frames)
            for s in sensors:
                sr = frame.sensor_results.add()
                sr.sensor_index = s["sensor_index"]
                sr.x_mean = s["x_mean"]
                sr.y_mean = s["y_mean"]
                sr.z_mean = s["z_mean"]
                sr.x_latest = s["x_latest"]
                sr.y_latest = s["y_latest"]
                sr.z_latest = s["z_latest"]
                sr.magnitude_mean = s["magnitude_mean"]
                sr.magnitude_latest = s["magnitude_latest"]
            # 同时填充 channels（兼容旧客户端）
            for sensor_idx in range(SENSOR_COUNT):
                if sensor_idx < len(sensors):
                    s = sensors[sensor_idx]
                    for axis_idx in range(AXIS_COUNT):
                        ch_idx = sensor_idx * AXIS_COUNT + axis_idx
                        ch = frame.channels.add()
                        ch.channel_index = ch_idx
                        ch.sensor_index = sensor_idx
                        ch.axis_index = axis_idx
                        ch.axis_name = "XYZ"[axis_idx]
                        ch.baseline = 32768.0
                        axis_key = ["x_mean","y_mean","z_mean"][axis_idx]
                        vals = [s[axis_key] for _ in range(50)]
                        ch.processed_values.extend(vals)
                        ch.raw_mean = s[axis_key] + 32768.0
                        ch.processed_mean = s[axis_key]
            frame.status.CopyFrom(self.status())
            return frame

        # 纯模拟模式（原有逻辑）
        t = time.time()
        sensor_data = [[0.0]*3 for _ in range(SENSOR_COUNT)]

        for sensor_idx in range(SENSOR_COUNT):
            for axis_idx in range(AXIS_COUNT):
                ch_idx = sensor_idx * AXIS_COUNT + axis_idx
                ch = frame.channels.add()
                ch.channel_index = ch_idx
                ch.sensor_index = sensor_idx
                ch.axis_index = axis_idx
                ch.axis_name = "XYZ"[axis_idx]
                ch.baseline = 32768.0
                angle = 2 * math.pi * self._freq[ch_idx] * t + self._phase[ch_idx]
                vals = [self._amp[ch_idx] * math.sin(angle + i * 0.02) + random.gauss(0, self.noise) for i in range(50)]
                ch.processed_values.extend(vals)
                ch.raw_mean = float(sum(vals)) / len(vals) + 32768.0
                ch.processed_mean = float(sum(vals)) / len(vals)
                sensor_data[sensor_idx][axis_idx] = ch.processed_mean

        for sensor_idx in range(SENSOR_COUNT):
            sr = frame.sensor_results.add()
            sr.sensor_index = sensor_idx
            sr.x_mean = sensor_data[sensor_idx][0]
            sr.y_mean = sensor_data[sensor_idx][1]
            sr.z_mean = sensor_data[sensor_idx][2]
            sr.x_latest = sr.x_mean + random.gauss(0, self.noise * 0.5)
            sr.y_latest = sr.y_mean + random.gauss(0, self.noise * 0.5)
            sr.z_latest = sr.z_mean + random.gauss(0, self.noise * 0.5)
            sr.magnitude_mean = math.sqrt(sum(v*v for v in sensor_data[sensor_idx]))
            sr.magnitude_latest = sr.magnitude_mean + random.gauss(0, self.noise * 0.3)

        frame.status.CopyFrom(self.status())
        return frame


class MagArrayServicer(ma_grpc.MagArrayLeakageDetectionServicer):
    def __init__(self, generator, interval_ms=100):
        self.generator = generator
        self.interval_ms = max(1, interval_ms)
        self._stream_count = 0

    def ListSerialPorts(self, request, context):
        _log("RPC", "ListSerialPorts")
        reply = ma_pb2.ListSerialPortsResponse()
        ports = self.generator.list_serial_ports()
        reply.ports.extend(ports)
        names = [p.name for p in ports]
        _log("RPC", f"ListSerialPorts 完成, 数量={len(ports)}, 端口列表={names}")
        return reply

    def StartDetection(self, request, context):
        # generator.start_detection 内部已打印完整参数日志，此处不再重复
        ok, msg = self.generator.start_detection(
            request.port_name, request.baudrate, request.preprocess)
        _log("RPC", f"StartDetection 返回 Ok={ok}, Message={msg}")
        return ma_pb2.OperationReply(ok=ok, message=msg)

    def StopDetection(self, request, context):
        _log("RPC", "StopDetection")
        ok, msg = self.generator.stop_detection()
        _log("RPC", f"StopDetection 返回 Ok={ok}, Message={msg}")
        return ma_pb2.OperationReply(ok=ok, message=msg)

    def GetStatus(self, request, context):
        _log("RPC", "GetStatus")
        return self.generator.status()

    def GetLatestFrame(self, request, context):
        frame = self.generator.next_frame()
        if frame:
            return frame
        return ma_pb2.MagArrayFrame()

    def ResetBaseline(self, request, context):
        _log("RPC", "ResetBaseline")
        return ma_pb2.OperationReply(ok=True, message="baseline reset")

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
    parser = argparse.ArgumentParser(description="Mock gRPC server for mag_array.proto")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50054)
    parser.add_argument("--interval", type=int, default=100, help="帧间隔 ms")
    parser.add_argument("--noise", type=float, default=0.02)
    parser.add_argument("--csv", type=str,
                        default=os.path.join(SCRIPT_DIR, "build_cmake", "20260626",
                                             "mag_array_db_export_20260626_113816.csv"),
                        help="CSV 数据文件路径（默认使用实际采集数据）")
    args = parser.parse_args()

    csv_frames = None
    csv_path = args.csv.strip().strip('"').strip("'") if args.csv else ""
    if csv_path:
        if not os.path.isabs(csv_path):
            csv_path = os.path.join(os.getcwd(), csv_path)
        if os.path.isfile(csv_path):
            csv_frames = load_magarray_csv(csv_path)
        else:
            _log("WARN", f"CSV 文件不存在，回退到纯模拟: {csv_path}")

    gen = MagArrayGenerator(noise=args.noise, csv_frames=csv_frames)
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    ma_grpc.add_MagArrayLeakageDetectionServicer_to_server(
        MagArrayServicer(gen, interval_ms=args.interval), server)

    bind = f"{args.host}:{args.port}"
    port = server.add_insecure_port(bind)
    server.start()

    _log("SERVER", f"漏磁检测gRPC服务已启动, 监听 {bind}")
    _log("SERVER", f"当前运行目录: {os.getcwd()}")
    _log("SERVER", f"传感器数={SENSOR_COUNT} 轴数={AXIS_COUNT} 通道数={CHANNEL_COUNT}")
    _log("SERVER", f"帧间隔={args.interval}ms 噪声={args.noise}")
    _log("SERVER", f"ListSerialPorts 返回: COM1~COM5")
    _log("SERVER", f"StartDetection 必须提供非空串口号, 否则返回 Ok=False")

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
