@echo off
setlocal
set ROOT=%~dp0
set PY=%ROOT%\.venv\Scripts\python.exe
if not exist "%PY%" set PY=python

rem Example:
rem   export_and_validate_aligned_sql.bat --source data\device_realtime_xxx.db --overwrite

"%PY%" "%ROOT%scripts\export_and_validate_aligned_sql.py" %*
set EXITCODE=%ERRORLEVEL%
exit /b %EXITCODE%