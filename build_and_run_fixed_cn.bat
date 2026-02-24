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

REM 1. 检查Visual Studio环境
echo.
echo [1/5] 设置Visual Studio开发环境...
::set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not exist %VCVARS% (
    echo 错误: 找不到 vcvarsall.bat
    exit /b 1
)

call %VCVARS% x64
if errorlevel 1 (
    echo VS环境设置失败
    exit /b 1
)
chcp 936 >nul
echo VS环境设置成功

REM 2. 检查Qt
echo.
echo [2/5] 检查Qt开发环境...
::set QMAKE="C:\Qt\5.15.2\msvc2019_64\bin\qmake.exe"
set QMAKE="C:\Qt\Qt5.15.2\5.15.2\msvc2019_64\bin\qmake.exe"
if not exist %QMAKE% (
    echo 错误: 找不到 qmake.exe
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
