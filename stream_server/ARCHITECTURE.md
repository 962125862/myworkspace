# Stream Server 架构文档

## 概述

stream_server 是一个高性能多路视频流接收与硬件解码服务器，支持同时接收并解码最多 20 路 H.264 视频流。

**核心能力：**
- 单端口 TCP 接收多路视频流 (TLV 二进制协议)
- 硬件加速解码: NVIDIA NVDEC (CUDA) / Intel VA-API / CPU 软解
- 20 路 60fps 并行解码 (NVIDIA GPU 利用率 ~36%, Intel VA-API CPU ~430%)
- 压力测试模式: 1 路实时流复制到 N 路虚拟流并行解码

---

## 模块边界

```
┌─────────────────────────────────────────────────────────┐
│                        main.c                           │
│              程序入口, 信号处理, 主循环                    │
└──────────────┬─────────────────────┬────────────────────┘
               │                     │
        ┌──────▼──────┐       ┌──────▼──────┐
        │  server.c   │       │  stream.c   │
        │  TCP 服务器  │──────>│  流管理器    │
        │             │       │  解码调度    │
        └──────┬──────┘       │  压力测试    │
               │              └──────┬──────┘
        ┌──────▼──────┐              │
        │ protocol.c  │       ┌──────▼──────┐
        │ 协议解析     │       │  decoder.c  │
        │ (纯函数)     │       │  硬件解码器  │
        └─────────────┘       │  FFmpeg封装  │
                              └─────────────┘
```

### 模块职责

| 模块 | 文件 | 职责 |
|------|------|------|
| **协议层** | `protocol.h/c` | 二进制 TLV 协议解析/构建，纯函数无状态 |
| **解码层** | `decoder.h/c` | H.264 硬件解码，FFmpeg 封装，支持三种后端 |
| **流管理层** | `stream.h/c` | 流生命周期、解码调度、FPS 统计、压力测试线程池 |
| **网络层** | `server.h/c` | TCP accept/recv、客户端管理、包分发 |
| **入口** | `main.c` | 参数解析、信号处理、统计打印主循环 |

可选模块:

| 模块 | 文件 | 职责 |
|------|------|------|
| **共享内存发布** | `shm_frame.h/c` | 将最新帧(YUV: NV12/YUV420P)按需发布到 POSIX shared memory，供下游进程直接读取 |

---

## 数据流

### 单路流模式

```
Docker ml_worker (192.168.11.50)
       │
       │ TCP 连接
       ▼
┌─────────────────────────┐
│ accept_loop (thread)    │ ── accept() ──> client_handler (detached thread)
│                         │
│ client_handler:         │
│   1. recv 7 字节头部     │
│   2. protocol_parse_header()                    ← protocol.c
│   3. recv payload                                
│   4. handle_packet()                            ← server.c
│      │                  │
│      ├─ STREAM_START    │
│      │   stream_set_info()                      ← stream.c
│      │   stream_init_decoder()                  ← stream.c → decoder.c
│      │                  │
│      ├─ VIDEO_DATA      │
│      │   stream_decode_video()                  ← stream.c
│      │     └─ decoder_decode()                  ← decoder.c
│      │         ├─ av_parser_parse2() (循环)     ← FFmpeg parser
│      │         ├─ avcodec_send_packet()         ← FFmpeg codec
│      │         ├─ avcodec_receive_frame() (循环) ← FFmpeg codec
│      │         └─ av_hwframe_transfer_data()    ← GPU→CPU 传输
│      │                  │
│      └─ STREAM_STOP     │
│          stream_close_decoder()                 ← stream.c
└─────────────────────────┘
```

### 20 路压力测试模式

```
Docker ml_worker ── TCP ──> stream_server
                              │
          STREAM_START ───────┤
          + STRESS_TEST=1     │
                              ▼
                    stream_manager_start_stress_test()
                    创建 20 个虚拟流 + 20 个解码线程
                              │
                              │ VIDEO_DATA 到达时:
                              ▼
                ┌─ stream_decode_video(源流)     ← client_handler 线程
                │
                └─ stream_stress_test_decode()   ← 非阻塞分发
                     │
                     ├─ queue[0] ──signal──> thread[0] ──> decode(虚拟流1)
                     ├─ queue[1] ──signal──> thread[1] ──> decode(虚拟流2)
                     ├─ ...
                     └─ queue[19] ─signal──> thread[19] ─> decode(虚拟流20)

每个线程独立:
  stress_decode_worker():
    while (running) {
        pthread_cond_timedwait(queue_cond, 50ms)  // 等待新数据
        memcpy(local_buf, queue_data)              // 取走数据
        stream_decode_video(virtual_stream)        // 独立解码
    }
```

---

## 线程模型

```
main thread
  │
  ├─ server_start()
  │    └─ accept_loop [thread]
  │         └─ client_handler [detached thread] × N 个客户端
  │              └─ handle_packet()
  │                   ├─ stream_decode_video()    (源流解码)
  │                   └─ stream_stress_test_decode() (分发到线程池)
  │
  ├─ stress_decode_worker [thread] × 20
  │    └─ stream_decode_video(虚拟流)
  │
  └─ 主循环: sleep(1) + 定期打印统计
```

### 锁层次 (Lock Hierarchy)

为避免死锁，锁的获取必须遵循以下顺序:

```
mgr->lock (全局)
  └─ stream->lock (per-stream)
       └─ queue_locks[i] (per-thread, 压力测试)
```

**规则：**
- 持有内层锁时，不得获取外层锁
- `stream_decode_video()` 持有 `stream->lock` 期间调用 `decoder_decode()`
- `stress_decode_worker` 先获取 `queue_lock` 取数据，释放后再调用 `stream_decode_video()`

---

## 协议格式

```
┌───────────────┬──────────┬────────────┬────────────────┐
│ length (4B)   │ type(1B) │ stream_id  │   payload ...  │
│ uint32 BE     │ uint8    │ (2B) BE    │                │
└───────────────┴──────────┴────────────┴────────────────┘
                |<------------ length 字节 ------------->|
```

| 消息类型 | 值 | Payload |
|----------|------|---------|
| VIDEO_DATA | 0x01 | H.264 NAL 单元原始数据 |
| HEARTBEAT | 0x02 | 无 |
| STREAM_START | 0x03 | StreamInfo (16B: width/height/fps/bitrate, 各 uint32 BE) |
| STREAM_STOP | 0x04 | 无 |

---

## 硬件解码后端对比

| 特性 | NVIDIA NVDEC | Intel VA-API | CPU 软解 |
|------|-------------|-------------|---------|
| FFmpeg 像素格式 | `AV_PIX_FMT_CUDA` | `AV_PIX_FMT_VAAPI` | `AV_PIX_FMT_YUV420P` |
| hw_frames_ctx | FFmpeg 自动创建 | 手动创建 (vaapi_get_format) | 不需要 |
| get_buffer2 | 不需要 | hw_get_buffer | 默认 |
| extra_hw_frames | 4 | N/A | N/A |
| thread_count | 1 | 默认 | 可配置 (slice 模式) |
| 输出格式 | NV12 (hw transfer) | NV12 (hw transfer) | YUV420P |
| 20 路 60fps | GPU ~36% | CPU ~430% | 不实际 |

### NVIDIA 初始化要点 (参考 moonlight-qt)

```
关键：只设置 hw_device_ctx，不手动创建 hw_frames_ctx
ctx->codec_ctx->get_format = get_hw_format;          // 简单格式选择
ctx->codec_ctx->hw_device_ctx = av_buffer_ref(...);   // 设备上下文
ctx->codec_ctx->extra_hw_frames = 4;                  // 额外帧池
ctx->codec_ctx->thread_count = 1;                     // 硬解单线程
// 不设置 get_buffer2，不手动 av_hwframe_ctx_alloc
```

### VA-API 初始化要点 (参考 embedded ffmpeg_vaapi.c)

```
vaapi_get_format() 回调中:
  1. av_hwframe_ctx_alloc() 创建帧上下文
  2. 设置 format=VAAPI, sw_format=NV12, initial_pool_size=17
  3. av_hwframe_ctx_init() 初始化
  4. 挂载到 ctx->hw_frames_ctx
hw_get_buffer() 回调: 从帧池分配 surface
```

---

## 关键设计决策

### 1. Parser 循环消费

H.264 parser (`av_parser_parse2`) 每次调用可能只消费输入数据的一部分。必须循环调用直到输入数据全部消费完毕，否则会丢失帧导致帧率减半 (Frames:Decoded = 2:1)。

```c
while (parse_remaining > 0) {
    int parsed = av_parser_parse2(..., parse_ptr, parse_remaining, ...);
    parse_ptr += parsed;
    parse_remaining -= parsed;
    if (pkt_size == 0) continue;  // parser 缓冲中，还没输出
    // send_packet + receive_frame
}
```

### 2. 只保留最后一帧

`decoder_decode()` 在一次调用中可能产生多帧（parser 循环 + receive 循环），但只返回最后一帧。中间帧被释放。这对实时流来说是正确的——下游只需要最新帧。

### 3. 非阻塞压力测试分发

`stream_stress_test_decode()` 使用覆盖策略：如果某线程还没消费完旧数据，直接覆盖。这保证了 TCP 接收线程不会被任何慢速解码线程阻塞。

### 4. bytes_in 只计一次

统计 `ctx->stats.bytes_in += size` 放在 parser 循环外部，且仅在有帧输出时计算，避免循环内重复累加。

### 5. SHM 按需发布 (request/publish + seqlock)

为避免下游跨线程/跨进程直接引用 `last_frame` 引发的生命周期问题，可启用共享内存发布：

- Writer(stream_server) 每路流创建一个 shm: `/stream_server_stream_XX`
- Reader(下游) 通过自增 `request_seq` 请求“下一帧”，writer 在下一次解码成功时 memcpy 并更新 `publish_seq`
- 一致性：使用 `write_seq` seqlock（写入期间为奇数；读者发现奇数/前后不一致则重试）

启用：`ENABLE_SHM=1`；若希望每帧都发布可设 `SHM_ALWAYS=1`。

---

## 目录结构

```
stream_server/
├── include/
│   ├── protocol.h          # 协议定义
│   ├── decoder.h           # 解码器 API
│   ├── shm_frame.h          # 共享内存发布 API (可选)
│   ├── stream.h            # 流管理器 API
│   └── server.h            # TCP 服务器 API
├── src/
│   ├── main.c              # 程序入口
│   ├── protocol.c          # 协议解析 (纯函数)
│   ├── decoder.c           # 硬件解码器实现
│   ├── shm_frame.c          # 共享内存发布实现 (可选)
│   ├── stream.c            # 流管理 + 压力测试线程池
│   └── server.c            # TCP 服务器实现
├── tests/
│   ├── test_hw_decode.c    # 硬件解码单元测试
│   ├── test_decode_perf.c  # 解码性能测试
│   ├── test_10sec_load.c   # 10 秒负载测试
│   └── benchmark_10streams.c # 10 路基准测试
├── test_20streams_simple.c # 20 路简化压力测试
├── test_20streams_hw.c     # 20 路完整压力测试
├── stream_receiver_decode.c # 独立流接收解码器
├── CMakeLists.txt          # CMake 构建
├── Makefile                # 简化构建 (不依赖 cmake)
├── install_deps.sh         # 依赖安装脚本
├── install_nvidia_deps.sh  # NVIDIA 依赖安装
├── start_server.sh         # 启动服务器脚本
└── stress_test_20streams.sh # 20 路压力测试脚本
```
