# Stream Server 测试与运行指南

## 概述

本系统由两部分组成：
- **stream_server** (运行在 192.168.11.31)：接收视频流并进行硬件解码
- **ml_worker Docker 容器** (运行在 192.168.11.50)：从 Sunshine 主机抓取屏幕画面，通过 TCP 推流到 stream_server

网络拓扑：
```
┌─────────────────────┐        TCP 19000         ┌──────────────────────┐
│  192.168.11.50      │ ──────────────────────>   │  192.168.11.31       │
│  Docker ml_worker   │   H.264 视频流 (TLV)      │  stream_server       │
│  (推流端)           │                           │  (接收+硬件解码)      │
└─────────────────────┘                           └──────────────────────┘
        ▲                                                  │
        │  Moonlight 协议                                  │
┌───────┴─────────────┐                           GPU: NVIDIA / Intel
│  Sunshine 主机       │                           解码: NVDEC / VA-API
│  (屏幕源)           │
└─────────────────────┘
```

---

## 1. 构建 (在 192.168.11.31 上)

### 1.1 安装依赖

```bash
# 基础依赖 + FFmpeg
cd ~/work/my_ml_work/stream_server
./install_deps.sh

# NVIDIA 额外依赖 (如果需要 NVDEC)
./install_nvidia_deps.sh
```

### 1.2 编译

```bash
cd ~/work/my_ml_work/stream_server
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**检查 cmake 输出确认硬件加速启用：**
```
-- FFmpeg CUDA hwcontext supported - NVDEC enabled      ← 必须看到
-- VA-API found - Intel hwaccel enabled                  ← 必须看到
-- Hardware acceleration defines: HAVE_CUDA;HAVE_VAAPI   ← 两个都有
```

如果只看到空的 `Hardware acceleration defines:`，说明缺少 CUDA Toolkit 或 libva-dev，只能用 CPU 软解。

### 1.3 从开发机同步代码

如果在本地 (192.168.11.50) 开发，同步到 31：
```bash
# 在 192.168.11.50 上执行
rsync -avz --exclude='build/' \
  ~/work/my_ml_work/stream_server/ \
  192.168.11.31:~/work/my_ml_work/stream_server/
```

---

## 2. Docker Worker 配置 (在 192.168.11.50 上)

### 2.1 Worker 配置文件

位置：`~/work/my_ml_work/deploy/workers/worker00.conf`

```bash
# Sunshine 主机 (被抓取屏幕的机器)
HOST="192.168.11.50"
APP="Desktop"
IMAGE="ml-worker:latest"
WORKER_BIN=""

# 推流目标 (stream_server 的地址)
TCP_HOST="192.168.11.31"
TCP_PORT="19000"
STREAM_ID="1"

# 视频参数
WIDTH="1280"
HEIGHT="720"
FPS="60"
BITRATE="10000"
```

### 2.2 mlctl.sh 常用命令

```bash
cd ~/work/my_ml_work

# 启动推流
./deploy/mlctl.sh up worker00

# 停止推流
./deploy/mlctl.sh down worker00

# 重启推流
./deploy/mlctl.sh restart worker00

# 查看推流日志
./deploy/mlctl.sh logs worker00

# 查看 worker 状态
./deploy/mlctl.sh status worker00

# 首次使用需要先配对 Sunshine
./deploy/mlctl.sh pair worker00
```

### 2.3 确认推流正常

```bash
docker logs -f mlw-worker00
```

正常输出：
```
tcp_sender: connected to 192.168.11.31:19000
tcp_sender: stream_start sent (stream_id=1, 1280x720@60, 10000 kbps)
video worker ready: stream_id=1 -> 192.168.11.31:19000 (1280x720@60)
streaming started, press Ctrl+C to stop
[video] fps=60.0/60.0 mbps=4.00 delay=0.04/0.07ms buf=1024.0KB state=connected
[video] fps=60.0/60.0 mbps=4.02 delay=0.04/0.05ms buf=1024.0KB state=connected
```

关键检查项：
- `state=connected` — TCP 连接正常
- `fps=60.0/60.0` — 推流端稳定 60fps
- 没有 `state=reconnecting` — 没有断连

---

## 3. 测试流程

### 3.1 单路 VA-API 解码测试

**步骤 1：在 31 上启动服务器**
```bash
ssh 192.168.11.31
cd ~/work/my_ml_work/stream_server
DECODE_BACKEND=vaapi ./build/stream_server -p 19000 -c 20 -s 5
```

**步骤 2：在 50 上启动推流**
```bash
cd ~/work/my_ml_work
./deploy/mlctl.sh up worker00
```

**步骤 3：观察服务器输出**

正常输出：
```
[Server] Forcing Intel VA-API backend (from env)
[Decoder] Using H.264 decoder with VA-API hwaccel
[Decoder] VA-API device created: /dev/dri/renderD128
[Stream 1] Decoder initialized (Intel VA-API)
[Server] Stream 1 started: 1280x720@60fps

========== Stream Statistics ==========
Stream 01 (stream_01): ACTIVE, Frames: 300, Bytes: 2.72 MB, Decoded: 299, FPS: 60.0
  Video: 1280x720@60fps, 10000kbps
=======================================
```

**判定标准：** `FPS: 60.0`，`Frames` 和 `Decoded` 接近 1:1

**步骤 4：测试完毕清理**
```bash
# 50 上停止推流
./deploy/mlctl.sh down worker00

# 31 上 Ctrl+C 停止服务器
```

### 3.2 单路 NVIDIA NVDEC 解码测试

与 3.1 相同，只改环境变量：
```bash
# 31 上
DECODE_BACKEND=nvidia ./build/stream_server -p 19000 -c 20 -s 5
```

正常输出：
```
[Decoder] Created (NVIDIA NVDEC)
[Decoder] NVIDIA CUDA device created (GPU 0)
[Stream 1] Decoder initialized (NVIDIA NVDEC)

Stream 01 (stream_01): ACTIVE, Frames: 300, Bytes: 2.72 MB, Decoded: 299, FPS: 60.0
```

### 3.3 20 路压力测试 (NVIDIA)

**步骤 1：在 31 上启动压力测试模式**
```bash
DECODE_BACKEND=nvidia STRESS_TEST=1 STRESS_COPIES=20 \
  ./build/stream_server -p 19000 -c 20 -s 5
```

**步骤 2：在 50 上启动推流 (只需一路)**
```bash
./deploy/mlctl.sh up worker00
```

服务器收到第一帧后自动创建 20 个虚拟流并行解码。

**步骤 3：另开终端监控 GPU**
```bash
ssh 192.168.11.31 'watch -n 1 nvidia-smi'
```

**预期指标：**
| 指标 | 预期值 |
|------|--------|
| 每路 FPS | ~60.0 |
| GPU 利用率 | ~36% |
| CPU 利用率 | ~220% |
| Worker 断连 | 无 |

**步骤 4：Ctrl+C 停止服务器，查看汇总报告**

### 3.4 20 路压力测试 (VA-API)

```bash
DECODE_BACKEND=vaapi STRESS_TEST=1 STRESS_COPIES=20 \
  ./build/stream_server -p 19000 -c 20 -s 5
```

**预期指标：**
| 指标 | 预期值 |
|------|--------|
| 每路 FPS | ~60.0 |
| CPU 利用率 | ~430% |

### 3.5 自动化压力测试脚本

```bash
# 在 31 上执行，自动构建、监控 GPU、启动服务器、生成报告
cd ~/work/my_ml_work/stream_server
./stress_test_20streams.sh
```

---

## 4. 后台运行与日志收集

如果需要后台运行服务器并收集日志：

```bash
# 后台运行 (stdbuf -oL 确保日志实时刷新)
cd ~/work/my_ml_work/stream_server
DECODE_BACKEND=nvidia stdbuf -oL ./build/stream_server -p 19000 -c 20 -s 5 \
  > /tmp/stream_server.log 2>&1 &

# 查看实时日志
tail -f /tmp/stream_server.log

# 停止服务器
kill $(pgrep -f stream_server)
```

注意：C 程序的 stdout 在重定向到文件时默认全缓冲，必须加 `stdbuf -oL` 才能实时看到日志。不加的话日志文件会是空的，直到缓冲区满或进程退出。

---

## 5. 环境变量参考

| 变量 | 可选值 | 默认 | 说明 |
|------|--------|------|------|
| `DECODE_BACKEND` | `nvidia`, `vaapi`, `cpu` | 自动检测 | 指定解码后端 |
| `STRESS_TEST` | `1` / 不设置 | 不启用 | 开启压力测试模式 |
| `STRESS_COPIES` | 1-100 | `20` | 虚拟流数量 |

`DECODE_BACKEND` 的别名:
- NVIDIA: `nvidia`, `cuvid`
- Intel: `vaapi`, `intel`, `intel_va`, `qsv`
- CPU: `cpu`

---

## 6. 解读统计输出

### 正常统计
```
Stream 01 (stream_01): ACTIVE, Frames: 600, Bytes: 5.25 MB, Decoded: 599, FPS: 60.0
```

| 字段 | 含义 | 正常范围 |
|------|------|----------|
| Frames | 网络接收的帧数 | 持续增长 |
| Bytes | 网络接收的总字节 | 持续增长 |
| Decoded | 成功解码帧数 | 应接近 Frames (差 1-2 帧正常) |
| FPS | 实时解码帧率 | 59.0 - 61.0 |

### 异常判断

| 现象 | 说明 | 原因 |
|------|------|------|
| `FPS: 30.0` 且 `Decoded ≈ Frames/2` | 帧率减半 | parser 没有循环消费输入 |
| `FPS: 0.0` 且 `Decoded: 0` | 完全不解码 | 解码器初始化失败，检查 GPU 驱动 |
| `Frames` 增长但 `Decoded` 不增长 | 收到数据但解码失败 | 码流格式不对，或硬件资源耗尽 |
| Worker 日志显示 `state=reconnecting` | TCP 断连重连 | 服务器接收线程阻塞 |

---

## 7. 故障排查

### 7.1 服务器启动失败

**端口被占用：**
```bash
# 查看端口占用
ss -tuln | grep 19000
lsof -i :19000

# 强制释放
fuser -k 19000/tcp
```

**找不到可执行文件：**
```bash
ls -la build/stream_server    # 确认编译产物存在
cmake .. && make -j$(nproc)   # 重新编译
```

### 7.2 解码器初始化失败

**NVIDIA NVDEC：**
```bash
# 检查 GPU 是否可用
nvidia-smi

# 检查 FFmpeg 是否支持 CUDA
ffmpeg -hwaccels 2>/dev/null | grep cuda

# 检查 CUDA 头文件 (编译时需要)
ls /usr/local/cuda/include/cuda.h

# 检查编译时是否启用
grep HAVE_CUDA build/CMakeCache.txt
```

**Intel VA-API：**
```bash
# 检查 GPU 设备
ls -la /dev/dri/renderD128

# 检查 VA-API 信息
vainfo

# 检查用户权限
groups | grep -E 'video|render'

# 如果权限不足
sudo usermod -aG video,render $USER
# 需要重新登录生效

# 检查编译时是否启用
grep HAVE_VAAPI build/CMakeCache.txt
```

### 7.3 Worker 推流失败

**Docker 容器启动失败：**
```bash
# 查看容器状态
docker ps -a | grep mlw

# 查看详细错误
docker logs mlw-worker00

# 检查镜像是否存在
docker images | grep ml-worker
```

**TCP 连接不上：**
```bash
# 从 50 测试到 31 的连通性
nc -zv 192.168.11.31 19000

# 检查防火墙
ssh 192.168.11.31 'sudo iptables -L -n | grep 19000'

# 确认服务器正在监听
ssh 192.168.11.31 'ss -tuln | grep 19000'
```

**推流连接后立即断开：**
```bash
# 查看 worker 日志确认断连原因
docker logs -f mlw-worker00

# 常见原因: 服务器端 decode 阻塞 TCP 线程
# 确认使用了线程池 (压力测试模式下)
```

### 7.4 帧率只有 30fps

这是之前修复的核心 bug。如果复现：

```bash
# 检查服务器输出中 Frames 和 Decoded 的比例
# 如果 Frames:Decoded ≈ 2:1，说明 parser 未循环消费
# 正确的比例应该接近 1:1

# 检查 decoder.c 中 decoder_decode() 的 parser 循环:
# while (parse_remaining > 0) {
#     av_parser_parse2(...)
# }
# 如果没有 while 循环，就是这个问题
```

### 7.5 20 路压力测试 Worker 断连

```bash
# 检查是否用了线程池模式
# 服务器日志应该显示:
# [Stress] Created 20 virtual streams
# [Stress] Started 20 decode threads

# 如果没有线程池，串行解码会阻塞 TCP 接收线程
# 21 次解码 × 0.8ms/帧 ≈ 16.8ms > 16.6ms (60fps 帧间隔)
# 导致接收堆积 → TCP 缓冲满 → worker 超时断连
```

### 7.6 日志文件为空

```bash
# C 程序 stdout 重定向到文件时默认全缓冲
# 必须用 stdbuf 强制行缓冲
stdbuf -oL ./build/stream_server ... > /tmp/log.log 2>&1

# 或者不重定向，直接看终端输出
./build/stream_server -p 19000 -s 5
```

---

## 8. 完整测试脚本

以下是一键执行 VA-API + NVIDIA 双后端单路 60fps 验证的脚本：

```bash
#!/usr/bin/env bash
# test_both_backends.sh
# 在 192.168.11.50 (本机) 上执行
# 自动测试 VA-API 和 NVIDIA 两个后端的单路解码

set -uo pipefail

REMOTE="192.168.11.31"
REMOTE_DIR="~/work/my_ml_work/stream_server"
LOCAL_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_DIR="$LOCAL_DIR/../deploy"
PORT=19000
TEST_DURATION=15  # 每个后端测试秒数

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }

# 清理函数
cleanup() {
    echo "[cleanup] Stopping worker..."
    "$DEPLOY_DIR/mlctl.sh" down worker00 2>/dev/null || true
    echo "[cleanup] Stopping remote server..."
    ssh "$REMOTE" "pkill -f './build/stream_server' 2>/dev/null; true" 2>/dev/null || true
}
trap cleanup EXIT

# 测试单个后端
test_backend() {
    local backend="$1"
    local log_file="/tmp/${backend}_test.log"

    echo ""
    echo "============================================"
    echo "  Testing: $backend"
    echo "============================================"

    # 1. 启动远程服务器
    echo "[1/4] Starting server on $REMOTE ($backend)..."
    ssh "$REMOTE" "pkill -f './build/stream_server' 2>/dev/null; true" 2>/dev/null || true
    sleep 1
    ssh "$REMOTE" "cd $REMOTE_DIR && DECODE_BACKEND=$backend stdbuf -oL ./build/stream_server -p $PORT -c 20 -s 5 </dev/null > $log_file 2>&1 &"
    sleep 2

    # 验证服务器启动
    if ! ssh "$REMOTE" "ss -tuln | grep -q $PORT"; then
        fail "$backend: Server failed to start"
        ssh "$REMOTE" "cat $log_file" 2>/dev/null
        return 1
    fi

    # 2. 启动推流
    echo "[2/4] Starting worker..."
    "$DEPLOY_DIR/mlctl.sh" up worker00 2>/dev/null

    # 3. 等待测试
    echo "[3/4] Running for ${TEST_DURATION}s..."
    sleep "$TEST_DURATION"

    # 4. 检查结果
    echo "[4/4] Checking results..."
    local stats
    stats=$(ssh "$REMOTE" "tail -20 $log_file")

    # 提取 FPS
    local fps
    fps=$(echo "$stats" | grep -oP 'FPS: \K[0-9.]+' | tail -1)

    # 提取 Frames 和 Decoded
    local frames decoded
    frames=$(echo "$stats" | grep -oP 'Frames: \K[0-9]+' | tail -1)
    decoded=$(echo "$stats" | grep -oP 'Decoded: \K[0-9]+' | tail -1)

    echo "  FPS: ${fps:-N/A}"
    echo "  Frames: ${frames:-N/A}"
    echo "  Decoded: ${decoded:-N/A}"

    # 停止
    "$DEPLOY_DIR/mlctl.sh" down worker00 2>/dev/null || true
    ssh "$REMOTE" "pkill -f './build/stream_server' 2>/dev/null; true" 2>/dev/null || true
    sleep 2

    # 判定
    if [[ -n "$fps" ]] && (( $(echo "$fps >= 55.0" | bc -l) )); then
        pass "$backend: ${fps} fps (Frames=$frames, Decoded=$decoded)"
        return 0
    else
        fail "$backend: ${fps:-0} fps (expected >= 55)"
        echo "  Last log:"
        echo "$stats" | tail -10
        return 1
    fi
}

echo "=========================================="
echo "  Stream Server - Dual Backend Test"
echo "=========================================="
echo "Remote: $REMOTE"
echo "Duration per backend: ${TEST_DURATION}s"
echo "=========================================="

vaapi_result=0
nvidia_result=0

test_backend "vaapi"  || vaapi_result=1
test_backend "nvidia" || nvidia_result=1

echo ""
echo "=========================================="
echo "  Results"
echo "=========================================="
if [[ $vaapi_result -eq 0 ]]; then pass "VA-API:  60fps OK"; else fail "VA-API:  FAILED"; fi
if [[ $nvidia_result -eq 0 ]]; then pass "NVIDIA:  60fps OK"; else fail "NVIDIA:  FAILED"; fi
echo "=========================================="
```

使用方式 (在 192.168.11.50 上)：
```bash
cd ~/work/my_ml_work/stream_server
chmod +x test_both_backends.sh
./test_both_backends.sh
```

---

## 9. 服务器命令行参数速查

```
./build/stream_server [选项]

选项:
  -h, --host <addr>         绑定地址 (默认: 0.0.0.0)
  -p, --port <port>         监听端口 (默认: 9000)
  -c, --connections <n>     最大连接数 (默认: 20)
  -s, --stats-interval <s>  统计间隔秒数 (默认: 10)
  -d, --daemon              后台运行
  -v, --verbose             详细输出 (预留)

环境变量:
  DECODE_BACKEND=nvidia|vaapi|cpu    解码后端
  STRESS_TEST=1                      启用压力测试
  STRESS_COPIES=20                   虚拟流数量
```
