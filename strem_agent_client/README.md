# strem_agent_client

跨平台客户端（Windows/macOS/Linux）：

- 从 `strem_agent_server` 的 `video-port` 接收 H.264 AnnexB bytestream
- 解码并展示
- 监听键盘/鼠标事件，通过 TCP(control) 把输入事件发回 `strem_agent_server`

本目录先提供 Python 版本（最快跨平台落地）。

## Install

建议 Python 3.10+。

```bash
pip install -r requirements.txt
```

Windows：

```powershell
py -m pip install -r requirements.txt
```

## Run

```bash
python strem_agent_client.py --host 192.168.11.31 --video-port 31234 --ctrl-port 31235 --stream-id 1
```

如果启用了 token：

```bash
python strem_agent_client.py --host 192.168.11.31 --token xxx
```

## Late-join (REQ_IDR)

某些 Sunshine 场景下，推流过程中 IDR（关键帧）可能不周期性出现，导致“晚加入”客户端一直黑屏。

本项目在控制通道新增 `REQ_IDR`（命令值 `10`）：

- client 连上 ctrl 后会主动请求一次 IDR
- server 在新 SUB（video）连接时也会自动请求一次 IDR（即使 client 没连 ctrl）

如果你在 Windows IDE 启动，建议使用：

```bash
python ide_demo_client.py
```

---

Doc-Version: 0.2.0
Repo-Rev: 4e07aa7
