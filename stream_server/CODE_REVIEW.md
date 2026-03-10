# Code Review 记录

## CR 概要

| 项目 | 数量 |
|------|------|
| 修复的 Bug | 4 |
| 删除的无用代码 | 5 处 |
| 修复的编译警告 | 4 个 |
| 潜在风险 | 3 个 |

---

## 已修复的 Bug

### Bug 1: av_parser_parse2 未循环消费 — 帧率减半

**严重度：** 高  
**影响：** 所有后端 (NVIDIA/VA-API/CPU) 均只有 30fps，应为 60fps  
**文件：** `src/decoder.c` - `decoder_decode()`

**根因：** `av_parser_parse2()` 每次调用只消费部分输入数据。H.264 parser 需要看到下一帧的 start code 才能输出当前帧。旧代码只调用一次 parser，剩余数据被丢弃，导致精确的 2:1 帧丢失比 (Frames:Decoded = 2:1)。

**修复：** 将 parser 调用改为 `while (parse_remaining > 0)` 循环，直到输入数据全部消费完毕。

### Bug 2: NVIDIA CUDA 手动创建 hw_frames_ctx 干扰 NVDEC

**严重度：** 高  
**影响：** CUDA 后端解码失败或不稳定  
**文件：** `src/decoder.c` - `decoder_init()`

**根因：** 旧代码在 `cuda_get_format()` 中手动调用 `av_hwframe_ctx_alloc()` + `av_hwframe_ctx_init()` 创建硬件帧上下文，这会干扰 NVDEC 的内部帧管理。参考 moonlight-qt 的实现，NVIDIA CUDA 只需设置 `hw_device_ctx` 和简单的 `get_format` 回调，让 FFmpeg/NVDEC 自行管理 `hw_frames_ctx`。

**修复：** 
- 删除 `cuda_get_format()` 函数
- CUDA 初始化改为只设置 `hw_device_ctx` + `get_format` + `extra_hw_frames = 4`

### Bug 3: bytes_in 双重计数

**严重度：** 中  
**影响：** 解码统计中的输入字节数被多次累加  
**文件：** `src/decoder.c` - `decoder_decode()`

**根因：** `ctx->stats.bytes_in += size` 放在 `receive_frame` 循环内部，如果一次 `decoder_decode` 调用产生多帧，`size` 会被累加多次。

**修复：** 将 `bytes_in` 累加移到 parser 循环外部，且仅在有帧输出时计算一次。

### Bug 4: 压力测试串行解码阻塞 TCP 接收

**严重度：** 高  
**影响：** 20 路压力测试时 Docker worker 频繁断连重连  
**文件：** `src/stream.c`

**根因：** 旧实现在 TCP 接收线程中串行解码 21 路流 (1 源流 + 20 虚拟流)，每帧需要 21 次解码操作，超过 16.6ms 帧间隔，导致 TCP 接收阻塞、worker 超时断连。

**修复：** 实现线程池架构，每个虚拟流有独立解码线程。`stream_stress_test_decode()` 只做非阻塞数据分发 (memcpy + signal)，不在 TCP 线程中解码。

---

## 删除的无用代码

| # | 位置 | 内容 | 说明 |
|---|------|------|------|
| 1 | `decoder.c` | `cuda_get_format()` 函数 | 整个函数被 `#if 0` 包裹，已用 `get_hw_format` 替代 |
| 2 | `decoder.c` struct | `AVFrame* frame_hw` 字段 | DecoderCtx 中未使用的硬件帧 |
| 3 | `decoder.c` struct | `enum AVPixelFormat sw_pix_fmt` 字段 | 未使用的软件像素格式 |
| 4 | `server.h` struct | `worker_threads` / `worker_count` 字段 | TcpServer 中从未赋值/使用的字段 |
| 5 | `main.c` | `int verbose` 变量 | 设置但从未读取 |

---

## 修复的编译警告

| # | 警告信息 | 文件 | 修复方式 |
|---|---------|------|---------|
| 1 | `variable 'verbose' set but not used` | `main.c` | 删除变量，`-v` 选项改为空操作预留 |
| 2 | `'key_frame' is deprecated` (2处) | `decoder.c:536,645` | 改用 `!!(frame->flags & AV_FRAME_FLAG_KEY)` |
| 3 | `'__builtin_strncpy' output may be truncated` | `decoder.c` | 改用 `snprintf(stats->name, sizeof(stats->name), "%s", name)` |

---

## 潜在风险

### 风险 1: 未对齐内存访问 (protocol.c)

```c
header->length = BE32(*(uint32_t*)buf);
header->stream_id = BE16(*(uint16_t*)(buf + 5));
```

**说明：** 在 `buf + 5` 处直接 cast 为 `uint16_t*` 可能导致未对齐访问。在 x86 上无问题，但在 ARM 等严格对齐的架构上可能崩溃。

**建议：** 如需支持 ARM，改用 `memcpy` + 字节序转换。

### 风险 2: client_handler detached 线程的资源清理

```c
pthread_create(&thread, NULL, client_handler, arg);
pthread_detach(thread);
```

**说明：** `client_handler` 使用 detached 线程，`server_stop()` 中通过 `usleep(500ms)` 等待线程退出。如果某个 handler 线程因网络原因阻塞超过 500ms，可能在资源已释放后仍在访问。

**建议：** 改用 joinable 线程 + 线程列表，或增加引用计数保护 client 生命周期。

### 风险 3: strncpy 未确保 NUL 终止 (main.c:42,67)

```c
strncpy(config.bind_host, "0.0.0.0", sizeof(config.bind_host) - 1);
strncpy(config.bind_host, optarg, sizeof(config.bind_host) - 1);
```

**说明：** `strncpy` 在源字符串短于 n 时会填充 NUL，但如果源字符串恰好等于 n 则不会添加 NUL 终止符。这里使用 `sizeof - 1` 配合 `memset(0)` 初始化是安全的，但风格不一致。

**建议：** 统一改用 `snprintf(config.bind_host, sizeof(config.bind_host), "%s", value)`。

---

## 测试结果

| 后端 | 单路 | 20 路压力测试 |
|------|------|--------------|
| NVIDIA NVDEC | 60 fps | 60 fps × 20, GPU ~36%, CPU ~220% |
| Intel VA-API | 60 fps | 60 fps × 20, CPU ~430% |

**编译状态：** 0 warnings (`-Wall -Wextra`)
