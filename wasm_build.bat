@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  WebAssembly Build Script
REM  Usage: wasm_build.bat [clean|rebuild|serve|deploy]
REM    (none)  - incremental build
REM    clean   - clean build-wasm directory only
REM    rebuild - clean then full rebuild
REM    serve   - start local HTTP server for preview
REM    deploy  - copy output to docs/ for GitHub Pages
REM ============================================================

REM ---------- Path Configuration ----------
set QT_WASM_ROOT=C:\Qt\Qt5.15.2\5.15.2\wasm_32
set MY_EMSDK=C:\Qt\emsdk\emsdk
set EMSCRIPTEN=%MY_EMSDK%\upstream\emscripten
set BUILD_DIR=build-wasm
set PRO_FILE=realtime_data.pro
set SERVE_PORT=8000

REM ---------- Project Root ----------
set PROJECT_ROOT=%~dp0
cd /d "%PROJECT_ROOT%"

REM ---------- Parse Arguments ----------
if "%~1"=="clean" goto :do_clean
if "%~1"=="rebuild" goto :do_rebuild
if "%~1"=="serve" goto :do_serve
if "%~1"=="deploy" goto :do_deploy
goto :do_build

REM ============================================================
:do_clean
REM ============================================================
echo [CLEAN] Removing %BUILD_DIR% ...
if exist "%BUILD_DIR%" (
    rmdir /S /Q "%BUILD_DIR%"
    echo [CLEAN] Done.
) else (
    echo [CLEAN] %BUILD_DIR% does not exist, nothing to clean.
)
goto :eof

REM ============================================================
:do_rebuild
REM ============================================================
echo [REBUILD] Clean and rebuild ...
if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
goto :do_build

REM ============================================================
:do_serve
REM ============================================================
if not exist "%BUILD_DIR%\realtime_data.html" (
    echo [ERROR] Not built yet. Run wasm_build.bat first.
    pause
    exit /b 1
)
echo [SERVE] Starting HTTP server: http://localhost:%SERVE_PORT%/realtime_data.html
echo         Press Ctrl+C to stop.
cd /d "%BUILD_DIR%"
python -m http.server %SERVE_PORT%
cd /d "%PROJECT_ROOT%"
goto :eof

REM ============================================================
:do_deploy
REM ============================================================
if not exist "%BUILD_DIR%\realtime_data.html" (
    echo [ERROR] Not built yet. Run wasm_build.bat first.
    pause
    exit /b 1
)
set DOCS_DIR=docs
echo [DEPLOY] Copying build output to %DOCS_DIR%\ for GitHub Pages ...
if not exist "%DOCS_DIR%" mkdir "%DOCS_DIR%"

REM -- Copy static assets --
copy /Y "%BUILD_DIR%\realtime_data.js"   "%DOCS_DIR%\realtime_data.js"   >nul
copy /Y "%BUILD_DIR%\realtime_data.wasm" "%DOCS_DIR%\realtime_data.wasm" >nul
copy /Y "%BUILD_DIR%\qtloader.js"        "%DOCS_DIR%\qtloader.js"        >nul
copy /Y "%BUILD_DIR%\qtlogo.svg"         "%DOCS_DIR%\qtlogo.svg"         >nul

REM -- Copy HTML as index.html for clean URL --
copy /Y "%BUILD_DIR%\realtime_data.html" "%DOCS_DIR%\index.html"         >nul

REM -- .nojekyll prevents GitHub Pages from running Jekyll --
if not exist "%DOCS_DIR%\.nojekyll" type nul > "%DOCS_DIR%\.nojekyll"

echo.
echo ============================================================
echo  DEPLOY READY
echo ============================================================
echo.
echo  Folder: %DOCS_DIR%\
echo.
for %%F in (index.html realtime_data.js realtime_data.wasm qtloader.js qtlogo.svg .nojekyll) do (
    if exist "%DOCS_DIR%\%%F" (
        for %%A in ("%DOCS_DIR%\%%F") do (
            set "sz=%%~zA"
            set /a "kb=!sz!/1024"
            echo    %%F   !kb! KB
        )
    )
)
echo.
echo  Next steps:
echo    1. git add docs/
echo    2. git commit -m "deploy wasm to GitHub Pages"
echo    3. git push
echo    4. In GitHub repo Settings - Pages, set source to:
echo       Branch: main   Folder: /docs
echo.
pause
goto :eof

REM ============================================================
:do_build
REM ============================================================
echo ============================================================
echo  WebAssembly Build
echo ============================================================

REM ---------- Check Environment ----------
echo [CHECK] Qt WASM path ...
if not exist "%QT_WASM_ROOT%\bin\qmake.exe" (
    echo [ERROR] Qt for WebAssembly not found: %QT_WASM_ROOT%
    pause
    exit /b 1
)

echo [CHECK] emsdk ...
if not exist "%MY_EMSDK%\emsdk_env.bat" (
    echo [ERROR] emsdk_env.bat not found: %MY_EMSDK%
    pause
    exit /b 1
)

if not exist "%EMSCRIPTEN%\emconfigure.bat" (
    echo [ERROR] emscripten not found: %EMSCRIPTEN%
    pause
    exit /b 1
)

REM ---------- Activate emsdk ----------
echo [ENV] Activating emsdk ...
call "%MY_EMSDK%\emsdk_env.bat"
set PATH=%QT_WASM_ROOT%\bin;%PATH%

REM ---------- Create Build Directory ----------
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

REM ---------- qmake ----------
echo [CONFIG] emconfigure qmake ...
call "%EMSCRIPTEN%\emconfigure.bat" "%QT_WASM_ROOT%\bin\qmake.exe" "..\%PRO_FILE%" CONFIG+=wasm
if errorlevel 1 (
    echo [ERROR] qmake failed!
    cd /d "%PROJECT_ROOT%"
    pause
    exit /b 1
)

REM ---------- Compile ----------
echo [BUILD] emmake make -j4 ...
call "%EMSCRIPTEN%\emmake.bat" make -j4
if errorlevel 1 (
    echo [ERROR] Build failed!
    cd /d "%PROJECT_ROOT%"
    pause
    exit /b 1
)

REM ---------- Verify Output ----------
echo.
set BUILD_OK=1
for %%F in (realtime_data.html realtime_data.js realtime_data.wasm) do (
    if not exist "%%F" (
        echo [ERROR] Missing output: %%F
        set BUILD_OK=0
    )
)
if "!BUILD_OK!"=="0" (
    echo [ERROR] Build incomplete!
    cd /d "%PROJECT_ROOT%"
    pause
    exit /b 1
)

REM ---------- Print Results ----------
echo ============================================================
echo  BUILD SUCCESS
echo ============================================================
echo.
echo  Output: %BUILD_DIR%\
echo.
for %%F in (realtime_data.html realtime_data.js realtime_data.wasm qtloader.js) do (
    if exist "%%F" (
        for %%A in ("%%F") do (
            set "sz=%%~zA"
            set /a "kb=!sz!/1024"
            echo    %%F   !kb! KB
        )
    )
)
echo.
echo  Preview : wasm_build.bat serve
echo  Deploy  : wasm_build.bat deploy
echo  Clean   : wasm_build.bat clean
echo  Rebuild : wasm_build.bat rebuild
echo.

cd /d "%PROJECT_ROOT%"
pause
