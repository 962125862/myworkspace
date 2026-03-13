# Web WebRTC + WS 控制 (MVP) 运维手册

本文档整理了本次排障过程中遇到的问题，并给出一套可重复执行的启动、重建、打包镜像流程。

仓库根目录：`/home/gejun/work/my_ml_work`  
本栈目录：`/home/gejun/work/my_ml_work/web_webrtc`

## 架构概览（谁连谁）

### 视频链路

1. `strem_agent_server` 在 TCP 视频端口（默认 `31234`）输出 H264 AnnexB，客户端需发送 `SUB <stream_id>\n` 订阅。
2. `republisher` 从 `strem_agent_server` 拉取 H264，并通过 RTSP 推给 MediaMTX（本机 `34568`）。
3. `mediamtx` 提供 WebRTC 播放：
   - WebRTC HTTP 页面：`34569/tcp`
   - ICE：`34570/udp`（主）以及 `34570/tcp`（fallback）

### 控制链路（鼠标/键盘）

1. 浏览器连 `ws_ctrl_bridge`：`ws://<host>:34567/ws`
2. `ws_ctrl_bridge` 把输入事件转成 MLCT 包，通过 TCP 发到 `strem_agent_server` 控制端口（默认 `31235`）
3. `strem_agent_server` 再转发到 `ml_worker`（注入到主机）

## 遇到的问题（现象与根因）

1. 黑屏/无画面（WebRTC 连不上）
   - 现象：MediaMTX 日志出现 `deadline exceeded while waiting connection`
   - 根因：MediaMTX 广播给浏览器的 ICE candidates 是 `127.0.0.1` 或内网 `192.168.x.x`，外网客户端不可达
   - 处理：设置 `PUBLIC_HOST`（公网 IP 或域名），让 MediaMTX 把公网地址带进 candidates
     - 已通过 `.env` 持久化，避免下次 `up` 忘记带环境变量导致再次黑屏

2. `docker-compose` v1 报 `'ContainerConfig'`
   - 现象：`docker-compose up ...` 失败，报 `'ContainerConfig'`
   - 根因：`docker-compose` v1.29.2 太老，和本机 Docker Engine 28.x 组合容易不兼容
   - 处理：安装并改用 Compose v2（`docker compose ...`）

3. macOS `Cmd+C/V/...` 快捷键不好用
   - 根因：
     - 前端 `preventDefault()` 拦截了按键，但没有把修饰键（ctrl/meta/alt/shift）一起转发
     - 后端发送按键时 modifiers 固定为 0
   - 处理：
     - 前端增加 `modifiers` 位掩码上报
     - 后端转发 modifiers，并默认把 macOS 的 `META(Command)` 映射成 Windows 的 `CTRL`，让 `Cmd+C` 等价于远端 `Ctrl+C`

4. 上游几分钟不发 IDR（关键帧太稀）
   - 风险：晚加入或偶发丢包后，画面恢复会等到下一次 IDR，体验很差
   - 处理：
     - viewer 连接时发送 `REQ_IDR` burst（多次请求）
     - 可选开启周期性 `REQ_IDR` 兜底（默认关闭）

5. 画面流畅但鼠标“不跟手”
   - 常见根因：体感“输入慢”往往被视频端到端延迟主导（控制已到达，但你要等延迟后的视频帧才能看到反馈）
   - 处理（已做）：
     - 鼠标 move 事件合并：每帧最多发送一次，只发最新坐标
     - WebSocket 发送缓冲积压时丢弃中间 move，避免越积越慢

6. `webrtc-internals` 里 jitter buffer 指标“越来越高”
   - 说明：`jitterBufferDelay/jitterBufferMinimumDelay/jitterBufferTargetDelay` 很多都是累计量，时间越长数值越大是正常现象
   - 正确看法：算平均缓冲
     - 平均 jitter buffer（秒）=`jitterBufferDelay / jitterBufferEmittedCount`
   - 若 `jitter` 本身很大（例如 0.1s+），接收端会增加缓冲避免卡顿，这通常需要从网络/码率/编码器参数入手

## 本次改动点（做了哪些优化/修复）

- `web_webrtc/.env`
  - `PUBLIC_HOST=124.90.118.111`（当前公网 IP）

- `web_webrtc/docker-compose.yml`
  - 增加 IDR burst/周期性 IDR 的环境变量
  - 增加 macOS `Command -> Ctrl` 映射开关

- `web_webrtc/ws_ctrl_bridge/server.js`
  - 连接时 IDR burst + 可选周期性 `REQ_IDR`
  - 键盘 modifiers 转发；默认 macOS `META -> CTRL` 映射（`MAP_MAC_META_TO_CTRL`）

- `web_webrtc/ws_ctrl_bridge/web/index.html`
  - 键盘：发送 modifiers
  - 鼠标：move 合并 + 背压丢包（不排队）

- `web_webrtc/republisher/republish.py`
  - ffmpeg 增加时间戳生成/携带与更低 mux 缓冲（降低下游 jitter buffer 的压力）

## 一次性准备

### 1) 配好公网地址（必须）

检查 `.env`：

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
cat .env
```

期望：

```text
PUBLIC_HOST=124.90.118.111
```

如果公网 IP 会变，建议换成域名（或 DDNS）。

### 2) （可选）HTTP 代理（1095）

```bash
export HTTP_PROXY=http://127.0.0.1:1095
export HTTPS_PROXY=http://127.0.0.1:1095
export NO_PROXY=localhost,127.0.0.1,::1,192.168.0.0/16
```

验证：

```bash
curl -I -x http://127.0.0.1:1095 https://github.com
```

### 3) 安装 Compose v2（如果没有 `docker compose`）

```bash
sudo mkdir -p /usr/local/lib/docker/cli-plugins
sudo -E curl -L https://github.com/docker/compose/releases/download/v2.27.1/docker-compose-linux-x86_64 \
  -o /usr/local/lib/docker/cli-plugins/docker-compose
sudo chmod +x /usr/local/lib/docker/cli-plugins/docker-compose
docker compose version
```

## 启动/停止（日常操作）

### 启动全部服务（并构建镜像）

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --build
```

### 查看状态

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose ps
```

### 看日志

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose logs -f --tail 100 mediamtx
docker compose logs -f --tail 100 republisher
docker compose logs -f --tail 100 ws_ctrl_bridge
```

### 停止全部服务

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose down
```

## 重建/重启（打包镜像 + 应用变更）

代码改动后建议用下面的套路：`up -d --build --force-recreate`。

### 只重建并重启 ws_ctrl_bridge（最快）

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --build --no-deps --force-recreate ws_ctrl_bridge
```

### 只重建并重启 republisher

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --build --no-deps --force-recreate republisher
```

### 只重启 mediamtx（当 PUBLIC_HOST 变化时）

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose up -d --no-deps --force-recreate mediamtx
```

### 只打包镜像（不启动）

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
docker compose build
```

## 常用环境变量（关键调参）

### MediaMTX 公网候选地址

- `.env`：`PUBLIC_HOST=<公网IP或域名>`
- compose 会把它传给 `MTX_WEBRTCADDITIONALHOSTS`

### IDR 请求策略（ws_ctrl_bridge）

- `REQ_IDR_ON_CONNECT_BURST`（默认 `3`）
- `REQ_IDR_BURST_INTERVAL_MS`（默认 `500`）
- `REQ_IDR_PERIOD_SEC`（默认 `0`，设成 `2` 表示每 2 秒请求一次 IDR）

示例：

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
REQ_IDR_PERIOD_SEC=2 docker compose up -d --no-deps --force-recreate ws_ctrl_bridge
```

### macOS 快捷键映射

- `MAP_MAC_META_TO_CTRL`（默认 `1`）
  - `1`：将 macOS `Command` 映射为远端 Windows `Ctrl`（你要 `Cmd+C/V` 时用）
  - `0`：不映射，保留 `META`

示例：

```bash
cd /home/gejun/work/my_ml_work/web_webrtc
MAP_MAC_META_TO_CTRL=0 docker compose up -d --no-deps --force-recreate ws_ctrl_bridge
```

## 管理接口（解封黑名单）

`ws_ctrl_bridge` 内置了一个管理接口用于解封被 ban 的 IP（内存态；重启容器也会清空）。

启用方式：
- 设置环境变量 `ADMIN_SECRET`（推荐）
- 或者不设置 `ADMIN_SECRET`，则会回退使用 `TOKEN_ISSUE_SECRET`

解封单个 IP：

```bash
curl -sS -X POST 'http://127.0.0.1:34567/admin/unban?ip=1.2.3.4' \
  -H 'X-Admin-Secret: <ADMIN_SECRET>' 
```

清空全部 ban/bad-token 计数：

```bash
curl -sS -X POST 'http://127.0.0.1:34567/admin/unban?all=1' \
  -H 'X-Admin-Secret: <ADMIN_SECRET>' 
```

## 故障排查速查

### A) 黑屏

1. `docker compose ps` 确认 `mediamtx` 在跑
2. 看日志是否 ICE 失败（`deadline exceeded while waiting connection`）
3. 检查 `.env` 里的 `PUBLIC_HOST` 是否正确
4. 放通端口：`34569/tcp`、`34570/udp`、`34570/tcp`

### B) 能看画面但延迟涨/控制不跟手

1. 打开 `chrome://webrtc-internals/` 看 `jitter` 和平均 jitter buffer
2. 如果 `jitter` 高，优先降码率/分辨率（通常最有效）
3. 如果 IDR 稀疏，开启 `REQ_IDR_PERIOD_SEC` 做兜底
