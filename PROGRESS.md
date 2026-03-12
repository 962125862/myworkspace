# NVIDIA NVDEC 解码适配 - 项目进度

## 项目架构

```
本机 Docker (Sunshine H.264流源)
  → ml_worker 容器 (moonlight客户端读流 + TCP推送)
    → 192.168.11.31 stream_server (TCP接收 + 硬件解码)
```

注：如果你使用 `strem_agent_server/strem_agent_client` 做远程代理（视频 + 控制），请参考 `deploy/RUNBOOK_strem_agent.md`。

- Docker/脚本控制: `deploy/mlctl.sh`, worker配置在 `deploy/workers/worker00.conf`
- 推流目标: `TCP_HOST=192.168.11.31`, `TCP_PORT=19000`
- 流参数: 1280x720@60fps, H.264, 10Mbps

## 已完成

### 1. CMakeLists.txt 添加 CUDA 支持
- 自动检测 `/usr/local/cuda*/include/cuda.h` 路径
- 解决 FFmpeg 的 `hwcontext_cuda.h` 依赖 `cuda.h` 找不到的问题
- 同时支持 `HAVE_CUDA` 和 `HAVE_VAAPI` 编译定义

### 2. decoder.c NVIDIA CUDA 初始化
- 添加 `cuda_get_format` 回调函数（与 `vaapi_get_format` 对齐）
- 手动创建 CUDA hw_frames_ctx, 帧池 `initial_pool_size = 17`
- CUDA 初始化设置 `get_format` 回调 + `hw_device_ctx`

### 3. 31 机器编译部署
- 环境: Tesla T10, CUDA 12.2, 驱动 535.247.01, FFmpeg 60.31.102
- cmake 检测通过: `HAVE_CUDA;HAVE_VAAPI`
- 全部 target 编译成功

### 4. 端到端验证
- 本机 Docker 推流稳定 60fps/4.3Mbps
- 31 机器 stream_server 接收并用 NVDEC 解码成功
- `DECODE_BACKEND=nvidia` 环境变量控制后端

### 5. 20路文件压力测试通过
- 总吞吐量 1510.6 FPS, 每路 75.5 FPS
- 0 丢帧, GPU 利用率仅 5%, 温度 37°C

## 当前问题

### 解码输出 30 FPS (预期 60 FPS)

日志表现:
```
Stream 01: ACTIVE, Frames: 2593, Decoded: 1296, FPS: 30.0
```

接收帧数与解码帧数比例约 2:1。用户反馈 VA-API 可以拉满 60fps。

**可能原因:**
1. `decoder_decode` 函数每次 `send_packet` 后只调用一次 `receive_frame`, 如果 CUDA 解码器内部有缓冲, 会丢失帧
2. VA-API 路径设置了 `get_buffer2 = hw_get_buffer`, CUDA 路径没有设置, 可能导致帧分配方式不同

**排查方向:**
- 对比 VA-API 实时流解码是否真的 60fps
- 如果 VA-API 也是 30fps, 说明是流本身或 `decoder_decode` 逻辑的问题
- 如果只有 CUDA 是 30fps, 检查 CUDA 解码器是否需要循环 `receive_frame`

**修复思路:** 修改 `decoder_decode` 在 `send_packet` 后循环调用 `receive_frame`:
```c
// 当前: 只取一帧
ret = avcodec_receive_frame(ctx->codec_ctx, ctx->frame);

// 改为: 循环取帧直到 EAGAIN
while (avcodec_receive_frame(ctx->codec_ctx, ctx->frame) == 0) {
    // 处理帧...
}
```

## 文件修改清单

| 文件 | 修改 |
|------|------|
| `stream_server/CMakeLists.txt` | CUDA路径检测, HWACCEL_INCLUDES, target编译定义 |
| `stream_server/src/decoder.c` | 添加cuda_get_format回调, 修改CUDA初始化逻辑 |
| `stream_server/tests/CMakeLists.txt` | 移除重复test_20streams_hw target |
| `stream_server/install_nvidia_deps.sh` | 新增NVIDIA环境安装脚本 |
| `stream_server/stream_receiver_decode.c` | 新增独立接收+解码测试程序(有编译错误未修复) |

## 下次继续

1. 先用 `DECODE_BACKEND=vaapi` 跑实时流, 确认 VA-API 是否真的 60fps
2. 根据对比结果修复 CUDA 30fps 问题
3. `stream_receiver_decode.c` 编译错误需要修复(协议结构体不匹配)

---

Doc-Version: 0.1.1
Repo-Rev: 4e07aa7
