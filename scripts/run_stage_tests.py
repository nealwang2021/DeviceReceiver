#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
三轴台（StageService）自动化测试编排 + HTML 报告生成器。

做什么
  - 在指定 CMake 构建目录下执行 ``ctest -R Stage -V``（Qt Test：集成 + 主窗口面板）
  - 将 ctest 完整输出写入 HTML；并嵌入最新的 stage_test_*_steps_*.log 步骤日志（若存在）

何时使用
  - 本地/CI 验证 StageReceiverBackend 与 MainWindow 三轴台控件；需先 ``build_cmake.bat`` 编译出测试 exe

命令行示例
  python scripts/run_stage_tests.py --build-dir build_cmake
  python scripts/run_stage_tests.py --build-dir build_cmake --report build_cmake/my_report.html

依赖
  - Python 3.8+；本机 PATH 中有与 CMake 一致的 ctest（或 ``--ctest`` 指定）
  - 测试二进制与 stage_grpc_test_server.py 由 CMake/CTest 或手工环境准备

与批处理关系
  - 项目根目录 ``run_stage_tests.bat`` 为本脚本的快捷封装

与其它 Python 测试
  - ``tests/e2e/`` 下为 pytest 可选进程/E2E 烟测，与 CTest 本脚本独立
"""
from __future__ import annotations

import argparse
import datetime as _dt
import html
import os
import subprocess
import sys
from pathlib import Path


def _run_ctest(tests_dir: Path, ctest: str) -> tuple[int, str]:
    env = os.environ.copy()
    proc = subprocess.run(
        [
            ctest,
            "-R",
            "Stage",
            "-V",
            # 让通过的测试也把 QtTest 的 stdout/stderr 打进输出，便于生成详细报告
            "--test-output-size-passed",
            "20000000",
            "--test-output-size-failed",
            "20000000",
        ],
        cwd=str(tests_dir),
        capture_output=True,
        text=True,
        env=env,
    )
    out = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
    return proc.returncode, out


def _write_html_report(build_dir: Path, exit_code: int, log: str, out_file: Path) -> None:
    ts = _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    safe = html.escape(log)
    status = "PASS" if exit_code == 0 else "FAIL"
    color = "#16a34a" if exit_code == 0 else "#dc2626"
    artifacts_dir = build_dir / "build" / "release"

    def _read_latest(glob_pattern: str) -> str:
        # Pick the newest artifact to avoid pollution from previously running/hung tests.
        matches = sorted(
            artifacts_dir.glob(glob_pattern),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if not matches:
            return f"[missing] {glob_pattern} (expected under: {artifacts_dir})"
        p = matches[0]
        try:
            content = p.read_text(encoding="utf-8", errors="replace")
            return f"[using {p.name}]\n" + content
        except Exception as e:
            return f"[read error] {p.name}: {e}"

    integration_steps = _read_latest("stage_test_integration_steps_*.log")
    panel_steps = _read_latest("stage_test_panel_steps_*.log")

    body = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Stage tests {status}</title>
<style>body{{font-family:system-ui,sans-serif;margin:24px;}} pre{{background:#f4f4f5;padding:12px;overflow:auto;}}</style>
</head><body>
<h1 style="color:{color}">三轴台自动化测试 — {status}</h1>
<p>时间: {html.escape(ts)}</p>
<p>构建目录: {html.escape(str(build_dir))}</p>
<p>ctest 退出码: {exit_code}</p>
<h2>ctest -R Stage -V 输出</h2>
<pre>{safe}</pre>
<h2>StageIntegration 步骤日志</h2>
<pre>{html.escape(integration_steps)}</pre>
<h2>StagePanelUi 步骤日志</h2>
<pre>{html.escape(panel_steps)}</pre>
</body></html>"""
    out_file.write_text(body, encoding="utf-8")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description="Run Stage* ctests and write HTML report")
    ap.add_argument(
        "--build-dir",
        type=Path,
        default=root / "build_cmake",
        help="CMake build directory (contains CTestTestfile.cmake)",
    )
    ap.add_argument("--ctest", default="ctest", help="ctest executable")
    ap.add_argument(
        "--ctest-dir",
        type=Path,
        default=None,
        help="Directory with CTestTestfile.cmake (default: build-dir or build-dir/tests)",
    )
    ap.add_argument(
        "--report",
        type=Path,
        default=root / "build_cmake" / "stage_test_report.html",
        help="Output HTML report path",
    )
    args = ap.parse_args()
    build_dir = args.build_dir.resolve()
    if args.ctest_dir is not None:
        ctest_dir = args.ctest_dir.resolve()
    elif (build_dir / "CTestTestfile.cmake").is_file():
        ctest_dir = build_dir
    else:
        ctest_dir = (build_dir / "tests").resolve()
    if not (ctest_dir / "CTestTestfile.cmake").is_file():
        print(f"[ERROR] No CTestTestfile.cmake under: {ctest_dir}", file=sys.stderr)
        return 2
    code, log = _run_ctest(ctest_dir, args.ctest)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    _write_html_report(build_dir, code, log, args.report)
    print(f"[INFO] Report written: {args.report}")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
