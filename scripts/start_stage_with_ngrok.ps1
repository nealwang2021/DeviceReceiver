#Requires -Version 5.1
<#
.SYNOPSIS
  启动本仓库 stage_grpc_test_server.py（仅本机 127.0.0.1）+ ngrok TCP，并打印外网可连的 host:port。

.DESCRIPTION
  适用于无公网 IPv4（CGNAT）场景：由 ngrok 分配公网 TCP 入口，映射到本机 Stage 桩端口。
  需已安装 ngrok CLI 并执行过: ngrok config add-authtoken <TOKEN>

.PARAMETER Port
  与 stage 桩及 ngrok 一致的本地端口（默认 50052）。

.PARAMETER NgrokExe
  ngrok 可执行文件名或路径（默认在 PATH 中查找 ngrok）。

.EXAMPLE
  .\scripts\start_stage_with_ngrok.ps1
  .\scripts\start_stage_with_ngrok.ps1 -Port 50053
#>
param(
    [int]$Port = 50052,
    [string]$NgrokExe = "ngrok"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

if (-not (Test-Path (Join-Path $RepoRoot "stage_grpc_test_server.py"))) {
    Write-Error "未找到 stage_grpc_test_server.py，请从仓库根目录通过 scripts\start_stage_with_ngrok 运行。"
    exit 1
}

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Host "[ERROR] 未找到 python，请先安装 Python 并加入 PATH。" -ForegroundColor Red
    exit 1
}

$ngrokResolved = $null
if (Test-Path -LiteralPath $NgrokExe) {
    $ngrokResolved = (Resolve-Path -LiteralPath $NgrokExe).Path
} else {
    $ngCmd = Get-Command $NgrokExe -ErrorAction SilentlyContinue
    if ($ngCmd) {
        $ngrokResolved = $ngCmd.Source
    }
}
if (-not $ngrokResolved) {
    Write-Host "[ERROR] 未找到 ngrok。请从 https://ngrok.com/download 安装，并执行:" -ForegroundColor Red
    Write-Host "        ngrok config add-authtoken <你的令牌>" -ForegroundColor Yellow
    exit 1
}

$stageArgs = @(
    "stage_grpc_test_server.py",
    "--host", "127.0.0.1",
    "--port", "$Port"
)

Write-Host "[INFO] 启动 Stage 桩: python $($stageArgs -join ' ')" -ForegroundColor Cyan
$stageProc = Start-Process -FilePath "python" -ArgumentList $stageArgs -WorkingDirectory $RepoRoot -PassThru -WindowStyle Normal

Start-Sleep -Seconds 1.8

Write-Host "[INFO] 启动 ngrok tcp $Port ..." -ForegroundColor Cyan
$ngrokProc = Start-Process -FilePath $ngrokResolved -ArgumentList @("tcp", "$Port") -WorkingDirectory $RepoRoot -PassThru -WindowStyle Minimized

$deadline = (Get-Date).AddSeconds(50)
$publicLine = $null
while ((Get-Date) -lt $deadline) {
    try {
        $r = Invoke-RestMethod -Uri "http://127.0.0.1:4040/api/tunnels" -TimeoutSec 2 -ErrorAction Stop
        foreach ($t in $r.tunnels) {
            if ($t.public_url -and ($t.public_url -like "tcp://*")) {
                $publicLine = $t.public_url -replace "^tcp://", ""
                break
            }
        }
        if ($publicLine) { break }
    } catch {
        # ngrok Web 界面尚未就绪
    }
    Start-Sleep -Milliseconds 450
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Green
Write-Host " Stage 桩 + ngrok TCP" -ForegroundColor Green
Write-Host "  本地: 127.0.0.1:$Port  （仅本机 + ngrok 连接）"
if ($publicLine) {
    Write-Host "  外网 gRPC 地址（填入 StageGrpcEndpoint）:" -ForegroundColor Green
    Write-Host "    $publicLine" -ForegroundColor Yellow
} else {
    Write-Host "  [WARN] 未能从 http://127.0.0.1:4040/api/tunnels 读取隧道。" -ForegroundColor Yellow
    Write-Host "         请打开 ngrok 窗口或浏览器访问 http://127.0.0.1:4040 查看 tcp:// 公网地址。"
}
Write-Host "  安全: gRPC 为明文，勿长期暴露不可信网络；结束请关闭本窗口或 Ctrl+C。" -ForegroundColor DarkGray
Write-Host "============================================================" -ForegroundColor Green
Write-Host ""
Write-Host "[INFO] 按 Ctrl+C 将尝试结束 ngrok 与 Stage 子进程..." -ForegroundColor Cyan

function Stop-Children {
    param([System.Diagnostics.Process]$Ng, [System.Diagnostics.Process]$St)
    if ($Ng -and -not $Ng.HasExited) {
        Stop-Process -Id $Ng.Id -Force -ErrorAction SilentlyContinue
    }
    if ($St -and -not $St.HasExited) {
        Stop-Process -Id $St.Id -Force -ErrorAction SilentlyContinue
    }
}

try {
    Wait-Process -Id $ngrokProc.Id -ErrorAction SilentlyContinue
} catch {
    # ignore
} finally {
    Stop-Children -Ng $ngrokProc -St $stageProc
    Write-Host "[INFO] 已结束子进程。" -ForegroundColor Cyan
}
