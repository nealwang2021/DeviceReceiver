@echo off
REM ============================================================================
REM  build_and_run.bat — qmake + nmake 传统构建（与 build_cmake.bat 二选一）
REM ----------------------------------------------------------------------------
REM  功能概要:
REM    - 使用 realtime_data.pro + 本机 Qt qmake 生成 Makefile 并编译
REM    - 可选启用 HDF5 / gRPC（CONFIG+=hdf5 / grpc_client），可传根目录
REM    - 可选 -Install* 通过 vcpkg 尝试安装依赖（脚本内逻辑）
REM  适用场景:
REM    - 习惯 Qt 工程 .pro 流程、或需与历史 qmake 脚本一致时使用
REM  输出:
REM    - 默认产物在 build\release\realtime_data.exe（依脚本内路径）
REM  若需 CMake + vcpkg 统一集成，优先使用 build_cmake.bat
REM  详细参数见脚本末尾 :show_help 或执行 -Help
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM 默认参数
set "BUILD_TYPE=Release"
set "CLEAN_BUILD=0"
set "RUN_AFTER=0"
set "ENABLE_HDF5=1"
set "INSTALL_HDF5=0"
set "ENABLE_GRPC=1"
set "INSTALL_GRPC=0"
set "HDF5_ROOT="
set "GRPC_ROOT="
set "VCPKG_ROOT="
REM 自动设置 vcpkg 路径和 HDF5_ROOT
if exist "D:\vcpkg\vcpkg.exe" (
    set "VCPKG_ROOT=D:\vcpkg"
    set "HDF5_ROOT=D:\vcpkg\installed\x64-windows"
    set "GRPC_ROOT=D:\vcpkg\installed\x64-windows"
)
if exist "C:\vcpkg\vcpkg.exe" (
    set "VCPKG_ROOT=C:\vcpkg"
    set "HDF5_ROOT=C:\vcpkg\installed\x64-windows"
    set "GRPC_ROOT=C:\vcpkg\installed\x64-windows"
)

REM 解析参数
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="-Debug" (
    set "BUILD_TYPE=Debug"
    shift
    goto :parse_args
)
if /I "%~1"=="-Clean" (
    set "CLEAN_BUILD=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-Run" (
    set "RUN_AFTER=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-NoHdf5" (
    set "ENABLE_HDF5=0"
    shift
    goto :parse_args
)
if /I "%~1"=="-NoInstallHdf5" (
    set "INSTALL_HDF5=0"
    shift
    goto :parse_args
)
if /I "%~1"=="-NoGrpc" (
    set "ENABLE_GRPC=0"
    shift
    goto :parse_args
)
if /I "%~1"=="-NoInstallGrpc" (
    set "INSTALL_GRPC=0"
    shift
    goto :parse_args
)
if /I "%~1"=="-Hdf5Root" (
    shift
    if "%~1"=="" (
        echo 参数错误: -Hdf5Root 需要路径值
        goto :show_help
    )
    set "HDF5_ROOT=%~1"
    shift
    goto :parse_args
)
if /I "%~1"=="-GrpcRoot" (
    shift
    if "%~1"=="" (
        echo 参数错误: -GrpcRoot 需要路径值
        goto :show_help
    )
    set "GRPC_ROOT=%~1"
    shift
    goto :parse_args
)
if /I "%~1"=="-Help" goto :show_help
if /I "%~1"=="-HDF5" (
    set "ENABLE_HDF5=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-HDF5Install" (
    set "INSTALL_HDF5=1"
    set "ENABLE_HDF5=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-GRPC" (
    set "ENABLE_GRPC=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-GRPCInstall" (
    set "INSTALL_GRPC=1"
    set "ENABLE_GRPC=1"
    shift
    goto :parse_args
)

echo 未知参数: %~1
set "ARG_PARSE_ERROR=1"
goto :show_help

:show_help
echo(用法: build_and_run_fixed_cn.bat [-Debug] [-Clean] [-Run] [-HDF5] [-HDF5Install] [-NoHdf5] [-NoInstallHdf5] [-Hdf5Root 路径] [-GRPC] [-GRPCInstall] [-NoGrpc] [-NoInstallGrpc] [-GrpcRoot 路径] [-Help]
echo(
echo(参数:
echo(  -Debug    构建调试版本 (默认: 发布版本)
echo(  -Clean    清理构建文件
echo(  -Run      构建成功后运行程序
echo(  -NoHdf5   禁用 HDF5 导出功能构建
echo(  -NoInstallHdf5 禁止自动安装 HDF5（仅探测本机）
echo(  -Hdf5Root 指定 HDF5 根目录（需包含 include\hdf5.h 和 lib）
echo(  -HDF5    启用 HDF5 支持（尝试检测 HDF5_ROOT 或 vcpkg）
echo(  -HDF5Install 启用并尝试通过 vcpkg 自动安装 HDF5
echo(  -GRPC    启用 gRPC Client 支持（尝试检测 GRPC_ROOT 或 vcpkg）
echo(  -GRPCInstall 启用并尝试通过 vcpkg 自动安装 gRPC
echo(  -NoGrpc  禁用 gRPC Client 支持构建
echo(  -NoInstallGrpc 禁止自动安装 gRPC（仅探测本机）
echo(  -GrpcRoot 指定 gRPC 根目录（需包含 include\grpcpp\grpcpp.h 和 lib）
echo(  -Help     Show help message
echo(
echo(示例:
echo(  build_and_run_fixed_cn.bat
echo(  build_and_run_fixed_cn.bat -Debug
echo(  build_and_run_fixed_cn.bat -Clean -Run
echo(  build_and_run_fixed_cn.bat -Hdf5Root C:\vcpkg\installed\x64-windows
echo(  build_and_run_fixed_cn.bat -GRPC -GrpcRoot D:\vcpkg\installed\x64-windows
echo(  build_and_run_fixed_cn.bat -HDF5 -GRPC
echo(  build_and_run_fixed_cn.bat -NoHdf5
if "%ARG_PARSE_ERROR%"=="1" (
    exit /b 1
)
exit /b 0

:args_done

REM --- 查找 vcvarsall.bat ---
if not defined VCVARS (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
            if exist "%%I\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%%I\VC\Auxiliary\Build\vcvarsall.bat"
        )
    )
)
if not defined VCVARS (
    for %%P in (
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    ) do if exist %%P set "VCVARS=%%~P"
)
if not defined VCVARS ( echo [ERROR] 未找到 vcvarsall.bat，请安装 VS C++ 工具链 & exit /b 1 )

call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 ( echo [ERROR] VS 环境设置失败 & exit /b 1 )

for /f "usebackq tokens=*" %%I in (`where qmake 2^>nul`) do if not defined QMAKE set "QMAKE=%%I"
if not defined QMAKE (
    if exist "C:\Qt\5.15.2\msvc2019_64\bin\qmake.exe" set "QMAKE=C:\Qt\5.15.2\msvc2019_64\bin\qmake.exe"
)
if not defined QMAKE ( echo [ERROR] 未找到 qmake & exit /b 1 )
if not exist "%QMAKE%" ( echo [ERROR] qmake 路径无效: %QMAKE% & exit /b 1 )
for %%D in ("%QMAKE%") do set "QT_BIN=%%~dpD"

if "%ENABLE_HDF5%"=="1" (
    if defined HDF5_ROOT (
        if not exist "%HDF5_ROOT%\include\hdf5.h" (
            echo [WARN] HDF5_ROOT 无效，禁用 HDF5
            set "ENABLE_HDF5=0"
        )
    ) else (
        set "ENABLE_HDF5=0"
    )
)

REM [Prebuild] 检查 gRPC 支持
if "%ENABLE_GRPC%"=="1" (
    if not defined GRPC_ROOT set "ENABLE_GRPC=0"
    if defined GRPC_ROOT (
        if not exist "%GRPC_ROOT%\include\grpcpp\grpcpp.h" (
            echo [WARN] GRPC_ROOT 无效，禁用 gRPC
            set "ENABLE_GRPC=0"
        )
    )
)

if "%CLEAN_BUILD%"=="1" (
    if exist build rmdir /s /q build >nul 2>&1
    if exist debug rmdir /s /q debug >nul 2>&1
    if exist release rmdir /s /q release >nul 2>&1
    del /q Makefile Makefile.Debug Makefile.Release .qmake.stash 2>nul
    if exist proto\generated\device.pb.h     del /q proto\generated\device.pb.h
    if exist proto\generated\device.pb.cc    del /q proto\generated\device.pb.cc
    if exist proto\generated\device.grpc.pb.h  del /q proto\generated\device.grpc.pb.h
    if exist proto\generated\device.grpc.pb.cc del /q proto\generated\device.grpc.pb.cc
    if exist proto\generated\stage.pb.h     del /q proto\generated\stage.pb.h
    if exist proto\generated\stage.pb.cc    del /q proto\generated\stage.pb.cc
    if exist proto\generated\stage.grpc.pb.h  del /q proto\generated\stage.grpc.pb.h
    if exist proto\generated\stage.grpc.pb.cc del /q proto\generated\stage.grpc.pb.cc
)

REM ============================================================
REM [Prebuild] 从 proto/device.proto 生成 protobuf/gRPC C++ 代码
REM 仅在 ENABLE_GRPC=1 时执行；若工具不可用则跳过并依赖已有生成文件
REM ============================================================
if "%ENABLE_GRPC%"=="1" (
    set "PROTO_DIR=%CD%\proto"
    set "PROTO_OUT=%CD%\proto\generated"
    set "PROTO_FILE=%CD%\proto\device.proto"
    set "PROTO_FILE_STAGE=%CD%\proto\stage.proto"
)
REM 延迟展开：检查 PROTO_FILE 是否存在（变量在 if 块内赋值，需用 !VAR! 读取）
if "%ENABLE_GRPC%"=="1" (
    if not exist "!PROTO_FILE!" (
        echo [WARN] 未找到 proto 文件: !PROTO_FILE!
        goto :after_protoc
    )

    REM --- 创建输出目录 ---
    if not exist "!PROTO_OUT!" mkdir "!PROTO_OUT!"

    REM --- 查找 protoc ---
    set "PROTOC_EXE="
    if defined GRPC_ROOT (
        if exist "%GRPC_ROOT%\tools\protobuf\protoc.exe" (
            set "PROTOC_EXE=%GRPC_ROOT%\tools\protobuf\protoc.exe"
        ) else if exist "%GRPC_ROOT%\bin\protoc.exe" (
            set "PROTOC_EXE=%GRPC_ROOT%\bin\protoc.exe"
        )
    )
    if not defined PROTOC_EXE (
        for /f "usebackq tokens=*" %%I in (`where protoc 2^>nul`) do (
            if not defined PROTOC_EXE set "PROTOC_EXE=%%I"
        )
    )
    if not defined PROTOC_EXE (
        echo [WARN] protoc 未找到，跳过代码生成，构建将依赖 proto\generated\ 中的已有文件
        goto :after_protoc
    )

    REM --- 查找 grpc_cpp_plugin ---
    set "GRPC_PLUGIN="
    if defined GRPC_ROOT (
        if exist "%GRPC_ROOT%\tools\grpc\grpc_cpp_plugin.exe" (
            set "GRPC_PLUGIN=%GRPC_ROOT%\tools\grpc\grpc_cpp_plugin.exe"
        ) else if exist "%GRPC_ROOT%\bin\grpc_cpp_plugin.exe" (
            set "GRPC_PLUGIN=%GRPC_ROOT%\bin\grpc_cpp_plugin.exe"
        )
    )
    if not defined GRPC_PLUGIN (
        for /f "usebackq tokens=*" %%I in (`where grpc_cpp_plugin 2^>nul`) do (
            if not defined GRPC_PLUGIN set "GRPC_PLUGIN=%%I"
        )
    )
    if not defined GRPC_PLUGIN (
        echo [WARN] grpc_cpp_plugin 未找到，跳过 gRPC 桩代码生成
        goto :after_protoc
    )

    "!PROTOC_EXE!" --proto_path="!PROTO_DIR!" --cpp_out="!PROTO_OUT!" "!PROTO_FILE!"
    if errorlevel 1 ( echo [ERROR] protoc --cpp_out 失败 & exit /b 1 )

    "!PROTOC_EXE!" --proto_path="!PROTO_DIR!" --grpc_out="!PROTO_OUT!" --plugin=protoc-gen-grpc="!GRPC_PLUGIN!" "!PROTO_FILE!"
    if errorlevel 1 ( echo [ERROR] protoc --grpc_out 失败 & exit /b 1 )

    if exist "!PROTO_FILE_STAGE!" (
        "!PROTOC_EXE!" --proto_path="!PROTO_DIR!" --cpp_out="!PROTO_OUT!" "!PROTO_FILE_STAGE!"
        if errorlevel 1 ( echo [ERROR] stage.proto protoc --cpp_out 失败 & exit /b 1 )

        "!PROTOC_EXE!" --proto_path="!PROTO_DIR!" --grpc_out="!PROTO_OUT!" --plugin=protoc-gen-grpc="!GRPC_PLUGIN!" "!PROTO_FILE_STAGE!"
        if errorlevel 1 ( echo [ERROR] stage.proto protoc --grpc_out 失败 & exit /b 1 )
    ) else (
        echo [WARN] 未找到 stage.proto，跳过 Stage gRPC 代码生成
    )
)
:after_protoc

REM --- qmake ---
set "QMAKE_CONFIGS="
if "%ENABLE_HDF5%"=="1" set "QMAKE_CONFIGS=%QMAKE_CONFIGS% CONFIG+=hdf5 HDF5_ROOT=\"%HDF5_ROOT%\""
if "%ENABLE_GRPC%"=="1" set "QMAKE_CONFIGS=%QMAKE_CONFIGS% CONFIG+=grpc_client GRPC_ROOT=\"%GRPC_ROOT%\""
set "QMAKE_ARGS=realtime_data.pro%QMAKE_CONFIGS%"
"%QMAKE%" %QMAKE_ARGS% >nul 2>&1
if errorlevel 1 (
    REM 重新跑一遍只为显示错误
    "%QMAKE%" %QMAKE_ARGS%
    echo [ERROR] qmake 失败
    exit /b 1
)

REM --- nmake: 捕获日志，失败时只显示 error/warning/fatal 行 ---
set "BUILD_LOG=%TEMP%\realtime_build.log"
if /I "%BUILD_TYPE%"=="Release" (
    nmake release > "%BUILD_LOG%" 2>&1
) else (
    nmake debug > "%BUILD_LOG%" 2>&1
)
if errorlevel 1 (
    findstr /I /C:"error" /C:"warning" /C:"fatal" /C:"unresolved" /C:"undefined" "%BUILD_LOG%"
    echo.
    echo [ERROR] 编译失败，完整日志: %BUILD_LOG%
    exit /b 1
)
del /q "%BUILD_LOG%" 2>nul

if /I "%BUILD_TYPE%"=="Release" (
    set "EXE_PATH=build\release\realtime_data.exe"
    set "EXE_DIR=%CD%\build\release"
) else (
    set "EXE_PATH=build\debug\realtime_data.exe"
    set "EXE_DIR=%CD%\build\debug"
)

echo [OK] Build: %BUILD_TYPE% — %EXE_PATH%

if exist "%QT_BIN%windeployqt.exe" (
    set "QT_DEPLOY_MODE=--release"
    if /I not "%BUILD_TYPE%"=="Release" set "QT_DEPLOY_MODE=--debug"
    "%QT_BIN%windeployqt.exe" %QT_DEPLOY_MODE% --compiler-runtime "%EXE_PATH%" >nul 2>&1
)

if "%ENABLE_HDF5%"=="1" if defined HDF5_ROOT (
    copy /Y "%HDF5_ROOT%\bin\*.dll" "%EXE_DIR%\" >nul 2>&1
)
if "%ENABLE_GRPC%"=="1" if defined GRPC_ROOT (
    copy /Y "%GRPC_ROOT%\bin\*.dll" "%EXE_DIR%\" >nul 2>&1
)

if "%RUN_AFTER%"=="1" if exist "%EXE_PATH%" start "" "%EXE_PATH%"
