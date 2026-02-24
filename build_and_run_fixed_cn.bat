@echo off
chcp 936 >nul
echo 实时数据接收器构建脚本
echo.

REM 设置参数
set BUILD_TYPE=Release
set CLEAN_BUILD=0
set RUN_AFTER=0

REM 解析参数
:parse_args
if "%1"=="" goto :args_done
if /i "%1"=="-Debug" (
    set BUILD_TYPE=Debug
    shift
    goto :parse_args
)
if /i "%1"=="-Clean" (
    set CLEAN_BUILD=1
    shift
    goto :parse_args
)
if /i "%1"=="-Run" (
    set RUN_AFTER=1
    shift
    goto :parse_args
)
if /i "%1"=="-Help" (
    call :show_help
    exit /b 0
)
echo 未知参数: %1
call :show_help
exit /b 1

:show_help
echo 用法: build_and_run.bat [-Debug] [-Clean] [-Run] [-Help]
echo.
echo 参数:
echo   -Debug    : 构建调试版本 (默认: 发布版本)
echo   -Clean    : 清理构建文件
echo   -Run      : 构建成功后运行程序
echo   -Help     : 显示此帮助信息
echo.
echo 示例:
echo   build_and_run.bat           : 构建发布版本
echo   build_and_run.bat -Debug    : 构建调试版本
echo   build_and_run.bat -Clean    : 清理构建文件
echo   build_and_run.bat -Run      : 构建并运行发布版本
echo   build_and_run.bat -Clean -Run : 清理后构建并运行
goto :eof

:args_done
echo 构建类型: %BUILD_TYPE%

REM 1. 检查Visual Studio环境（优先使用环境变量）
echo.
echo [1/5] 设置Visual Studio开发环境...

REM 优先使用已配置的 VCVARS 路径
if defined VCVARS (
    echo 使用 VCVARS 环境变量: %VCVARS%
) else (
    REM 尝试使用 VSINSTALLDIR 环境变量
    if defined VSINSTALLDIR (
        set VCVARS="%VSINSTALLDIR%VC\Auxiliary\Build\vcvarsall.bat"
    )
)

REM 如果仍未找到，尝试常见安装路径（VS2019/VS2022）
if not defined VCVARS (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" (
        set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    )
)

if not defined VCVARS (
    echo 错误: 未能确定 vcvarsall.bat 的位置。请设置 VCVARS 或 VSINSTALLDIR 环境变量，或将 Visual Studio 安装路径添加到系统中。
    exit /b 1
)

if not exist %VCVARS% (
    echo 错误: 找到的 vcvarsall.bat 路径不存在: %VCVARS%
    exit /b 1
)

call %VCVARS% x64
if errorlevel 1 (
    echo VS环境设置失败
    exit /b 1
)
chcp 936 >nul
echo VS环境设置成功

REM 2. 检查Qt（优先使用环境变量或 PATH 中的 qmake）
echo.
echo [2/5] 检查Qt开发环境...

REM 如果外部已定义 QMAKE，优先使用
if defined QMAKE (
    echo 使用 QMAKE 环境变量: %QMAKE%
)

REM 否则尝试使用 QTDIR 环境变量
if not defined QMAKE if defined QTDIR (
    set QMAKE="%QTDIR%\bin\qmake.exe"
)

REM 否则尝试在 PATH 中查找 qmake
if not defined QMAKE (
    for /f "usebackq tokens=*" %%i in (`where qmake 2^>nul`) do (
        set QMAKE=%%i
        goto :qmake_found
    )
)
:qmake_found

if not defined QMAKE (
    REM 兜底旧的硬编码路径（保留以兼容旧环境）
    if exist "C:\Qt\Qt5.15.2\5.15.2\msvc2019_64\bin\qmake.exe" (
        set QMAKE="C:\Qt\Qt5.15.2\5.15.2\msvc2019_64\bin\qmake.exe"
    )
)

if not defined QMAKE (
    echo 错误: 未能找到 qmake。请设置 QTDIR/QMAKE 环境变量或将 Qt 的 bin 添加到 PATH。
    exit /b 1
)

if not exist %QMAKE% (
    echo 错误: 找到的 qmake 路径不存在: %QMAKE%
    exit /b 1
)

REM 3. 清理构建文件（如果指定）
if %CLEAN_BUILD%==1 (
    echo.
    echo [3/5] 清理构建文件...
    if exist build rmdir /s /q build
    if exist debug rmdir /s /q debug
    if exist release rmdir /s /q release
    if exist Makefile del /q Makefile 2>nul
    if exist Makefile.Debug del /q Makefile.Debug 2>nul
    if exist Makefile.Release del /q Makefile.Release 2>nul
    if exist .qmake.stash del /q .qmake.stash 2>nul
    echo 清理完成
)

REM 4. 运行qmake
echo.
echo [4/5] 生成Makefile...
%QMAKE% realtime_data.pro
if errorlevel 1 (
    echo qmake失败
    exit /b 1
)
echo qmake成功

REM 5. 运行nmake
echo.
echo [5/5] 编译%BUILD_TYPE%版本...
if "%BUILD_TYPE%"=="Release" (
    nmake release
) else (
    nmake debug
)

if errorlevel 1 (
    echo 编译失败
    exit /b 1
)
echo 编译成功

REM 6. 检查可执行文件
echo.
echo 构建完成!
if "%BUILD_TYPE%"=="Release" (
    set EXE_PATH=build\release\realtime_data.exe
) else (
    set EXE_PATH=build\debug\realtime_data.exe
)

if exist "%EXE_PATH%" (
    for %%F in ("%EXE_PATH%") do (
        set "FILE_SIZE=%%~zF"
    )
    REM 计算文件大小（MB）
    set /a FILE_SIZE_MB=FILE_SIZE / 1048576
    set /a FILE_SIZE_KB=(FILE_SIZE %% 1048576) / 1024
    echo 可执行文件: %EXE_PATH%
    if %FILE_SIZE_MB% gtr 0 (
        echo 文件大小: %FILE_SIZE_MB%.%FILE_SIZE_KB% MB
    ) else (
        set /a FILE_SIZE_KB=FILE_SIZE / 1024
        echo 文件大小: %FILE_SIZE_KB% KB
    )
    
    REM 7. 运行程序（如果指定）
    if %RUN_AFTER%==1 (
        echo.
        echo 启动程序...
        start "" "%EXE_PATH%"
        echo 程序已启动
    )
) else (
    echo 警告: 未找到可执行文件 %EXE_PATH%
)

echo.
echo 构建脚本执行完毕
pause
