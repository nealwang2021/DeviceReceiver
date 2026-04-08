@echo off
setlocal

set "EP=%~1"
if "%EP%"=="" (
  set "EP=https://andres-unpecked-gary.ngrok-free.dev"
)

echo [INFO] checking endpoint: %EP%
python "%~dp0check_grpc_endpoint.py" --endpoint "%EP%" --mode auto --timeout 4
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
  echo [OK] grpc endpoint check passed.
) else (
  echo [FAIL] grpc endpoint check failed. code=%RC%
)
exit /b %RC%
