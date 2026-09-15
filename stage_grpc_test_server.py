#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
stage_grpc_test_server.py — 三轴台 StageService gRPC 桩（Python）
================================================================
与 proto/stage.proto 一致，供主窗口「三轴台测试装置」面板及 StageReceiverBackend 联调。

功能概要
  - 实现 Connect / GetPositions / PositionStream / Jog / MoveAbs/Rel / SetSpeed / 扫描等 RPC
  - 用于无真实工装时的功能验证；默认 0.0.0.0:50052（可与被测设备 50051 并存）
  - PositionStream：无 Jog 且尚未 Move/Jog 时按默认栅格弓字形步进 XY（0~1000mm，步距 10mm，与热力图默认一致）；MoveAbs/MoveRel/开 Jog 后锁定 XY。关闭：--no-serpentine

运行
  python stage_grpc_test_server.py [--port 50052] [--host 0.0.0.0] [--external]
  或 Windows：run_stage_grpc_test_server.bat（勿用 python 运行 .bat）
  无公网 IP 需穿透：scripts/start_stage_with_ngrok.bat（见 scripts/README_stage_tunnel.md）

依赖
  pip install grpcio protobuf；桩：proto/generated_py/stage_pb2*.py（改 proto 后需 protoc）

相关文件
  - 被测设备数据流：grpc_test_server.py（DeviceDataService）
  - 打独立 exe：package_stage_grpc_test_server.bat

命名区分（勿混淆）
  - grpc_test_server.py  → 被测设备 DeviceDataService（Subscribe/SendCommand）
  - stage_grpc_test_server.py → 三轴台 StageService（Connect / PositionStream / Jog / …）

默认监听 0.0.0.0:50052；控制端 Receiver/StageGrpcEndpoint 或界面地址需同端口（IPv6 用 [::1]:50052）。

绑定失败（Failed to bind）常见原因：端口已被占用。可换 --port 50053，或 netstat 查占用（Windows）。
"""

from __future__ import annotations

import argparse
import os
import signal
import socket
import sys
import threading
import time
from concurrent import futures

# ── 将 proto/generated_py 加入搜索路径（与 grpc_test_server.py 一致）────────
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RUNTIME_BASE_DIR = getattr(sys, "_MEIPASS", SCRIPT_DIR)
for _candidate in [
    os.path.join(RUNTIME_BASE_DIR, "proto", "generated_py"),
    os.path.join(SCRIPT_DIR, "proto", "generated_py"),
    SCRIPT_DIR,
]:
    if os.path.isfile(os.path.join(_candidate, "stage_pb2.py")):
        if _candidate not in sys.path:
            sys.path.insert(0, _candidate)
        break

import grpc
import stage_pb2
import stage_pb2_grpc


def _unix_ms_now() -> int:
    return int(time.time() * 1000)


def _mm_to_pulse(mm: float) -> int:
    return int(round(mm * 1000.0))


def _fill_positions(x_mm: float, y_mm: float, z_mm: float) -> stage_pb2.PositionsReply:
    r = stage_pb2.PositionsReply()
    r.x.pulse = _mm_to_pulse(x_mm)
    r.x.mm = x_mm
    r.y.pulse = _mm_to_pulse(y_mm)
    r.y.mm = y_mm
    r.z.pulse = _mm_to_pulse(z_mm)
    r.z.mm = z_mm
    r.unixMs = _unix_ms_now()
    return r


# 与 HeatMapPlotWindow 默认台位范围/步距对齐（弓字形扫查）
SERP_X_MIN_MM = 0.0
SERP_X_MAX_MM = 1000.0
SERP_Y_MIN_MM = 0.0
SERP_Y_MAX_MM = 1000.0
SERP_X_STEP_MM = 10.0
SERP_Y_STEP_MM = 10.0


def _serpentine_grid_dims() -> tuple[int, int]:
    """沿 X/Y 每步进一格的格点数（含端点）。"""
    nx = int(round((SERP_X_MAX_MM - SERP_X_MIN_MM) / SERP_X_STEP_MM)) + 1
    ny = int(round((SERP_Y_MAX_MM - SERP_Y_MIN_MM) / SERP_Y_STEP_MM)) + 1
    return max(1, nx), max(1, ny)


def _serpentine_xy_from_cell_index(cell_index: int, main_axis: int = 0) -> tuple[float, float]:
    """弓字形（往返蛇形）：按格点下标离散步进，单位 mm。每收到一次 PositionStream 周期前进一格。

    main_axis=0 (X_SCAN_Y_STEP): 沿 X 扫描行、Y 方向步进（默认，向后兼容）
    main_axis=1 (Y_SCAN_X_STEP): 沿 Y 扫描行、X 方向步进
    """
    nx, ny = _serpentine_grid_dims()
    if main_axis == 1:  # Y_SCAN_X_STEP — Y 扫描、X 步进
        # 交换轴：格点数也交换 — ny 行(沿 Y)、nx 列(沿 X)
        nx, ny = ny, nx
        step_scan = SERP_Y_STEP_MM   # 扫描方向的步距
        step_cross = SERP_X_STEP_MM  # 步进方向的步距
        min_scan = SERP_Y_MIN_MM
        min_cross = SERP_X_MIN_MM
        swap_axes = True
    else:
        step_scan = SERP_X_STEP_MM
        step_cross = SERP_Y_STEP_MM
        min_scan = SERP_X_MIN_MM
        min_cross = SERP_Y_MIN_MM
        swap_axes = False

    n = nx * ny
    k = cell_index % n if n > 0 else 0
    row = k // nx
    col = k % nx
    if row % 2 == 1:
        col = nx - 1 - col
    scan_val = min_scan + col * step_scan
    cross_val = min_cross + row * step_cross
    if swap_axes:
        return cross_val, scan_val  # X=步进, Y=扫描
    else:
        return scan_val, cross_val  # X=扫描, Y=步进


class StageTestServicer(stage_pb2_grpc.StageServiceServicer):
    """模拟三轴台：位置 + Jog + 扫描状态，供控制端全功能验证。"""

    def __init__(self, *, serpentine_demo: bool = True) -> None:
        self._lock = threading.Lock()
        self._x = 0.0
        self._y = 0.0
        self._z = 1.0
        self._jog_vx = 0.0
        self._jog_vy = 0.0
        self._jog_vz = 0.0
        self._speed_pulse_per_sec = 20000
        self._accel_ms = 100
        self._scan_running = False
        self._scan_detail = ""
        # True：PositionStream 在 vx=vy=vz=0 时用弓字形更新 XY（热力图联调）；False：仅 Jog/Move 改变位置
        self._serpentine_demo = serpentine_demo
        # MoveAbs/MoveRel/开启 Jog 后置 True，避免弓字形下一拍覆盖用户设定；Connect 时清零
        self._user_xy_locked = False
        self._serp_cell_index = 0
        # 扫描主轴与步进模式: X_SCAN_Y_STEP=0 或 Y_SCAN_X_STEP=1
        self._scan_main_axis = stage_pb2.X_SCAN_Y_STEP
        self._scan_x_step = SERP_X_STEP_MM

    def _jog_scale(self) -> float:
        if self._speed_pulse_per_sec < 1:
            return 1.0
        ref = 20000.0
        return max(0.1, min(3.0, self._speed_pulse_per_sec / ref))

    def Connect(self, request, context):
        with self._lock:
            self._user_xy_locked = False
            self._serp_cell_index = 0
        msg = f"stage-grpc-test-server: accepted stage TCP target {request.ip}:{request.port}"
        print(f"[Connect] {msg}")
        return stage_pb2.Result(ok=True, message=msg)

    def Disconnect(self, request, context):
        with self._lock:
            self._jog_vx = self._jog_vy = self._jog_vz = 0.0
        print("[Disconnect]")
        return stage_pb2.Result(ok=True, message="stage-grpc-test-server: disconnected (simulated)")

    def GetPositions(self, request, context):
        with self._lock:
            return _fill_positions(self._x, self._y, self._z)

    def PositionStream(self, request, context):
        interval_ms = max(10, request.intervalMs)
        print(f"[PositionStream] interval_ms={interval_ms}")
        last_print = 0.0
        try:
            while context.is_active():
                t0 = time.monotonic()
                with self._lock:
                    dt = interval_ms / 1000.0
                    jogging = (
                        self._jog_vx != 0.0
                        or self._jog_vy != 0.0
                        or self._jog_vz != 0.0
                    )
                    if jogging:
                        self._x += self._jog_vx * dt
                        self._y += self._jog_vy * dt
                        self._z += self._jog_vz * dt
                    elif self._scan_running and not self._user_xy_locked:
                        # 仅在扫描模式下自动弓字形走位，默认静止
                        sx, sy = _serpentine_xy_from_cell_index(self._serp_cell_index, self._scan_main_axis)
                        self._serp_cell_index += 1
                        self._x = sx
                        self._y = sy
                    reply = _fill_positions(self._x, self._y, self._z)

                    # 扫描模式下每秒打印一次实时位置
                    if self._scan_running and t0 - last_print >= 1.0:
                        last_print = t0
                        nx, ny = _serpentine_grid_dims()
                        if self._scan_main_axis == stage_pb2.Y_SCAN_X_STEP:
                            nx, ny = ny, nx
                        total = nx * ny
                        prog = self._serp_cell_index
                        ma = stage_pb2.ScanMainAxis.Name(self._scan_main_axis)
                        print(f"[ScanPos] X={self._x:.1f} Y={self._y:.1f} Z={self._z:.1f}  "
                              f"mode={ma} 进度={prog}/{total} ({100.0*prog/total:.1f}%)")

                yield reply
                elapsed = time.monotonic() - t0
                sleep_s = max(0.0, interval_ms / 1000.0 - elapsed)
                if sleep_s > 0:
                    time.sleep(sleep_s)
        finally:
            print("[PositionStream] ended")

    def Jog(self, request, context):
        base = 2.0 * self._jog_scale()
        with self._lock:
            if request.axis == stage_pb2.Axis.Y:
                vp = "_jog_vy"
            elif request.axis == stage_pb2.Axis.Z:
                vp = "_jog_vz"
            else:
                vp = "_jog_vx"
            if not request.enable:
                setattr(self, vp, 0.0)
                print(f"[Jog] axis={request.axis} off")
                return stage_pb2.Result(ok=True, message="jog off")
            self._user_xy_locked = True
            v = base if request.plus else -base
            setattr(self, vp, v)
            print(f"[Jog] axis={request.axis} plus={request.plus} on")
            return stage_pb2.Result(ok=True, message="jog on")

    def MoveAbs(self, request, context):
        with self._lock:
            self._user_xy_locked = True
            self._x = request.xMm
            self._y = request.yMm
            self._z = request.zMm
            msg = (
                f"move_abs ok -> ({self._x}, {self._y}, {self._z}) mm, "
                f"timeoutMs={request.timeoutMs}"
            )
        print(f"[MoveAbs] {msg}")
        return stage_pb2.Result(ok=True, message=msg)

    def MoveRel(self, request, context):
        d = request.deltaMm
        with self._lock:
            self._user_xy_locked = True
            if request.axis == stage_pb2.Axis.Y:
                self._y += d
            elif request.axis == stage_pb2.Axis.Z:
                self._z += d
            else:
                self._x += d
            msg = f"move_rel ok axis={request.axis} delta={d} mm"
        print(f"[MoveRel] {msg}")
        return stage_pb2.Result(ok=True, message=msg)

    def SetSpeed(self, request, context):
        with self._lock:
            self._speed_pulse_per_sec = request.speedPulsePerSec
            self._accel_ms = request.accelMs
            msg = f"set_speed ok pulse/s={self._speed_pulse_per_sec} accel_ms={self._accel_ms}"
        print(f"[SetSpeed] {msg}")
        return stage_pb2.Result(ok=True, message=msg)

    def StartScan(self, request, context):
        mode_name = stage_pb2.ScanMode.Name(request.mode)
        # proto3: enum/double 无 hasField，未设置时默认 0/0.0
        main_axis = request.mainAxis
        main_axis_name = stage_pb2.ScanMainAxis.Name(main_axis)
        x_step = request.xStep
        detail = (
            f"mode={mode_name} mainAxis={main_axis_name} xs={request.xs} xe={request.xe} ys={request.ys} ye={request.ye} "
            f"yStep={request.yStep} xStep={x_step} zFix={request.zFix}"
        )
        with self._lock:
            self._scan_running = True
            self._scan_detail = detail
            self._scan_main_axis = main_axis
            # 将扫描参数覆盖弓字形演示参数，使 PositionStream 按扫描范围走位
            global SERP_X_MIN_MM, SERP_X_MAX_MM, SERP_Y_MIN_MM, SERP_Y_MAX_MM
            global SERP_X_STEP_MM, SERP_Y_STEP_MM
            SERP_X_MIN_MM = request.xs
            SERP_X_MAX_MM = request.xe
            SERP_Y_MIN_MM = request.ys
            SERP_Y_MAX_MM = request.ye
            y_step = max(0.1, request.yStep)
            x_step_val = max(0.1, x_step) if x_step > 0 else max(0.1, request.yStep)
            if main_axis == stage_pb2.Y_SCAN_X_STEP:
                # Y 扫描、X 步进：沿 Y 的扫描步距=yStep，沿 X 的步进步距=xStep
                SERP_X_STEP_MM = x_step_val
                SERP_Y_STEP_MM = y_step
            else:
                # X 扫描、Y 步进（默认）：沿 X 的扫描步距=yStep，沿 Y 的步进步距=yStep
                SERP_X_STEP_MM = y_step
                SERP_Y_STEP_MM = y_step
            # 重置蛇形走位起点
            self._serp_cell_index = 0
            self._user_xy_locked = False
            # Z 轴固定到扫描平面
            self._z = request.zFix
        print(f"[StartScan] {detail}")
        return stage_pb2.Result(ok=True, message="scan started (simulated): " + detail)

    def StopScan(self, request, context):
        with self._lock:
            self._scan_running = False
            # 恢复默认演示范围
            global SERP_X_MIN_MM, SERP_X_MAX_MM, SERP_Y_MIN_MM, SERP_Y_MAX_MM
            global SERP_X_STEP_MM, SERP_Y_STEP_MM
            SERP_X_MIN_MM = 0.0
            SERP_X_MAX_MM = 1000.0
            SERP_Y_MIN_MM = 0.0
            SERP_Y_MAX_MM = 1000.0
            SERP_X_STEP_MM = 10.0
            SERP_Y_STEP_MM = 10.0
        print("[StopScan]")
        return stage_pb2.Result(ok=True, message="scan stopped (simulated)")

    def GetScanStatus(self, request, context):
        with self._lock:
            running = self._scan_running
            detail = self._scan_detail
        if running:
            status = "running: " + detail
        else:
            status = "idle" if not detail else f"idle; last={detail}"
        return stage_pb2.ScanStatusReply(running=running, status=status)


def _guess_primary_ipv4() -> str | None:
    """Best-effort：通过 UDP「假连接」得到本机用于出网的典型 IPv4（多网卡时仅供参考）。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.settimeout(0.5)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            return str(ip) if ip else None
        finally:
            s.close()
    except OSError:
        return None


def _hostname_ipv4_candidates() -> list[str]:
    """getaddrinfo(主机名) 得到的非 127 IPv4，去重。"""
    seen: set[str] = set()
    out: list[str] = []
    try:
        hn = socket.gethostname()
        for info in socket.getaddrinfo(hn, None, socket.AF_INET, socket.SOCK_STREAM):
            ip = info[4][0]
            if not ip or ip.startswith("127."):
                continue
            if ip not in seen:
                seen.add(ip)
                out.append(ip)
    except OSError:
        pass
    return out


def _print_external_hints(port: int, listen_host: str) -> None:
    """--external 时附加说明：局域网、无公网 IP、内网穿透脚本。"""
    primary = _guess_primary_ipv4()
    extra = _hostname_ipv4_candidates()
    print("  ─────────────────────────────────────────────")
    print("  [外网/局域网提示] --external")
    print(f"  当前监听: {listen_host}:{port}")
    if primary:
        print(f"  本机典型 IPv4（出网）: {primary}  → 同网段设备可试 {primary}:{port}")
    if extra:
        print("  本机其它 IPv4: " + ", ".join(extra))
    if not primary and not extra:
        print("  （未能枚举本机 IPv4，请用 ipconfig 查看）")
    print("  无公网 IPv4（运营商 CGNAT）时，端口转发无效，请使用 scripts/start_stage_with_ngrok.*")
    print("  Windows 防火墙若拦截入站，请放行本端口的 TCP。")
    print("  gRPC 为明文 insecure，勿长期暴露不可信公网；生产请 TLS 或 VPN。")
    print("  ─────────────────────────────────────────────")


def _print_bind_failure_help(port: int, host: str, err: BaseException) -> None:
    print("\n[错误] 无法在以下地址绑定 gRPC 监听:", file=sys.stderr)
    print(f"       {host}:{port}", file=sys.stderr)
    print(f"       ({err!r})", file=sys.stderr)
    print(
        "\n常见原因与处理：\n"
        "  1) 端口已被占用 — 关闭其它 stage_grpc_test_server 实例，或在任务管理器结束残留 python.exe；\n"
        "     Windows 查看占用:  cmd 中执行  netstat -ano | findstr :" + str(port) + "\n"
        "  2) 换端口启动 —  python stage_grpc_test_server.py --port 50053\n"
        "  3) 仅本机回环 —  python stage_grpc_test_server.py --host 127.0.0.1 --port " + str(port) + "\n",
        file=sys.stderr,
    )


def serve(
    port: int,
    host: str = "0.0.0.0",
    *,
    external_mode: bool = False,
    serpentine_demo: bool = True,
) -> None:
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=10))
    stage_pb2_grpc.add_StageServiceServicer_to_server(
        StageTestServicer(serpentine_demo=serpentine_demo), server
    )
    listen_addr = f"{host}:{port}"
    try:
        bound = server.add_insecure_port(listen_addr)
        if bound == 0:
            raise RuntimeError("add_insecure_port returned 0 (bind failed)")
    except RuntimeError as e:
        _print_bind_failure_help(port, host, e)
        raise
    server.start()

    print("=" * 60)
    print("  StageService gRPC 测试服务器（三轴台工装模拟）")
    print(f"  监听地址: {listen_addr}")
    if external_mode:
        _print_external_hints(port, host)
    print("  ─────────────────────────────────────────────")
    print("  被测设备数据流测试请使用: grpc_test_server.py（默认端口 50051）")
    print("  本脚本对应: stage.proto / 三轴台面板 StageReceiverBackend")
    print("  ─────────────────────────────────────────────")
    print("  退出: 按 Ctrl+C（或向进程发送结束信号）。")
    print("  若仍无法退出: 直接关闭本窗口, 或任务管理器结束对应 python.exe。")
    print("=" * 60)

    shutdown = threading.Event()

    def on_signal(sig, frame):
        print("\n[信号] 正在关闭 Stage 测试服务器...", flush=True)
        shutdown.set()

    try:
        signal.signal(signal.SIGINT, on_signal)
    except (ValueError, OSError):
        pass
    if hasattr(signal, "SIGTERM"):
        try:
            signal.signal(signal.SIGTERM, on_signal)
        except (ValueError, OSError):
            pass

    # Windows: 主线程若无限期 Event.wait()，Ctrl+C 可能长期无法生效（阻塞在 C 层）。
    # 使用短超时轮询，让主线程能及时处理 SIGINT / KeyboardInterrupt。
    try:
        while not shutdown.is_set():
            shutdown.wait(timeout=0.3)
    except KeyboardInterrupt:
        print("\n[Ctrl+C] 正在关闭 Stage 测试服务器...", flush=True)

    server.stop(grace=2)
    print("服务器已停止。")


def main():
    parser = argparse.ArgumentParser(
        description="StageService gRPC 测试服务器（三轴台工装，区别于 grpc_test_server.py）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例：
  python stage_grpc_test_server.py                  # 默认 0.0.0.0:50052
  python stage_grpc_test_server.py --port 50051    # 独占端口时可用 50051
  python stage_grpc_test_server.py --external      # 打印本机 IPv4 与防火墙/CGNAT 提示
  python stage_grpc_test_server.py --host 127.0.0.1 --port 50052   # 仅本机（配 ngrok 时常用）
        """,
    )
    parser.add_argument(
        "--port",
        type=int,
        default=50052,
        help="监听端口（默认 50052，避免与 grpc_test_server.py 默认 50051 冲突）",
    )
    parser.add_argument(
        "--host",
        default=None,
        metavar="ADDR",
        help="监听地址（默认 0.0.0.0；仅本机可设 127.0.0.1，与 ngrok 同机时常用）",
    )
    parser.add_argument(
        "--external",
        "--external-ipv4",
        dest="external",
        action="store_true",
        help="打印局域网/外网访问提示与本机 IPv4 枚举（不改变默认绑定，可与 --host 同用）",
    )
    parser.add_argument(
        "--no-serpentine",
        action="store_true",
        help="关闭 PositionStream 在无 Jog 时的弓字形 XY 演示（默认开启，便于热力图等需要 XY 同时变化的联调）",
    )
    args = parser.parse_args()
    host = args.host if args.host is not None else "0.0.0.0"
    serve(
        args.port,
        host=host,
        external_mode=args.external,
        serpentine_demo=not args.no_serpentine,
    )


if __name__ == "__main__":
    main()
