# Stage 桩 + 内网穿透（ngrok）

在无 **公网 IPv4**（运营商 CGNAT）且无可用 **公网 IPv6** 时，无法通过路由器端口转发让互联网直连本机。可用 **ngrok TCP** 将本机 `stage_grpc_test_server` 暴露为公网 `host:port`（由 ngrok 分配）。

**ngrok 手动安装（Windows）分步说明（HTML）：** [docs/ngrok_manual_install_zh.html](../docs/ngrok_manual_install_zh.html)

## 前置

1. 安装 [ngrok](https://ngrok.com/download)，并加入 `PATH`。（详细步骤见上文 HTML）
2. 注册账号后在控制台复制 authtoken，执行：
   ```bat
   ngrok config add-authtoken <你的令牌>
   ```
3. 本机已安装 Python，且已 `pip install grpcio protobuf`。

## 启动（推荐）

在仓库根目录：

```bat
scripts\start_stage_with_ngrok.bat
```

或指定端口：

```bat
scripts\start_stage_with_ngrok.bat -Port 50052
```

脚本会：

- 在新窗口启动 `python stage_grpc_test_server.py --host 127.0.0.1 --port <Port>`（仅本机监听，供 ngrok 连入）；
- 启动 `ngrok tcp <Port>`；
- 从 `http://127.0.0.1:4040/api/tunnels` 读取 **公网 TCP 地址**，并打印为「外网 gRPC 地址」。

在 **另一台机器** 上的 `realtime_data` 中，将「三轴台」**StageGrpcEndpoint** 填为该 **host:port**（无需写 `tcp://` 前缀）。

## 安全说明

- 当前 gRPC 为 **insecure**（无 TLS），穿透后流量经 ngrok 中继，请勿用于不可信环境或长期公网暴露。
- 生产环境请使用 TLS、VPN 或专线。

## 仅本机枚举 IP（无穿透）

若仅需 **同一局域网** 访问，可直接：

```bat
python stage_grpc_test_server.py --external
```

将打印本机 IPv4 提示与防火墙说明（默认仍监听 `0.0.0.0`）。

## 故障排查

| 现象 | 处理 |
|------|------|
| 找不到 ngrok | 安装并配置 PATH，或 `-NgrokExe "C:\path\to\ngrok.exe"` |
| 4040 无隧道 | 等几秒重试；浏览器打开 `http://127.0.0.1:4040` 查看 |
| 端口被占用 | 换 `-Port 50053` 并同步修改客户端地址 |
| PowerShell 禁止脚本 | `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` |
