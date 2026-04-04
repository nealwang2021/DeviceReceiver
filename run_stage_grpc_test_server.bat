@echo off
REM ============================================================================
REM  run_stage_grpc_test_server.bat — 启动三轴台 StageService Python 桩服务端
REM ----------------------------------------------------------------------------
REM  功能: 调用同目录 stage_grpc_test_server.py（gRPC StageService，默认端口 50052）
REM        供主程序「三轴台测试装置」面板联调；与 grpc_test_server.py（设备数据流）不同
REM  注意: 本文件为 .bat，勿执行 python run_stage_grpc_test_server.bat（会 SyntaxError）
REM  正确: 双击本脚本，或 cmd 下 run_stage_grpc_test_server.bat [参数]
REM        或直接: python stage_grpc_test_server.py [--port 50052] [--host 0.0.0.0] [--external]
REM        无公网IP穿透: scripts\start_stage_with_ngrok.bat（需 ngrok）
REM  依赖: pip install grpcio protobuf；proto 桩在 proto\generated_py\
REM ============================================================================
REM ---------------------------------------------------------------------------
REM 这是 cmd 批处理脚本, 不是 Python 文件.
REM 错误用法: python run_stage_grpc_test_server.bat  (会报 SyntaxError)
REM 正确用法:
REM   1) 在本目录打开 cmd, 直接运行: run_stage_grpc_test_server.bat
REM   2) 或: python stage_grpc_test_server.py [--port 50052]
REM ---------------------------------------------------------------------------
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"

if not exist "stage_grpc_test_server.py" (
    echo [ERROR] 未找到 stage_grpc_test_server.py
    exit /b 1
)

python stage_grpc_test_server.py %*
if errorlevel 1 (
    echo [INFO] 若提示缺少模块, 请执行: python -m pip install grpcio protobuf
    exit /b 1
)
exit /b 0
