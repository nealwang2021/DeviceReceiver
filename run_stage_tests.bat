@echo off
REM ============================================================================
REM  run_stage_tests.bat — 三轴台自动化测试 + HTML 报告（封装 Python）
REM ----------------------------------------------------------------------------
REM  前置条件: 已执行 build_cmake.bat 且生成 build_cmake\CTestTestfile.cmake
REM  行为: 调用 scripts\run_stage_tests.py，对 ctest -R Stage 跑集成/UI 测试，
REM        并在 build_cmake\stage_test_report.html 生成汇总报告（含步骤日志）
REM  等价手动: python scripts\run_stage_tests.py --build-dir <项目>\build_cmake
REM ============================================================================
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
if not exist "build_cmake\CTestTestfile.cmake" (
  echo [ERROR] Run CMake configure first: build_cmake.bat
  exit /b 2
)
python scripts\run_stage_tests.py --build-dir "%ROOT%build_cmake" --report "%ROOT%build_cmake\stage_test_report.html"
exit /b %ERRORLEVEL%
