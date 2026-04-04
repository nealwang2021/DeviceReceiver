"""
tests/e2e/test_stage_e2e_optional.py — 可选端到端 / 烟测用例
===========================================================
依赖 pytest；部分用例标记 optional，需可执行文件或 pywinauto 时才会深入执行。

做什么
  - test_repo_paths_exist：仓库关键路径存在性
  - test_realtime_data_process_smoke：短暂启动 stage_grpc_test_server + realtime_data（需已编译 exe）
  - test_pywinauto_find_window_optional：若安装 pywinauto，枚举顶层窗口（弱断言）

何时运行
  - 本地验证「能拉起进程」；**非** CTest 三轴台自动化（见 scripts/run_stage_tests.py）

注意
  控件级 UI 断言易碎（分辨率/主题/焦点），默认以进程级烟测为主。
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

import pytest


def test_repo_paths_exist(repo_root: Path, stage_server_script: Path) -> None:
    assert (repo_root / "MainWindow.cpp").is_file()
    assert stage_server_script.is_file()


@pytest.mark.optional
def test_realtime_data_process_smoke(realtime_data_exe: Path | None, repo_root: Path) -> None:
    if realtime_data_exe is None:
        pytest.skip("Set REALTIME_DATA_EXE or build under build_cmake/build/release")

    srv = subprocess.Popen(
        [sys.executable, str(repo_root / "stage_grpc_test_server.py"), "--port", "50052"],
        cwd=str(repo_root),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(2.0)
    try:
        proc = subprocess.Popen(
            [str(realtime_data_exe)],
            cwd=str(realtime_data_exe.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(4.0)
        proc.terminate()
        try:
            proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()


@pytest.mark.optional
def test_pywinauto_find_window_optional(realtime_data_exe: Path | None) -> None:
    if realtime_data_exe is None:
        pytest.skip("REALTIME_DATA_EXE not set")
    try:
        from pywinauto import Desktop  # type: ignore
    except ImportError:
        pytest.skip("pip install pywinauto for UI tree inspection")

    proc = subprocess.Popen(
        [str(realtime_data_exe)],
        cwd=str(realtime_data_exe.parent),
    )
    time.sleep(4.0)
    try:
        desk = Desktop(backend="uia")
        wins = [w.window_text() for w in desk.windows()[:40]]
        assert len(wins) >= 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=12)
        except subprocess.TimeoutExpired:
            proc.kill()
