@echo off
REM ============================================================================
REM  start_stage_with_ngrok.bat — 启动 Stage 桩 + ngrok TCP，打印外网 host:port
REM ----------------------------------------------------------------------------
REM  功能: 调用 scripts\start_stage_with_ngrok.ps1（默认端口 50052）
REM  前置: 安装 Python、ngrok CLI；执行 ngrok config add-authtoken <令牌>
REM  说明: 适用于无公网 IPv4（CGNAT）；外网地址由 ngrok 分配，脚本从 4040 接口读取
REM  参数: 透传给 PowerShell，例如: start_stage_with_ngrok.bat -Port 50053
REM ============================================================================
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0.."

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_stage_with_ngrok.ps1" %*
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" (
    echo [INFO] 若提示无法加载脚本，可执行:  Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
    exit /b %ERR%
)
exit /b 0
