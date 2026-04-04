#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import csv
import math
import os
import random
import re
import signal
import sys
import threading
import time
from concurrent import futures

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
for candidate in [
    os.path.join(SCRIPT_DIR, "proto", "generated_py"),
    SCRIPT_DIR,
]:
    if os.path.isfile(os.path.join(candidate, "device_pb2.py")):
        if candidate not in sys.path:
            sys.path.insert(0, candidate)
        break

import grpc
from google.protobuf import empty_pb2
import device_pb2
import device_pb2_grpc


def _to_int(value, default_value=0):
    try:
        if value is None or value == "":
            return int(default_value)
        return int(float(value))
    except (ValueError, TypeError):
        return int(default_value)


def _to_float(value, default_value=0.0):
    try:
        if value is None or value == "":
            return float(default_value)
        return float(value)
    except (ValueError, TypeError):
        return float(default_value)


class CsvReplaySource:
    POS_AMP_PATTERN = re.compile(r"^Pos(\d+)_Amp$")

    def __init__(self, csv_path: str):
        self.csv_path = csv_path
        self._rows = []
        self._positions = []
        self._normalized_ts = []
        self._loop_span_ms = 1
        self._replay_base_unix_ms = None
        self._emit_counter = 0
        self._last_emit_unix_ms = 0
        self._load()

    @property
    def cell_count(self) -> int:
        return len(self._positions)

    @property
    def frame_count(self) -> int:
        return len(self._rows)

    def _load(self):
        with open(self.csv_path, "r", encoding="utf-8-sig", newline="") as f:
            reader = csv.DictReader(f)
            if not reader.fieldnames:
                raise RuntimeError(f"CSV header missing: {self.csv_path}")

            positions = []
            for name in reader.fieldnames:
                m = self.POS_AMP_PATTERN.match(name)
                if m:
                    positions.append(int(m.group(1)))
            positions.sort()

            if not positions:
                raise RuntimeError(f"CSV missing PosXX_Amp columns: {self.csv_path}")

            rows = [row for row in reader]
            if not rows:
                raise RuntimeError(f"CSV has no data rows: {self.csv_path}")

            self._positions = positions
            self._rows = rows

            source_ts = [_to_int(r.get("TimestampUnixMs"), 0) for r in rows]
            if any(ts > 0 for ts in source_ts):
                first_valid = next((ts for ts in source_ts if ts > 0), source_ts[0])
                raw_offsets = []
                for ts in source_ts:
                    if ts <= 0:
                        raw_offsets.append(None)
                    else:
                        raw_offsets.append(max(0, ts - first_valid))
            else:
                raw_offsets = [None for _ in rows]

            positive_deltas = []
            prev = None
            for ts in source_ts:
                if ts <= 0:
                    continue
                if prev is not None and ts > prev:
                    positive_deltas.append(ts - prev)
                prev = ts

            if positive_deltas:
                sorted_deltas = sorted(positive_deltas)
                step_ms = sorted_deltas[len(sorted_deltas) // 2]
            else:
                step_ms = 1

            # 关键：CSV 原始时间可能乱序/重复。这里把回放时间轴强制修正为严格单调递增，
            # 避免客户端按时间增量处理时出现“更新一段时间后停更”。
            normalized = []
            prev = -1
            for idx, raw in enumerate(raw_offsets):
                candidate = idx if raw is None else raw
                if candidate <= prev:
                    candidate = prev + max(1, step_ms)
                normalized.append(candidate)
                prev = candidate
            self._normalized_ts = normalized

            last_offset = self._normalized_ts[-1] if self._normalized_ts else 0
            self._loop_span_ms = max(1, last_offset + max(1, step_ms))

    def next_frame(self):
        if self._replay_base_unix_ms is None:
            self._replay_base_unix_ms = int(time.time() * 1000)

        row_count = len(self._rows)
        row_index = self._emit_counter % row_count
        loop_index = self._emit_counter // row_count
        row = self._rows[row_index]
        self._emit_counter += 1

        frame = device_pb2.ProcessedFrameReply()
        frame.sequence = self._emit_counter

        # CSV 模式下时间戳使用“实时单调时钟”而非 CSV 原始时间轴：
        # - 与 --no-csv 模式保持一致
        # - 避免 CSV 时间乱序/回绕导致客户端增量更新停滞
        now_ms = int(time.time() * 1000)
        if now_ms <= self._last_emit_unix_ms:
            now_ms = self._last_emit_unix_ms + 1
        self._last_emit_unix_ms = now_ms
        frame.timestamp_unix_ms = now_ms

        csv_cell_count = _to_int(row.get("CellCount"), len(self._positions))
        active_count = min(max(1, csv_cell_count), len(self._positions))
        frame.cell_count = active_count

        for display_index in range(active_count):
            pos = self._positions[display_index]
            prefix = f"Pos{pos:02d}"

            sample = frame.samples.add()
            sample.display_index = display_index
            sample.source_channel = _to_int(row.get(f"{prefix}_SourceChannel"), pos)
            sample.amp = _to_float(row.get(f"{prefix}_Amp"), 0.0)
            sample.phase = _to_float(row.get(f"{prefix}_Phase"), 0.0)
            sample.x = _to_float(row.get(f"{prefix}_X"), 0.0)
            sample.y = _to_float(row.get(f"{prefix}_Y"), 0.0)

        return frame


class FrameGenerator:
    def __init__(self, cell_count: int = 40, noise: float = 0.03, csv_path: str = ""):
        self.cell_count = max(1, min(cell_count, 512))
        self.noise = max(0.0, noise)
        self.sequence = 0
        self._opened = False
        self._sampling = False
        self._current_device_id = ""
        self._lock = threading.Lock()
        self._csv_source = None
        self._phase = [random.uniform(0.0, 2.0 * math.pi) for _ in range(512)]
        self._freq = [0.3 + i * 0.07 for i in range(512)]

        if csv_path:
            try:
                self._csv_source = CsvReplaySource(csv_path)
                self.cell_count = self._csv_source.cell_count
                print(
                    f"[grpc_test_server] CSV replay enabled: {csv_path} "
                    f"(frames={self._csv_source.frame_count}, cell_count={self.cell_count})"
                )
            except Exception as e:
                print(f"[grpc_test_server] CSV replay disabled, fallback to synthetic: {e}")

    def list_devices(self):
        display_name = "CSV Replay Device" if self._csv_source else "Mock Device 001"
        serial_number = "SN-CSV-001" if self._csv_source else "SN-MOCK-001"
        return [
            device_pb2.DeviceItem(
                device_id="mock-001",
                display_name=display_name,
                opened=self._opened,
                serial_number=serial_number,
            )
        ]

    def open_device(self, device_id: str):
        with self._lock:
            self._opened = True
            self._current_device_id = device_id or "mock-001"

    def close_device(self):
        with self._lock:
            self._opened = False
            self._sampling = False
            self._current_device_id = ""

    def start_sampling(self):
        with self._lock:
            if not self._opened:
                return False, "device not opened"
            self._sampling = True
            return True, "sampling started"

    def stop_sampling(self):
        with self._lock:
            self._sampling = False
            return True, "sampling stopped"

    def status(self):
        with self._lock:
            return self._opened, self._sampling, self._current_device_id, self.cell_count

    def next_frame(self):
        with self._lock:
            opened = self._opened
            sampling = self._sampling
            cell_count = self.cell_count
            self.sequence += 1
            sequence = self.sequence

        if not opened or not sampling:
            return None

        if self._csv_source:
            return self._csv_source.next_frame()

        now_ms = int(time.time() * 1000)
        t = now_ms / 1000.0

        frame = device_pb2.ProcessedFrameReply()
        frame.sequence = sequence
        frame.timestamp_unix_ms = now_ms
        frame.cell_count = cell_count

        for i in range(cell_count):
            angle = 2.0 * math.pi * self._freq[i] * t + self._phase[i]
            amp = 8.0 + 2.0 * math.sin(angle) + random.gauss(0.0, self.noise)
            phase = math.atan2(math.sin(angle), math.cos(angle)) + random.gauss(0.0, self.noise * 0.2)
            x = math.cos(angle) + random.gauss(0.0, self.noise)
            y = math.sin(angle) + random.gauss(0.0, self.noise)

            s = frame.samples.add()
            s.display_index = i
            s.source_channel = i
            s.amp = float(amp)
            s.phase = float(phase)
            s.x = float(x)
            s.y = float(y)

        return frame


class AcquisitionDeviceServicer(device_pb2_grpc.AcquisitionDeviceServicer):
    def __init__(self, generator: FrameGenerator, interval_ms: int = 100):
        self.generator = generator
        self.interval_ms = max(10, interval_ms)

    def ListDevices(self, request, context):
        reply = device_pb2.ListDevicesReply()
        reply.devices.extend(self.generator.list_devices())
        return reply

    def OpenDevice(self, request, context):
        device_id = request.device_id or "mock-001"
        self.generator.open_device(device_id)
        return device_pb2.CommandReply(ok=True, message=f"opened {device_id}")

    def CloseDevice(self, request, context):
        self.generator.close_device()
        return device_pb2.CommandReply(ok=True, message="closed")

    def StartSampling(self, request, context):
        ok, msg = self.generator.start_sampling()
        return device_pb2.CommandReply(ok=ok, message=msg)

    def StopSampling(self, request, context):
        ok, msg = self.generator.stop_sampling()
        return device_pb2.CommandReply(ok=ok, message=msg)

    def ResetDevice(self, request, context):
        self.generator.close_device()
        self.generator.open_device("mock-001")
        return device_pb2.CommandReply(ok=True, message="reset")

    def GetStatus(self, request, context):
        opened, sampling, current_id, cell_count = self.generator.status()
        return device_pb2.StatusReply(
            opened=opened,
            sampling=sampling,
            current_device_id=current_id,
            cell_count=cell_count,
        )

    def SubscribeProcessedFrames(self, request, context):
        interval_sec = self.interval_ms / 1000.0
        while context.is_active():
            frame = self.generator.next_frame()
            if frame is not None:
                yield frame
            time.sleep(interval_sec)


def parse_args():
    default_csv = os.path.join(SCRIPT_DIR, "proto", "display_aligned_20260327_171739.csv")
    parser = argparse.ArgumentParser(description="Mock gRPC server for device.proto AcquisitionDevice")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=50051)
    parser.add_argument("--cells", type=int, default=40)
    parser.add_argument("--interval", type=int, default=100)
    parser.add_argument("--noise", type=float, default=0.03)
    parser.add_argument("--csv", default=default_csv, help="CSV replay source path")
    parser.add_argument("--no-csv", action="store_true", help="Disable CSV replay and use synthetic data")
    return parser.parse_args()


def main():
    args = parse_args()

    csv_path = "" if args.no_csv else args.csv
    generator = FrameGenerator(cell_count=args.cells, noise=args.noise, csv_path=csv_path)
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    device_pb2_grpc.add_AcquisitionDeviceServicer_to_server(
        AcquisitionDeviceServicer(generator, interval_ms=args.interval), server
    )

    bind_addr = f"{args.host}:{args.port}"
    server.add_insecure_port(bind_addr)
    server.start()

    print(f"[grpc_test_server] AcquisitionDevice listening on {bind_addr}")
    print("[grpc_test_server] methods: ListDevices/OpenDevice/StartSampling/SubscribeProcessedFrames")

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
