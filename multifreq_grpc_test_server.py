#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# multifreq_grpc_test_server.py — 多频涡流 gRPC 模拟服务
# =========================================================
# 作用：
# - 基于 proto/multifreq_eddy.proto 提供 MultiFreqEddyCurrent gRPC 模拟服务
# - 生成含多频点阻抗数据的 DetectionFrame（纯模拟，无需外部数据源）
# - 配合主程序测试 GrpcMultiFreqBackend
#
# 数据周期与帧率：
#   基频 100Hz → 单周期 10ms，平均周期数 10 → 采集块 100ms。
#   服务端以独立线程持续生成帧，默认 100fps（10ms 间隔），
#   每帧携带滑动窗口内的最新频点计算结果。
#
# 常用启动：
#   python multifreq_grpc_test_server.py
#   python multifreq_grpc_test_server.py --port 50051
#   python multifreq_grpc_test_server.py --base-freq 100 --factors 1,2,4,8 --fps 100
#   python multifreq_grpc_test_server.py --base-freq 50 --avg-cycle 10 --fps 50
#
# 参数：
#   --host / --port          监听地址（默认 0.0.0.0:50051）
#   --base-freq              基频 Hz（默认 100，可选 1|2|5|10|20|50|100|200|500|1000）
#   --avg-cycle              平均周期数（默认 10）
#   --norm-scale             归一化系数（默认 1.0）
#   --factors                逗号分隔倍频系数（默认 "1,2,4,8"）
#   --fps / --interval       帧率 fps（默认 100）或帧间隔 ms；--fps 优先
#   --noise                  阻抗噪声幅度（默认 0.005）
#

import argparse
import csv
import logging
import math
import os
import random
import signal
import sys
import threading
import time
from collections import OrderedDict
from concurrent import futures
from dataclasses import dataclass, field

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

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

VALID_BASE_FREQUENCIES: set = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000}
DEFAULT_FACTORS: list = [1, 2, 4, 8]

logger = logging.getLogger("multifreq-grpc-server")


def _base_freq_enum(hz: int):
    """将整数 Hz 映射到 proto BaseFrequency 枚举值。"""
    name = f"BASE_FREQUENCY_HZ_{hz}"
    try:
        return mf_pb2.BaseFrequency.Value(name)
    except ValueError:
        return mf_pb2.BASE_FREQUENCY_HZ_100


# ---------------------------------------------------------------------------
# CSV 数据加载器
# ---------------------------------------------------------------------------

def load_multifreq_csv(path: str) -> list:
    """从 CSV 文件加载多频涡流帧数据，按 frame_index 分组。
    返回 list[dict]: frame_index → {frequency_factor: {col: value}, ...}
    """
    csv.field_size_limit(10 * 1024 * 1024)
    frames = OrderedDict()
    with open(path, "r", encoding="utf-8") as f:
        reader = csv.reader(f)
        for row in reader:
            if not row:
                continue
            # 跳过元数据头
            if row[0].startswith("#"):
                continue
            # 跳过列标题行
            if row[0].startswith("timestamp_ms_utc"):
                continue
            if len(row) < 12:
                continue
            try:
                fi = int(row[1])
                factor = int(row[2])
                pt = {
                    "frequency_factor": factor,
                    "frequency_hz": float(row[3]),
                    "impedance_real": float(row[4]),
                    "impedance_imag": float(row[5]),
                    "impedance_magnitude": float(row[6]),
                    "impedance_phase_deg": float(row[7]),
                    "normalized_impedance_real": float(row[8]),
                    "normalized_impedance_imag": float(row[9]),
                    "voltage_magnitude": float(row[10]),
                    "current_magnitude": float(row[11]),
                }
            except (ValueError, IndexError):
                continue
            if fi not in frames:
                frames[fi] = []
            frames[fi].append(pt)
    return list(frames.values())  # list of list-of-points, in CSV order


# ---------------------------------------------------------------------------
# 多频涡流模拟数据生成器
# ---------------------------------------------------------------------------

@dataclass
class MultiFreqConfig:
    """可运行时更新的多频采集配置。"""
    base_freq_hz: int = 100
    avg_cycle_count: int = 10
    norm_scale: float = 1.0
    factors: list = field(default_factory=lambda: [1, 2, 4, 8])
    noise: float = 0.005

    def apply_start_detection(self, config) -> list:
        """将 StartDetectionRequest.config 写入并返回变更字段列表。"""
        changed = []
        if config.base_frequency and config.base_frequency != mf_pb2.BASE_FREQUENCY_UNSPECIFIED:
            new_hz = self.base_freq_hz
            for hz in sorted(VALID_BASE_FREQUENCIES):
                if _base_freq_enum(hz) == config.base_frequency:
                    new_hz = hz
                    break
            if new_hz != self.base_freq_hz:
                self.base_freq_hz = new_hz
                changed.append(f"base_freq={new_hz}Hz")
        if config.average_cycle_count > 0 and config.average_cycle_count != self.avg_cycle_count:
            self.avg_cycle_count = config.average_cycle_count
            changed.append(f"avg_cycle={self.avg_cycle_count}")
        if config.normalize_scale > 0 and abs(config.normalize_scale - self.norm_scale) > 1e-9:
            self.norm_scale = config.normalize_scale
            changed.append(f"norm_scale={self.norm_scale:.3f}")
        if config.frequency_factors:
            new_factors = list(config.frequency_factors)
            if new_factors != self.factors:
                self.factors = new_factors
                changed.append(f"factors={self.factors}")
        return changed

    @property
    def sample_rate_hz(self) -> int:
        """采样率 = 基频 × 最高倍频 × 平均周期数 × 4（每周期 4 点）。"""
        max_factor = max(self.factors) if self.factors else 1
        return self.base_freq_hz * max_factor * self.avg_cycle_count * 4

    @property
    def samples_per_frame(self) -> int:
        """每帧采样数，保证至少覆盖基频一个完整周期。"""
        min_factor = min(self.factors) if self.factors else 1
        return max(256, self.sample_rate_hz // (self.base_freq_hz * min_factor))

    @property
    def cycle_duration_ms(self) -> float:
        """单采集块时长（ms）= 平均周期数 / 基频 × 1000。"""
        return self.avg_cycle_count / self.base_freq_hz * 1000.0


class MultiFreqGenerator:
    """多频涡流模拟数据生成器（支持 CSV 回放和纯模拟）。"""

    def __init__(self, config: MultiFreqConfig, csv_frames: list = None):
        self.cfg = config
        self.csv_frames = csv_frames or []  # list[list[dict]]: 每个元素是一帧的频率点列表
        self._csv_idx = 0  # CSV 回放当前位置
        self.frame_index: int = 0
        self._lock = threading.Lock()
        self._running: bool = False
        self._selected_serial: str = ""
        self._selected_desc: str = ""
        self._start_time: float = 0.0
        if self.csv_frames:
            logger.info("CSV 数据已加载: %d 帧, 每帧 %d 频率点",
                        len(self.csv_frames),
                        len(self.csv_frames[0]) if self.csv_frames else 0)

    # ---- 设备管理 ----

    def list_devices(self):
        return [
            mf_pb2.DeviceInfo(
                index=0,
                description="多频涡流 Mock Device 001",
                serial_number="MF-MOCK-001",
                type="multi_freq_eddy_current",
            )
        ]

    def start_detection(self, serial: str, device_index: int, config) -> tuple:
        changed = self.cfg.apply_start_detection(config)
        with self._lock:
            self._running = True
            self._selected_serial = serial or "MF-MOCK-001"
            self._selected_desc = (
                f"多频涡流 BaseFreq={self.cfg.base_freq_hz}Hz "
                f"Cycles={self.cfg.avg_cycle_count} "
                f"Factors={self.cfg.factors}"
            )
            self._start_time = time.time()
        detail = "; ".join(changed) if changed else "unchanged"
        msg = f"started: {self._selected_desc}  ({detail})"
        logger.info("StartDetection: %s", msg)
        return True, msg

    def stop_detection(self) -> tuple:
        with self._lock:
            was_running = self._running
            self._running = False
        logger.info("StopDetection: was_running=%s", was_running)
        return True, "stopped" if was_running else "already stopped"

    def status(self):
        with self._lock:
            return mf_pb2.RuntimeStatus(
                running=self._running,
                selected_device_serial_number=self._selected_serial,
                selected_device_description=self._selected_desc,
                base_frequency_hz=self.cfg.base_freq_hz,
                sample_rate_hz=self.cfg.sample_rate_hz,
                sample_count_per_frame=self.cfg.samples_per_frame,
                average_cycle_count=self.cfg.avg_cycle_count,
                queue_capacity=1024,
                max_queue_len=64,
                overrun_count=0,
                frame_index=self.frame_index,
            )

    # ---- 帧生成 ----

    def _build_csv_frame(self, fid: int, points: list) -> "mf_pb2.DetectionFrame":
        """用 CSV 数据构建一帧（不填充 curve/spectrum 通道）。"""
        now_ms = int(time.time() * 1000)
        frame = mf_pb2.DetectionFrame()
        frame.timestamp_unix_ms = now_ms
        frame.frame_index = fid
        frame.base_frequency_hz = self.cfg.base_freq_hz
        frame.sample_rate_hz = self.cfg.sample_rate_hz
        frame.sample_count_per_frame = self.cfg.samples_per_frame

        for pt_data in points:
            pt = frame.point_results.add()
            pt.frequency_factor = pt_data["frequency_factor"]
            pt.frequency_hz = pt_data["frequency_hz"]
            pt.impedance_real = pt_data["impedance_real"]
            pt.impedance_imag = pt_data["impedance_imag"]
            pt.impedance_magnitude = pt_data["impedance_magnitude"]
            pt.impedance_phase_deg = pt_data["impedance_phase_deg"]
            pt.normalized_impedance_real = pt_data["normalized_impedance_real"]
            pt.normalized_impedance_imag = pt_data["normalized_impedance_imag"]
            pt.voltage_magnitude = pt_data["voltage_magnitude"]
            pt.current_magnitude = pt_data["current_magnitude"]
            pt.valid = True

        frame.status.CopyFrom(self.status())
        return frame

    def next_frame(self):
        """生成下一帧 DetectionFrame。若未启动则返回 None。"""
        with self._lock:
            if not self._running:
                return None
            self.frame_index += 1
            fid = self.frame_index
            bf = self.cfg.base_freq_hz
            sr = self.cfg.sample_rate_hz
            spf = self.cfg.samples_per_frame

        # CSV 回放模式
        if self.csv_frames:
            points = self.csv_frames[self._csv_idx]
            self._csv_idx = (self._csv_idx + 1) % len(self.csv_frames)
            frame = self._build_csv_frame(fid, points)
            # status 在 _build_csv_frame 内部已通过 self.status() 设置，无需重复
            return frame

        # 纯模拟模式（原有逻辑）
        factors = list(self.cfg.factors)
        noise = self.cfg.noise

        now_ms = int(time.time() * 1000)
        elapsed = time.time() - self._start_time

        frame = mf_pb2.DetectionFrame()
        frame.timestamp_unix_ms = now_ms
        frame.frame_index = fid
        frame.base_frequency_hz = bf
        frame.sample_rate_hz = sr
        frame.sample_count_per_frame = spf

        for factor in factors:
            freq_hz = float(bf * factor)
            pt = frame.point_results.add()
            pt.frequency_factor = factor
            pt.frequency_hz = freq_hz

            base_mag = 0.03 / math.sqrt(factor)
            phase_base = (factor - 1) * 25.0 + random.gauss(0, 5.0)
            noise_mag = random.gauss(0, noise)
            noise_phase = random.gauss(0, noise * 0.3)

            mag = max(1e-12, base_mag + noise_mag)
            phase_deg = phase_base + noise_phase + 180.0 * math.sin(elapsed * 2.0 * math.pi * 0.1)
            phase_rad = math.radians(phase_deg)

            z_real = mag * math.cos(phase_rad)
            z_imag = mag * math.sin(phase_rad)

            pt.impedance_real = z_real
            pt.impedance_imag = z_imag
            pt.impedance_magnitude = mag
            pt.impedance_phase_deg = phase_deg

            denom = 2.0 * math.pi * freq_hz
            pt.normalized_impedance_real = (z_real / denom) * 1e6 if denom > 1e-12 else 0.0
            pt.normalized_impedance_imag = (z_imag / denom) * 1e6 if denom > 1e-12 else 0.0

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


# ---------------------------------------------------------------------------
# gRPC 服务实现
# ---------------------------------------------------------------------------

class MultiFreqEddyCurrentServicer(mf_grpc.MultiFreqEddyCurrentServicer):
    def __init__(self, generator: MultiFreqGenerator, frame_interval_sec: float = 0.01):
        self.generator = generator
        self.interval_sec = max(0.001, frame_interval_sec)
        self._stream_count = 0

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
        self._stream_count += 1
        stream_id = self._stream_count
        logger.info("StreamFrames#%d 开始推流, 间隔=%.1fms", stream_id, self.interval_sec * 1000)
        frame_count = 0
        none_count = 0
        try:
            while context.is_active():
                frame = self.generator.next_frame()
                if frame is not None:
                    yield frame
                    frame_count += 1
                    if frame_count % 100 == 1:
                        logger.info("StreamFrames#%d 已推送 %d 帧 (frame_index=%d, pts=%d)",
                                    stream_id, frame_count, frame.frame_index, len(frame.point_results))
                else:
                    none_count += 1
                    if none_count == 1:
                        logger.warning("StreamFrames#%d next_frame() 返回 None! (第1次)", stream_id)
                    elif none_count % 500 == 0:
                        logger.warning("StreamFrames#%d next_frame() 返回 None (第%d次)",
                                       stream_id, none_count)
                time.sleep(self.interval_sec)
        finally:
            logger.info("StreamFrames#%d 结束, 共推送 %d 帧, None=%d 次",
                        stream_id, frame_count, none_count)


# ---------------------------------------------------------------------------
# 命令行
# ---------------------------------------------------------------------------

def parse_args():
    p = argparse.ArgumentParser(
        description="Mock gRPC server for multifreq_eddy.proto — MultiFreqEddyCurrent",
    )
    p.add_argument("--host", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    p.add_argument("--port", type=int, default=50051, help="监听端口（默认 50051）")
    p.add_argument("--base-freq", type=int, default=100,
                   choices=sorted(VALID_BASE_FREQUENCIES),
                   help="基频 Hz（默认 100）")
    p.add_argument("--avg-cycle", type=int, default=10,
                   help="平均周期数（默认 10）")
    p.add_argument("--norm-scale", type=float, default=1.0,
                   help="归一化系数（默认 1.0）")
    p.add_argument("--factors", type=str, default="1,2,4,8",
                   help="逗号分隔倍频系数（默认 1,2,4,8）")
    p.add_argument("--fps", type=float, default=100.0,
                   help="输出帧率 fps（默认 100，即 10ms/帧）")
    p.add_argument("--interval", type=int, default=None,
                   help="帧间隔 ms（默认由 --fps 推算；指定后覆盖 --fps）")
    p.add_argument("--noise", type=float, default=0.005,
                   help="阻抗噪声幅度（默认 0.005）")
    p.add_argument("--csv", type=str,
                   default=os.path.join(SCRIPT_DIR, "build_cmake", "20260626",
                                        "multifreq_eddy_db_export_20260626_121550.csv"),
                   help="CSV 数据文件路径（默认使用实际采集数据）")
    return p.parse_args()


def main():
    args = parse_args()

    # 帧率
    if args.interval is not None:
        frame_interval_sec = max(1, args.interval) / 1000.0
        fps = 1000.0 / frame_interval_sec
    else:
        fps = max(1.0, args.fps)
        frame_interval_sec = 1.0 / fps

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(name)s] %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )

    # 倍频系数解析
    factors = [int(x.strip()) for x in args.factors.split(",") if x.strip().lstrip("-").isdigit()]
    if not factors:
        factors = DEFAULT_FACTORS

    cfg = MultiFreqConfig(
        base_freq_hz=args.base_freq,
        avg_cycle_count=args.avg_cycle,
        norm_scale=args.norm_scale,
        factors=factors,
        noise=args.noise,
    )

    # 加载 CSV 数据
    csv_frames = None
    csv_path = args.csv.strip().strip('"').strip("'") if args.csv else ""
    if csv_path:
        if not os.path.isabs(csv_path):
            csv_path = os.path.join(os.getcwd(), csv_path)
        if os.path.isfile(csv_path):
            logger.info("加载 CSV: %s", csv_path)
            csv_frames = load_multifreq_csv(csv_path)
        else:
            logger.warning("CSV 文件不存在，回退到纯模拟: %s", csv_path)

    generator = MultiFreqGenerator(cfg, csv_frames=csv_frames)

    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16))
    mf_grpc.add_MultiFreqEddyCurrentServicer_to_server(
        MultiFreqEddyCurrentServicer(generator, frame_interval_sec=frame_interval_sec), server
    )

    bind_addr = f"{args.host}:{args.port}"
    server.add_insecure_port(bind_addr)
    server.start()

    logger.info("MultiFreqEddyCurrent listening on %s", bind_addr)
    if csv_frames:
        logger.info("  mode           = CSV 回放 (%d 帧循环)", len(csv_frames))
    else:
        logger.info("  mode           = 纯模拟")
    logger.info("  base_freq      = %d Hz", cfg.base_freq_hz)
    logger.info("  avg_cycle      = %d  (采集块 %.1f ms)", cfg.avg_cycle_count, cfg.cycle_duration_ms)
    logger.info("  sample_rate    = %d Hz", cfg.sample_rate_hz)
    logger.info("  samples/frame  = %d", cfg.samples_per_frame)
    logger.info("  factors        = %s", cfg.factors)
    logger.info("  frame_interval = %.1f ms  (%.0f fps)", frame_interval_sec * 1000, fps)
    logger.info("  noise          = %.4f", cfg.noise)
    logger.info("  methods: ListDevices StartDetection StopDetection GetStatus GetLatestFrame StreamFrames")

    stop_event = threading.Event()

    def _stop_handler(signum, frame):
        logger.info("收到信号 %s，正在停止...", signum)
        stop_event.set()

    signal.signal(signal.SIGINT, _stop_handler)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _stop_handler)

    try:
        while not stop_event.is_set():
            time.sleep(0.2)
    finally:
        server.stop(grace=1)
        logger.info("服务已停止")


if __name__ == "__main__":
    main()
