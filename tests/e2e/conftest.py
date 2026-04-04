"""
tests/e2e/conftest.py — pytest 会话级 fixture（可选端到端测试）
===============================================================
本目录测试默认**不**随主 CMake 构建；需手动安装 pytest，用于烟测/路径检查。

提供的 fixture
  repo_root          仓库根目录
  realtime_data_exe  主程序 exe（环境变量 REALTIME_DATA_EXE 或常见 build 路径）
  stage_server_script 项目根目录 stage_grpc_test_server.py 路径

用法示例
  cd tests/e2e
  pip install -r requirements.txt
  pytest -v

说明
  与 scripts/run_stage_tests.py（CTest/Qt Test）不同；本目录偏进程级/可选 UI 探测。
"""
from __future__ import annotations

import os
from pathlib import Path

import pytest


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


@pytest.fixture(scope="session")
def realtime_data_exe(repo_root: Path) -> Path | None:
    """可通过环境变量 REALTIME_DATA_EXE 指定；否则尝试常见构建路径。"""
    env = os.environ.get("REALTIME_DATA_EXE", "").strip()
    if env and Path(env).is_file():
        return Path(env).resolve()
    candidates = [
        repo_root / "build_cmake" / "build" / "release" / "realtime_data.exe",
        repo_root / "build" / "release" / "realtime_data.exe",
    ]
    for p in candidates:
        if p.is_file():
            return p.resolve()
    return None


@pytest.fixture(scope="session")
def stage_server_script(repo_root: Path) -> Path:
    return repo_root / "stage_grpc_test_server.py"
