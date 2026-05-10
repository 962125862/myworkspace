# HEVC444 / stream_server 项目进度

## 当前主链路

```text
Docker / Sunshine
  -> ml_worker
    -> 192.168.11.31 stream_server
      -> 内置 ZMQ bridge
         - tcp://192.168.11.31:5566
         - ipc:///tmp/stream_server_bgr.sock
```

当前正式服务：

- 主机：`192.168.11.31`
- 服务：`stream_server_9000.service`
- 推流端口：`9000/tcp`
- ZMQ TCP：`tcp://0.0.0.0:5566`
- ZMQ IPC：`ipc:///tmp/stream_server_bgr.sock`
- 当前 Intel 栈：`i915 + Intel iHD 24.1.0`
- 启动命令保持不变，只要启用了 `--zmq-bridge-bind`，就会自动带出默认 IPC

当前正式链路的默认行为：

- `ml_worker` 默认请求 `HEVC 4:4:4 8-bit`
- `stream_server` 在 `auto` 模式下对 `HEVC444` 优先走 Intel VA-API
- `GET_LATEST_BGR` 统一走解码后适配层，不再由 `zmq_bridge` 自己维护多套源格式分支
- 常见快路径优先走 `libyuv`
- 本机 `HEVC444 + Intel` 下载到 CPU 后当前通常落到 `VUYX`
- `VUYX -> BGR24` 现在已有专门快路径，并带 x86 SIMD 分发
- `STREAM_DEFER_HW_DOWNLOAD=on` 仍保持为默认值，取图时才做 `GPU -> CPU` 下载

## 已完成

### 1. 自动后端选择收敛

- `DECODE_BACKEND=auto` 保留为真正自动模式，不会在 `server` 层被提前硬解析
- `HEVC444` 当前在本机默认优先走 Intel VA-API
- 显式指定 `intel/nvidia/cpu` 时仍可覆盖自动路由

### 2. 解码后格式适配收敛

- `decoder` 负责暴露真实 CPU 帧格式
- `zmq_bridge` 统一请求 `BGR24`
- `NV12/YUV420P/YUV444P/VUYX` 已纳入统一转换入口
- Intel `HEVC444` 实测可走到 `VUYX -> BGR24`
- `VUYX -> BGR24` 已切到专门快路径，不再走 `swscale`
- `ctx == NULL` 的 `swscale` 调用已改成复用缓存的 `SwsContext`
- `GET_LATEST_BGR` 路径上的一个 UAF 已修复

### 3. ZMQ bridge 工程化改造

- `GET_LATEST_BGR` 支持按帧缓存最近一次 `BGR24`
- `GET_LATEST_BGR` 支持可选输出 ROI，返回裁剪后的 tight `BGR24` payload 以降低 IPC 带宽
- BGR payload 改成 zero-copy 发送，减少一次额外 memcpy
- 新增第二个监听地址能力：同一个 `ROUTER` 可同时 bind `tcp` 和 `ipc`
- 只要启用了 `--zmq-bridge-bind`，默认就会再启一个 `ipc:///tmp/stream_server_bgr.sock`
- bridge 退出时会清理 IPC socket 文件

### 4. 正式环境部署完成

- 最新正式版本已部署到 `192.168.11.31`
- 正式服务已验证：
  - `tcp://192.168.11.31:5566`
  - `ipc:///tmp/stream_server_bgr.sock`
- IPC 路径已在正式服务上实测 `GET_LATEST_BGR` 成功

### 5. 20 路并发能力验证

正式 `stream_server` 上，`1024x768 HEVC444` 的当前结果：

- 20 路真实流同时解码稳定
- 不持续取图时：
  - 平均每路 `31.5 fps`
  - 平均 `Dec` 约 `0.348 ms/frame`
  - `stream_server` 进程平均 CPU 约 `25.8%`
  - `stream_server` 进程平均 RSS 约 `448.9 MB`
- 20 路 `IPC + GET_LATEST_BGR`，目标 `30 fps/路` 时：
  - `Intel`：总吞吐 `343.43 fps`，约 `17.17 fps/路`
    - `Dec 0.303 ms`
    - `Xfer 2.011 ms`
    - `Convert 0.885 ms`
    - 进程平均 CPU `134.8%`
    - 进程平均 RSS `534.7 MB`
  - `NVIDIA`：总吞吐 `595.53 fps`，约 `29.78 fps/路`
    - `Dec 1.551 ms`
    - `Xfer 0.906 ms`
    - `Convert 0.624 ms`
    - 进程平均 CPU `140.6%`
    - 进程平均 RSS `916.1 MB`
    - GPU 显存平均约 `4460 MB`

结论：

- 当前正式业务频率下没有阻塞使用的问题
- Intel 这条路的瓶颈已经不是解码本身，而是 `Xfer + Convert`
- 如需看完整口径和对比说明，直接参考 `deploy/BENCHMARK_stream_server_2026-04-13.md`

## 当前已知问题 / 风险

当前没有阻塞正式使用的问题，但还有这些工程层面的剩余问题：

### 1. 本地开发环境与正式环境不完全一致

- 本地 `build-stream-server` 这套环境未带 `libzmq`
- 因此本地可做无 ZMQ 编译验证，但 ZMQ/IPC 运行态验证仍主要依赖 `192.168.11.31`

影响：

- 本地无法完整复现正式服务的 ZMQ 行为
- 涉及 `ipc/tcp` bridge 的改动需要远端再验一次

### 2. 部署链路还不够工程化

- `192.168.11.31:/home/gejun/work/my_ml_work` 不是 git 工作树
- 现在正式部署仍依赖 `scp/rsync` 式文件同步
- 远端普通用户无 `systemctl restart` 权限
- 正式服务替换当前依赖：
  - 先重编二进制
  - 再向 `MainPID` 发送 `SIGTERM`
  - 让 `Restart=always` 的 systemd 自动拉起新版本

影响：

- 部署流程可用，但不够标准化
- 回滚和版本追溯需要额外人工约束

### 3. Intel 路径的跨机器可移植性仍需保守看待

- 本机 `192.168.11.31` 上，Intel `HEVC444` 已验证可用
- 当前本机实际栈是 `i915 + Intel iHD 24.1.0 + FFmpeg/VAAPI`
- Intel 下载到 CPU 后的像素格式、驱动行为、FFmpeg/VAAPI 组合，在其它 Ubuntu 主机上不保证完全一致

影响：

- 现在可以说“本机可用”
- 还不能直接外推为“所有 Intel Ubuntu 主机都可无差别运行”

### 4. Intel 高并发取图仍受 `Xfer` 和单线程 bridge 限制

- 在 `STREAM_DEFER_HW_DOWNLOAD=on` 下，`GET_LATEST_BGR` 的关键成本是：
  - `av_hwframe_transfer_data()`
  - CPU 上转 `BGR24`
- 当前 `ZMQ` bridge 仍是单个 `ROUTER` 线程串行处理请求
- 本机 20 路 `30 fps/路` 的实测里：
  - `Intel` 约 `2.896 ms/request`（`Xfer + Convert`）
  - `NVIDIA` 约 `1.530 ms/request`

影响：

- 当前实际业务频率内没有问题
- 如果目标变成“20 路持续接近 30 fps 的 `BGR24` 拉取”，当前 Intel 路线不占优

### 5. bridge 并发化实验未进入正式版本

- 已做过一版 worker 化 bridge 实验
- 20 路 `30 fps/路` 的 Intel 场景下，吞吐从约 `17.17 fps/路` 提到约 `20.40 fps/路`
- 但 `stream_server` 进程 CPU 也从约 `134.8%` 抬到了约 `259%`

影响：

- 这条路当前不符合“保留 CPU 余量给其他生产业务”的约束
- 因此没有进入正式版本

### 6. 文档维护需要跟上正式状态

- 旧版 `PROGRESS.md` 曾长期停留在 `H.264/NVDEC/30fps` 结论
- 当前主链路已经切到 `HEVC444 + Intel-first + tcp/ipc 双 ZMQ`
- 当前 `VUYX -> BGR24` 也已经不是 `swscale fallback`

影响：

- 后续功能改动如果不及时回写状态文档，会再次产生认知漂移

## 建议的下一步

1. 把部署流程收成可回滚的脚本化版本
2. 如果后续 `BGR24` 拉取频率继续上升，优先评估 `NVIDIA` 或减少 CPU `BGR24` 需求
3. 如果要继续研究 Intel，再做非正式环境的 `FFmpeg + intel-media-va-driver` A/B 测试

---

Doc-Version: 0.3.0
Repo-Rev: 0dcf925
