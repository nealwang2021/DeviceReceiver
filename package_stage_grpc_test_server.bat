@echo off
REM ============================================================================
REM  package_stage_grpc_test_server.bat — 将 stage_grpc_test_server.py 打成独立 exe
REM ----------------------------------------------------------------------------
REM  功能: 与 package_grpc_test_server.bat 相同流程，目标为三轴台 StageService 桩
REM        输出 build\release\stage_grpc_test_server.exe
REM  前置: stage_grpc_test_server.py + proto\generated_py\stage_pb2*.py
REM        （若缺桩，脚本内提示 protoc 命令）
REM  与 grpc_test_server.exe 区别: 本包为工装/三轴台 gRPC，非主数据通道
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

set "ROOT_DIR=%~dp0"
pushd "%ROOT_DIR%" >nul

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
    popd >nul
    exit /b 1
)

echo [INFO] 使用解释器: "%PYTHON_EXE%" %PYTHON_ARGS%

if not exist "stage_grpc_test_server.py" (
    echo [ERROR] 未找到 stage_grpc_test_server.py
    popd >nul
    exit /b 1
)

set "PROTO_PY_SRC=%CD%\proto\generated_py"

if not exist "%PROTO_PY_SRC%\stage_pb2.py" (
    echo [ERROR] 未找到 proto\generated_py\stage_pb2.py
    echo [INFO] 请执行: python -m grpc_tools.protoc -Iproto --python_out=proto/generated_py --grpc_python_out=proto/generated_py proto/stage.proto
    popd >nul
    exit /b 1
)

"%PYTHON_EXE%" %PYTHON_ARGS% -m pip show pyinstaller >nul 2>&1
if errorlevel 1 (
    echo [INFO] 正在安装 PyInstaller...
    "%PYTHON_EXE%" %PYTHON_ARGS% -m pip install pyinstaller
    if errorlevel 1 (
        echo [ERROR] PyInstaller 安装失败
        popd >nul
        exit /b 1
    )
)

set "DIST_DIR=build\release"
set "WORK_DIR=build\temp\pyinstaller_stage"

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"

echo [INFO] 开始打包 stage_grpc_test_server.exe （三轴台 StageService，区别于 grpc_test_server.exe）...
"%PYTHON_EXE%" %PYTHON_ARGS% -m PyInstaller ^
    --noconfirm ^
    --clean ^
    --onefile ^
    --name stage_grpc_test_server ^
    --distpath "%DIST_DIR%" ^
    --workpath "%WORK_DIR%" ^
    --specpath "%WORK_DIR%" ^
    --paths "%PROTO_PY_SRC%" ^
    --add-data "%PROTO_PY_SRC%;proto\generated_py" ^
    --hidden-import stage_pb2 ^
    --hidden-import stage_pb2_grpc ^
    --hidden-import grpc ^
    --hidden-import google.protobuf ^
    --collect-submodules google.protobuf ^
    --hidden-import grpc._cython.cygrpc ^
    stage_grpc_test_server.py

if errorlevel 1 (
    echo [ERROR] 打包失败
    popd >nul
    exit /b 1
)

if exist "build\debug\" (
    copy /Y "%DIST_DIR%\stage_grpc_test_server.exe" "build\debug\stage_grpc_test_server.exe" >nul 2>&1
)

echo [OK] 打包完成: %DIST_DIR%\stage_grpc_test_server.exe
popd >nul
exit /b 0

:try_python
set "CAND_PY_EXE=%~1"
set "CAND_PY_ARGS=%~2"

if "%CAND_PY_EXE%"=="" exit /b 0
if not exist "%CAND_PY_EXE%" exit /b 0

"%CAND_PY_EXE%" %CAND_PY_ARGS% -c "import grpc, google.protobuf" >nul 2>&1
if errorlevel 1 (
    exit /b 0
)

set "PYTHON_EXE=%CAND_PY_EXE%"
set "PYTHON_ARGS=%CAND_PY_ARGS%"
exit /b 0
