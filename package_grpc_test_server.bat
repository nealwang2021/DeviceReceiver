@echo off
REM ============================================================================
REM  package_grpc_test_server.bat — 将 grpc_test_server.py 打成独立 exe（PyInstaller）
REM ----------------------------------------------------------------------------
REM  功能: 自动寻找可 import grpc/protobuf 的 Python，用 PyInstaller --onefile 打包
REM        被测设备数据流 AcquisitionDevice（非三轴台 stage_grpc_test_server）
REM  输出: build\release\grpc_test_server.exe（若存在 build\debug 会同步复制）
REM  运行示例: grpc_test_server.exe -d D:\data\xxx.db --no-csv --host 127.0.0.1
REM  说明: 不包含演示 CSV；回放请用 --db 或运行时指定 --csv 外部文件路径
REM  前置: 根目录存在 grpc_test_server.py 与 proto\generated_py\device_pb2*.py
REM  首次会自动 pip install pyinstaller（若未安装）
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

REM 固定到批处理所在目录（避免 pushd / 相对路径异常）；chcp 失败不阻断
cd /d "%~dp0" 2>nul
if errorlevel 1 (
    echo [ERROR] 无法切换到脚本目录: %~dp0
    exit /b 1
)
chcp 65001 >nul 2>&1

set "PYTHON_EXE="
set "PYTHON_ARGS="

if exist "%CD%\.venv\Scripts\python.exe" (
    call :try_python "%CD%\.venv\Scripts\python.exe" ""
)

if not defined PYTHON_EXE (
    for /f "usebackq delims=" %%I in (`where python 2^>nul`) do (
        if not defined PYTHON_EXE call :try_python "%%I" ""
    )
)

if not defined PYTHON_EXE (
    for /f "usebackq delims=" %%I in (`where py 2^>nul`) do (
        if not defined PYTHON_EXE call :try_python "%%I" "-3"
    )
)

if not defined PYTHON_EXE (
    echo [ERROR] 未找到满足条件的 Python 解释器（需可 import grpc 和 google.protobuf）
    echo [INFO] 可先执行: python -m pip install grpcio protobuf
    exit /b 1
)

echo [INFO] 使用解释器: "%PYTHON_EXE%" %PYTHON_ARGS%

if not exist "grpc_test_server.py" (
    echo [ERROR] 未找到 grpc_test_server.py（当前目录: %CD%）
    exit /b 1
)

set "PROTO_PY_SRC=%CD%\proto\generated_py"

if not exist "%PROTO_PY_SRC%\device_pb2.py" (
    echo [ERROR] 未找到 proto\generated_py\device_pb2.py
    echo [INFO] 请先生成 Python protobuf 文件后再打包
    exit /b 1
)

"%PYTHON_EXE%" %PYTHON_ARGS% -m pip show pyinstaller >nul 2>&1
if errorlevel 1 (
    echo [INFO] 正在安装 PyInstaller...
    "%PYTHON_EXE%" %PYTHON_ARGS% -m pip install pyinstaller
    if errorlevel 1 (
        echo [ERROR] PyInstaller 安装失败
        exit /b 1
    )
)

set "DIST_DIR=build\release"
set "WORK_DIR=build\temp\pyinstaller"

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%" 2>nul
if not exist "%WORK_DIR%" mkdir "%WORK_DIR%" 2>nul

REM --add-data 源路径须为绝对路径，否则在 --specpath=WORK_DIR 时 PyInstaller 会误解析到临时目录下
set "ADD_PROTO=%PROTO_PY_SRC%;proto\generated_py"

echo [INFO] 开始打包 grpc_test_server.exe （不含内置 CSV）...
"%PYTHON_EXE%" %PYTHON_ARGS% -m PyInstaller ^
    --noconfirm ^
    --clean ^
    --onefile ^
    --name grpc_test_server ^
    --distpath "%DIST_DIR%" ^
    --workpath "%WORK_DIR%" ^
    --specpath "%WORK_DIR%" ^
    --paths "%PROTO_PY_SRC%" ^
    --add-data "%ADD_PROTO%" ^
    --hidden-import device_pb2 ^
    --hidden-import device_pb2_grpc ^
    --hidden-import grpc ^
    --hidden-import google.protobuf ^
    --collect-submodules google.protobuf ^
    --hidden-import grpc._cython.cygrpc ^
    grpc_test_server.py

if errorlevel 1 (
    echo [ERROR] 打包失败
    exit /b 1
)

if exist "build\debug\" (
    copy /Y "%DIST_DIR%\grpc_test_server.exe" "build\debug\grpc_test_server.exe" >nul 2>&1
)

if exist "build_cmake\build\release\" (
    copy /Y "%DIST_DIR%\grpc_test_server.exe" "build_cmake\build\release\grpc_test_server.exe" >nul 2>&1
    echo [INFO] 已同步到 build_cmake\build\release\
)

echo [OK] 打包完成: %DIST_DIR%\grpc_test_server.exe
exit /b 0

:try_python
set "CAND_PY_EXE=%~1"
set "CAND_PY_ARGS=%~2"

if "%CAND_PY_EXE%"=="" exit /b 0
if not exist "%CAND_PY_EXE%" exit /b 0

"%CAND_PY_EXE%" %CAND_PY_ARGS% -c "import grpc, google.protobuf" >nul 2>&1
if errorlevel 1 (
    echo [WARN] 跳过解释器（缺少 grpc/protobuf）: "%CAND_PY_EXE%" %CAND_PY_ARGS%
    exit /b 0
)

set "PYTHON_EXE=%CAND_PY_EXE%"
set "PYTHON_ARGS=%CAND_PY_ARGS%"
exit /b 0
