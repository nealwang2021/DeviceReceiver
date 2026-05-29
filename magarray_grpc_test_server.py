#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
漏磁检测 (MagArray) gRPC 测试服务器
用法: python magarray_grpc_test_server.py [--port 50054]
"""

import argparse, math, os, random, signal, sys, threading, time
from concurrent import futures

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


class MagArrayGenerator:
    def __init__(self, noise=0.02):
        self.noise = noise
        self.frame_index = 0
        self._lock = threading.Lock()
        self._running = False
        self._port_name = ""
        self._baudrate = 0
        self._phase = [random.uniform(0, 2 * math.pi) for _ in range(CHANNEL_COUNT)]
        self._freq = [0.3 + (i % SENSOR_COUNT) * 0.05 for i in range(CHANNEL_COUNT)]
        self._amp = [1.5 + 0.5 * math.sin(i * 0.3) for i in range(CHANNEL_COUNT)]

    def list_serial_ports(self):
        return [ma_pb2.SerialPortInfo(name="COM3")]

    def start_detection(self, port_name, baudrate, preprocess):
        with self._lock:
            self._port_name = port_name or "COM3"
            self._baudrate = baudrate or 1000000
            self._running = True
            return True, f"started on {self._port_name}@{self._baudrate}"

    def stop_detection(self):
        with self._lock:
            self._running = False
            return True, "stopped"

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
                preprocess_mode=ma_pb2.PREPROCESS_MODE_AUTO_BASELINE,
            )

    def next_frame(self):
        with self._lock:
            if not self._running:
                return None
            self.frame_index += 1
            fid = self.frame_index

        now_ms = int(time.time() * 1000)
        t = time.time()

        frame = ma_pb2.MagArrayFrame()
        frame.timestamp_unix_ms = now_ms
        frame.frame_index = fid
        frame.sensor_count = SENSOR_COUNT
        frame.axis_count = AXIS_COUNT
        frame.channel_count = CHANNEL_COUNT
        frame.samples_per_channel = 1000

        sensor_data = [[0.0]*3 for _ in range(SENSOR_COUNT)]  # [sensor][axis]

        for sensor_idx in range(SENSOR_COUNT):
            for axis_idx in range(AXIS_COUNT):
                ch_idx = sensor_idx * AXIS_COUNT + axis_idx
                ch = frame.channels.add()
                ch.channel_index = ch_idx
                ch.sensor_index = sensor_idx
                ch.axis_index = axis_idx
                ch.axis_name = "XYZ"[axis_idx]
                ch.baseline = 32768.0
                # 生成 ~50 个采样点的 processed_values
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

    def ListSerialPorts(self, request, context):
        reply = ma_pb2.ListSerialPortsResponse()
        reply.ports.extend(self.generator.list_serial_ports())
        return reply

    def StartDetection(self, request, context):
        ok, msg = self.generator.start_detection(
            request.port_name, request.baudrate, request.preprocess)
        return ma_pb2.OperationReply(ok=ok, message=msg)

    def StopDetection(self, request, context):
        ok, msg = self.generator.stop_detection()
        return ma_pb2.OperationReply(ok=ok, message=msg)

    def GetStatus(self, request, context):
        return self.generator.status()

    def GetLatestFrame(self, request, context):
        frame = self.generator.next_frame()
        if frame:
            return frame
        return ma_pb2.MagArrayFrame()

    def StreamFrames(self, request, context):
        interval = self.interval_ms / 1000.0
        while context.is_active():
            frame = self.generator.next_frame()
            if frame:
                yield frame
            time.sleep(interval)


def main():
    parser = argparse.ArgumentParser(description="Mock gRPC server for mag_array.proto")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50054)
    parser.add_argument("--interval", type=int, default=100, help="帧间隔 ms")
    parser.add_argument("--noise", type=float, default=0.02)
    args = parser.parse_args()

    gen = MagArrayGenerator(noise=args.noise)
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    ma_grpc.add_MagArrayLeakageDetectionServicer_to_server(
        MagArrayServicer(gen, interval_ms=args.interval), server)

    bind = f"{args.host}:{args.port}"
    server.add_insecure_port(bind)
    server.start()
    print(f"[magarray_test_server] listening on {bind}")
    print(f"  frame_interval={args.interval}ms noise={args.noise}")
    print(f"  sensors={SENSOR_COUNT} axes={AXIS_COUNT} channels={CHANNEL_COUNT}")

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *a: stop.set())
    try:
        while not stop.is_set():
            time.sleep(0.2)
    finally:
        server.stop(grace=1)


if __name__ == "__main__":
    main()
