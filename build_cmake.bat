@echo off
REM ============================================================================
REM  build_cmake.bat — CMake 一键配置/编译 DeviceReceiver（推荐主构建入口）
REM ----------------------------------------------------------------------------
REM  功能概要:
REM    - 自动探测 VS / vcpkg / Qt，调用 CMake 生成 NMake 或 Ninja 工程
REM    - 可选启用 HDF5、gRPC、WASM；默认 BUILD_TESTS=ON 时顺带编译三轴台 Qt Test
REM    - Release 成功后可 windeployqt，复制 HDF5/gRPC DLL 到 exe 目录
REM  输出:
REM    - 构建目录: 默认 .\build_cmake\（与 BUILD_DIR 变量一致）
REM    - 主程序:   build_cmake\build\release\realtime_data.exe（Debug 则为 debug）
REM  常用命令:
REM    build_cmake.bat
REM    build_cmake.bat -Debug -Run
REM    build_cmake.bat -Clean
REM    build_cmake.bat -NoGrpc -NoHdf5
REM  详细参数: 执行 build_cmake.bat -Help
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

REM 默认参数
set "BUILD_TYPE=Release"
set "BUILD_DIR=build_cmake"
set "CLEAN_BUILD=0"
set "RUN_AFTER=0"
set "ENABLE_HDF5=1"
set "ENABLE_GRPC=1"
set "ENABLE_WASM=0"
set "FAST_BUILD=0"
set "HDF5_ROOT="
set "GRPC_ROOT="
REM 保留环境变量中的 VCPKG_ROOT（全功能构建常用）
set "VS_VERSION=2019"
set "QT_DIR=C:\Qt\5.15.2\msvc2019_64"

REM 自动检测vcpkg路径（固定盘符优先于环境变量）
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
REM 若仍未设置 triplet 根目录，使用环境变量 VCPKG_ROOT
if not defined GRPC_ROOT (
    if defined VCPKG_ROOT (
        if exist "!VCPKG_ROOT!\vcpkg.exe" (
            set "HDF5_ROOT=!VCPKG_ROOT!\installed\x64-windows"
            set "GRPC_ROOT=!VCPKG_ROOT!\installed\x64-windows"
        ) else (
            echo [WARN] 环境变量 VCPKG_ROOT=!VCPKG_ROOT! 但未找到 vcpkg.exe，忽略
        )
    )
)

REM 解析参数
:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="-Debug" (
    set "BUILD_TYPE=Debug"
    shift
    goto :parse_args
)
if /I "%~1"=="-Release" (
    set "BUILD_TYPE=Release"
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
if /I "%~1"=="-NoGrpc" (
    set "ENABLE_GRPC=0"
    shift
    goto :parse_args
)
if /I "%~1"=="-Wasm" (
    set "ENABLE_WASM=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-Fast" (
    set "FAST_BUILD=1"
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
if /I "%~1"=="-VcpkgRoot" (
    shift
    if "%~1"=="" (
        echo 参数错误: -VcpkgRoot 需要路径值
        goto :show_help
    )
    set "VCPKG_ROOT=%~1"
    set "HDF5_ROOT=%~1\installed\x64-windows"
    set "GRPC_ROOT=%~1\installed\x64-windows"
    shift
    goto :parse_args
)
if /I "%~1"=="-VSVersion" (
    shift
    if "%~1"=="" (
        echo 参数错误: -VSVersion 需要版本号 (2017, 2019, 2022)
        goto :show_help
    )
    set "VS_VERSION=%~1"
    shift
    goto :parse_args
)
if /I "%~1"=="-Help" goto :show_help
if /I "%~1"=="-HDF5" (
    set "ENABLE_HDF5=1"
    shift
    goto :parse_args
)
if /I "%~1"=="-GRPC" (
    set "ENABLE_GRPC=1"
    shift
    goto :parse_args
)
echo 未知参数: %~1
set "ARG_PARSE_ERROR=1"
goto :show_help

:show_help
echo(
echo 用法: build_cmake.bat [-Debug ^| -Release] [-Clean] [-Run] [-NoHdf5] [-NoGrpc] [-Wasm] [-Fast] [-Hdf5Root 路径] [-GrpcRoot 路径] [-VcpkgRoot 路径] [-VSVersion 版本] [-Help]
echo(
echo 参数:
echo  -Debug     构建调试版本 (默认: 发布版本)
echo  -Release   构建发布版本
echo  -Clean     清理构建文件
echo  -Run       构建成功后运行程序
echo  -NoHdf5    禁用 HDF5 导出功能构建
echo  -NoGrpc    禁用 gRPC Client 支持构建
echo  -Wasm      启用 WebAssembly 构建 (WASM)
echo  -Fast      快速模式：仅构建主程序，跳过测试目标和部署步骤
echo  -Hdf5Root  指定 HDF5 根目录
echo  -GrpcRoot  指定 gRPC 根目录
echo  -VcpkgRoot 指定 vcpkg 根目录
echo  -VSVersion 指定 Visual Studio 版本 (2017, 2019, 2022)
echo  -Help      显示帮助信息
echo(
echo 默认无参为全功能构建 ^(gRPC+HDF5^)，需 vcpkg 在 D:\vcpkg 或 C:\vcpkg，或已设置环境变量 VCPKG_ROOT
echo(
echo 示例:
echo  build_cmake.bat
echo  build_cmake.bat -Debug -Run
echo  build_cmake.bat -Clean -NoHdf5
echo  build_cmake.bat -Fast
echo  build_cmake.bat -Hdf5Root C:\vcpkg\installed\x64-windows -GRPC
echo  build_cmake.bat -Wasm
echo  build_cmake.bat -VcpkgRoot D:\vcpkg -VSVersion 2022
if "%ARG_PARSE_ERROR%"=="1" (
    exit /b 1
)
exit /b 0

:args_done

REM --- 检查构建目录 ---
if "%CLEAN_BUILD%"=="1" (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%" >nul 2>&1
    if exist build rmdir /s /q build >nul 2>&1
    if exist debug rmdir /s /q debug >nul 2>&1
    if exist release rmdir /s /q release >nul 2>&1
    echo [INFO] 已清理构建目录
)

REM --- 查找 Visual Studio ---
echo [INFO] 查找 Visual Studio %VS_VERSION%...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] 未找到 vswhere.exe
    exit /b 1
)

REM 查找 VS 版本
if "%VS_VERSION%"=="2022" (
    set "VS_VERSION_ARG=2022"
    set "VS_VERSION_RANGE=[17.0,18.0)"
    set "CMAKE_GENERATOR=Visual Studio 17 2022"
) else if "%VS_VERSION%"=="2019" (
    set "VS_VERSION_ARG=2019"
    set "VS_VERSION_RANGE=[16.0,17.0)"
    set "CMAKE_GENERATOR=Visual Studio 16 2019"
) else if "%VS_VERSION%"=="2017" (
    set "VS_VERSION_ARG=2017"
    set "VS_VERSION_RANGE=[15.0,16.0)"
    set "CMAKE_GENERATOR=Visual Studio 15 2017"
) else (
    echo [WARN] 不支持的 VS 版本 %VS_VERSION%，使用 2019
    set "VS_VERSION=2019"
    set "VS_VERSION_ARG=2019"
    set "VS_VERSION_RANGE=[16.0,17.0)"
    set "CMAKE_GENERATOR=Visual Studio 16 2019"
)

set "VS_PATH="
set "VS_MAJOR="
set "PREFER_NMAKE=0"

for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -version "!VS_VERSION_RANGE!" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
    set "VS_PATH=%%I"
)

if not defined VS_PATH (
    echo [WARN] 未找到 Visual Studio %VS_VERSION_ARG%，尝试自动选择已安装版本...
    for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        set "VS_PATH=%%I"
    )
    for /f "usebackq tokens=1 delims=." %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2^>nul`) do (
        set "VS_MAJOR=%%I"
    )

    if "!VS_MAJOR!"=="17" (
        set "CMAKE_GENERATOR=Visual Studio 17 2022"
        set "VS_VERSION_ARG=2022"
    ) else if "!VS_MAJOR!"=="16" (
        set "CMAKE_GENERATOR=Visual Studio 16 2019"
        set "VS_VERSION_ARG=2019"
    ) else if "!VS_MAJOR!"=="15" (
        set "CMAKE_GENERATOR=Visual Studio 15 2017"
        set "VS_VERSION_ARG=2017"
    ) else if defined VS_MAJOR (
        echo [WARN] 检测到较新 Visual Studio 主版本 !VS_MAJOR!，优先使用 NMake Makefiles
        set "PREFER_NMAKE=1"
        set "VS_VERSION_ARG=!VS_MAJOR!"
    )
)

if not defined VS_PATH (
    echo [ERROR] 未找到可用 Visual Studio 安装路径
    exit /b 1
)

if not defined CMAKE_GENERATOR (
    echo [ERROR] 无法确定 CMake 生成器
    exit /b 1
)

if "!PREFER_NMAKE!"=="1" (
    set "CMAKE_GENERATOR=NMake Makefiles"
)

echo [INFO] 使用 Visual Studio %VS_VERSION_ARG%: !VS_PATH!
echo [INFO] 使用 CMake 生成器: !CMAKE_GENERATOR!

set "VCVARS=!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "!VCVARS!" (
    echo [ERROR] 未找到 vcvarsall.bat: !VCVARS!
    exit /b 1
)

echo [INFO] 设置 VS 环境: !VCVARS!
call "!VCVARS!" x64 >nul 2>&1
if errorlevel 1 (
    echo [ERROR] VS 环境设置失败
    exit /b 1
)

REM --- 检查 CMake ---
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] 未找到 cmake，请安装 CMake 并添加到 PATH
    exit /b 1
)

REM --- 全功能依赖检查（默认 ENABLE_GRPC/HDF5 为 1）---
if "%ENABLE_GRPC%"=="1" (
    if not defined GRPC_ROOT (
        echo [ERROR] 默认全功能构建需要 gRPC，但未设置 GRPC_ROOT。
        echo        请安装 vcpkg 至 D:\vcpkg 或 C:\vcpkg，或设置环境变量 VCPKG_ROOT，或使用 -VcpkgRoot / -GrpcRoot
        echo        若仅需无 gRPC 构建，请传入 -NoGrpc
        exit /b 1
    )
)
if "%ENABLE_HDF5%"=="1" (
    if not defined HDF5_ROOT (
        echo [ERROR] 默认全功能构建需要 HDF5，但未设置 HDF5_ROOT。
        echo        请安装 vcpkg 至 D:\vcpkg 或 C:\vcpkg，或设置环境变量 VCPKG_ROOT，或使用 -VcpkgRoot / -Hdf5Root
        echo        若仅需无 HDF5 构建，请传入 -NoHdf5
        exit /b 1
    )
)

REM --- 准备 CMake 选项 ---
set "CMAKE_OPTIONS="

if exist "%QT_DIR%\bin\qmake.exe" (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DCMAKE_PREFIX_PATH="%QT_DIR%""
) else (
    echo [WARN] 未找到指定 Qt 目录: %QT_DIR%
)

if "%ENABLE_HDF5%"=="0" (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_HDF5=OFF"
) else if defined HDF5_ROOT (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_HDF5=ON -DHDF5_ROOT="!HDF5_ROOT!""
) else (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_HDF5=OFF"
)

if "%ENABLE_GRPC%"=="0" (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_GRPC=OFF"
) else if defined GRPC_ROOT (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_GRPC=ON -DGRPC_ROOT="!GRPC_ROOT!""
) else (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_GRPC=OFF"
)

if "%ENABLE_WASM%"=="1" (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_WASM=ON"
) else (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DENABLE_WASM=OFF"
)

REM 三轴台 / Stage 自动化测试（tst_stage_integration、tst_stage_panel），需 gRPC 且非 WASM
if "%FAST_BUILD%"=="1" (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DBUILD_TESTS=OFF"
) else (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DBUILD_TESTS=ON"
)

if defined VCPKG_ROOT (
    set "CMAKE_OPTIONS=!CMAKE_OPTIONS! -DVCPKG_ROOT="!VCPKG_ROOT!""
)

REM --- 创建构建目录 ---
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"

REM --- 配置项目 ---
echo [INFO] 配置项目...
echo [INFO] CMake 选项: !CMAKE_OPTIONS!

set "CMAKE_ARCHITECTURE=x64"
set "USE_NMAKE=0"

REM 处理缓存中的旧生成器，避免配置冲突
if exist "CMakeCache.txt" (
    set "CACHE_GENERATOR="
    for /f "tokens=2 delims==" %%G in ('findstr /B /C:"CMAKE_GENERATOR:INTERNAL=" CMakeCache.txt') do (
        set "CACHE_GENERATOR=%%G"
    )
    if defined CACHE_GENERATOR (
        if /I not "!CACHE_GENERATOR!"=="!CMAKE_GENERATOR!" (
            echo [WARN] 检测到生成器变化: !CACHE_GENERATOR! -> !CMAKE_GENERATOR!，清理旧缓存
            del /q CMakeCache.txt >nul 2>&1
            if exist CMakeFiles rmdir /s /q CMakeFiles >nul 2>&1
        )
    )
)

REM 尝试配置
if /I "!CMAKE_GENERATOR!"=="NMake Makefiles" (
    cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %CMAKE_OPTIONS% > cmake_configure.log 2>&1
    if errorlevel 1 (
        type cmake_configure.log | findstr /I /C:"error" /C:"warning" /C:"fatal" /C:"CMakeError" /C:"CMakeWarning"
        echo [INFO] 完整日志: %BUILD_DIR%\cmake_configure.log
        cd ..
        exit /b 1
    )
    set "USE_NMAKE=1"
    set "CMAKE_GENERATOR=NMake Makefiles"
) else (
    cmake .. -G "!CMAKE_GENERATOR!" -A %CMAKE_ARCHITECTURE% %CMAKE_OPTIONS% > cmake_configure.log 2>&1
    if errorlevel 1 (
        echo [ERROR] CMake 配置失败 (尝试 !CMAKE_GENERATOR!)
        echo [INFO] 尝试其他生成器...

        del /q CMakeCache.txt >nul 2>&1
        if exist CMakeFiles rmdir /s /q CMakeFiles >nul 2>&1
        
        REM 尝试使用NMake Makefiles
        cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% %CMAKE_OPTIONS% > cmake_configure.log 2>&1
        if errorlevel 1 (
            type cmake_configure.log | findstr /I /C:"error" /C:"warning" /C:"fatal" /C:"CMakeError" /C:"CMakeWarning"
            echo [INFO] 完整日志: %BUILD_DIR%\cmake_configure.log
            cd ..
            exit /b 1
        )
        set "USE_NMAKE=1"
        set "CMAKE_GENERATOR=NMake Makefiles"
    )
)

echo [OK] CMake 配置成功
echo [INFO] 功能摘要: ENABLE_GRPC=%ENABLE_GRPC% ENABLE_HDF5=%ENABLE_HDF5% ENABLE_WASM=%ENABLE_WASM%
if "%FAST_BUILD%"=="1" echo [INFO]   FAST_BUILD=ON (skip tests/deploy)
if defined GRPC_ROOT echo [INFO]   GRPC_ROOT=!GRPC_ROOT!
if defined HDF5_ROOT echo [INFO]   HDF5_ROOT=!HDF5_ROOT!
if defined VCPKG_ROOT echo [INFO]   VCPKG_ROOT=!VCPKG_ROOT!

REM --- 构建项目 ---
echo [INFO] 构建项目 (%BUILD_TYPE%)...

if /I "!USE_NMAKE!"=="1" (
    if "%FAST_BUILD%"=="1" (
        cmake --build . --target realtime_data > cmake_build.log 2>&1
    ) else (
        cmake --build . > cmake_build.log 2>&1
    )
) else (
    if "%FAST_BUILD%"=="1" (
        cmake --build . --config %BUILD_TYPE% --target realtime_data > cmake_build.log 2>&1
    ) else (
        cmake --build . --config %BUILD_TYPE% > cmake_build.log 2>&1
    )
)

if errorlevel 1 (
    echo [ERROR] 构建失败
    type cmake_build.log | findstr /I /C:"error" /C:"warning" /C:"fatal" /C:"LNK" /C:"C1083"
    echo [INFO] 完整日志: %BUILD_DIR%\cmake_build.log
    cd ..
    exit /b 1
)

REM --- 构建 Stage 自动化测试目标（如果启用 gRPC 且非 WASM）---
if "%FAST_BUILD%"=="0" if /I "%ENABLE_GRPC%"=="1" (
    if /I "%ENABLE_WASM%"=="0" (
        echo [INFO] 构建 Stage 自动化测试目标...
        cmake --build . --target tst_stage_integration > stage_tests_build_integration.log 2>&1
        if errorlevel 1 (
            echo [ERROR] 构建 tst_stage_integration 失败
            type stage_tests_build_integration.log | findstr /I /C:"error" /C:"fatal" /C:"LNK"
            echo [INFO] 完整日志: %BUILD_DIR%\stage_tests_build_integration.log
            cd ..
            exit /b 1
        )

        cmake --build . --target tst_stage_panel > stage_tests_build_panel.log 2>&1
        if errorlevel 1 (
            echo [ERROR] 构建 tst_stage_panel 失败
            type stage_tests_build_panel.log | findstr /I /C:"error" /C:"fatal" /C:"LNK"
            echo [INFO] 完整日志: %BUILD_DIR%\stage_tests_build_panel.log
            cd ..
            exit /b 1
        )
    )
)

REM --- 确定可执行文件路径 ---
if /I "%BUILD_TYPE%"=="Release" (
    set "EXE_PATH=build\release\realtime_data.exe"
) else (
    set "EXE_PATH=build\debug\realtime_data.exe"
)

set "EXE_FULL_PATH="

REM 检查可执行文件是否存在
if exist "%EXE_PATH%" (
    for %%F in ("%EXE_PATH%") do set "EXE_FULL_PATH=%%~fF"
) else (
    echo [WARN] 可执行文件未找到: %EXE_PATH%
    echo [INFO] 尝试在构建目录中查找...
    dir /s /b *.exe
    for /f "delims=" %%F in ('dir /s /b *.exe 2^>nul') do (
        if "%%~nxF"=="realtime_data.exe" set "EXE_FULL_PATH=%%F" & goto :exe_found
    )
    echo [ERROR] 未找到可执行文件
    cd ..
    exit /b 1
)
:exe_found

if not defined EXE_FULL_PATH (
    echo [ERROR] 未找到可执行文件
    cd ..
    exit /b 1
)

for %%D in ("!EXE_FULL_PATH!") do set "EXE_DIR=%%~dpD"

echo [OK] 构建成功: !EXE_FULL_PATH!

REM --- 复制依赖DLL ---
if "%FAST_BUILD%"=="0" if "%ENABLE_WASM%"=="0" (
    REM 查找windeployqt
    for /f "usebackq tokens=*" %%I in (`where windeployqt 2^>nul`) do (
        set "WINDEPLOYQT=%%I"
    )

    if not defined WINDEPLOYQT if exist "%QT_DIR%\bin\windeployqt.exe" (
        set "WINDEPLOYQT=%QT_DIR%\bin\windeployqt.exe"
    )
    
    if defined WINDEPLOYQT (
        echo [INFO] 部署 Qt 运行时库...
        if /I "%BUILD_TYPE%"=="Release" (
            "!WINDEPLOYQT!" --release --compiler-runtime "!EXE_FULL_PATH!" >nul 2>&1
        ) else (
            "!WINDEPLOYQT!" --debug --compiler-runtime "!EXE_FULL_PATH!" >nul 2>&1
        )
        REM 测试可执行文件同样需要 platforms 等插件（否则双击 tst_stage_*.exe 报无法初始化平台插件）
        if exist "!EXE_DIR!tst_stage_integration.exe" (
            if /I "%BUILD_TYPE%"=="Release" (
                "!WINDEPLOYQT!" --release --compiler-runtime "!EXE_DIR!tst_stage_integration.exe" >nul 2>&1
            ) else (
                "!WINDEPLOYQT!" --debug --compiler-runtime "!EXE_DIR!tst_stage_integration.exe" >nul 2>&1
            )
        )
        if exist "!EXE_DIR!tst_stage_panel.exe" (
            if /I "%BUILD_TYPE%"=="Release" (
                "!WINDEPLOYQT!" --release --compiler-runtime "!EXE_DIR!tst_stage_panel.exe" >nul 2>&1
            ) else (
                "!WINDEPLOYQT!" --debug --compiler-runtime "!EXE_DIR!tst_stage_panel.exe" >nul 2>&1
            )
        )
    )
    
    REM 复制HDF5 DLL
    if "%ENABLE_HDF5%"=="1" if defined HDF5_ROOT (
        if exist "!HDF5_ROOT!\bin\*.dll" (
            copy /Y "!HDF5_ROOT!\bin\*.dll" "!EXE_DIR!" >nul 2>&1
            echo [INFO] 已复制 HDF5 DLL
        )
    )
    
    REM 复制gRPC DLL
    if "%ENABLE_GRPC%"=="1" if defined GRPC_ROOT (
        if exist "!GRPC_ROOT!\bin\*.dll" (
            copy /Y "!GRPC_ROOT!\bin\*.dll" "!EXE_DIR!" >nul 2>&1
            echo [INFO] 已复制 gRPC DLL
        )
    )
)

REM --- 返回项目根目录 ---
cd ..

REM --- 运行程序 ---
if "%RUN_AFTER%"=="1" (
    echo [INFO] 启动程序...
    if exist "!EXE_FULL_PATH!" (
        start "" "!EXE_FULL_PATH!"
    ) else (
        echo [WARN] 可执行文件不存在: !EXE_FULL_PATH!
    )
)

echo [OK] Build completed
echo [INFO] Build directory: %BUILD_DIR%
echo [INFO] Executable: !EXE_FULL_PATH!
if "%ENABLE_HDF5%"=="1" echo [INFO] HDF5: enabled
if "%ENABLE_GRPC%"=="1" echo [INFO] gRPC: enabled
if "%ENABLE_WASM%"=="1" echo [INFO] WASM: enabled
if "%FAST_BUILD%"=="1" echo [INFO] Fast mode: enabled
echo [INFO] Build type: %BUILD_TYPE%
exit /b 0
