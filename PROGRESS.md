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
- 启动命令保持不变，只要启用了 `--zmq-bridge-bind`，就会自动带出默认 IPC

当前正式链路的默认行为：

- `ml_worker` 默认请求 `HEVC 4:4:4 8-bit`
- `stream_server` 在 `auto` 模式下对 `HEVC444` 优先走 Intel VA-API
- `GET_LATEST_BGR` 统一走解码后适配层，不再由 `zmq_bridge` 自己维护多套源格式分支
- 常见快路径优先走 `libyuv`
- `VUYX -> BGR24` 等当前无专门快路径的格式回退到 `swscale`

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

### 3. ZMQ bridge 工程化改造

- `GET_LATEST_BGR` 支持按帧缓存最近一次 `BGR24`
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

- 20 路同时解码稳定
- 20 路同时 `BGR24` 取图，`5 FPS/路`
  - `stream_server` 进程平均 CPU 约 `72%`
  - 相对空载大约多吃 `0.47` 个逻辑核
- 20 路同时 `BGR24` 取图，`10 FPS/路`
  - `stream_server` 进程平均 CPU 约 `113%`
  - 相对空载大约多吃 `0.88` 个逻辑核

结论：

- 当前正式场景没有阻塞使用的问题
- 按 `5 FPS/路` 的取图模式，CPU 成本是可接受的

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
- 但 Intel 下载到 CPU 后的像素格式、驱动行为、FFmpeg/VAAPI 组合，在其它 Ubuntu 主机上不保证完全一致

影响：

- 现在可以说“本机可用”
- 还不能直接外推为“所有 Intel Ubuntu 主机都可无差别运行”

### 4. 高并发取图场景仍有明显 CPU 增量

- `20 x 5 FPS` 取 `BGR24`：`stream_server` 平均约 `72% CPU`
- `20 x 10 FPS` 取 `BGR24`：`stream_server` 平均约 `113% CPU`

影响：

- 当前业务频率内没有问题
- 如果后续同时提升：
  - 路数
  - 分辨率
  - 单路取图频率
  需要重新压测

### 5. 文档维护需要跟上正式状态

- 旧版 `PROGRESS.md` 曾长期停留在 `H.264/NVDEC/30fps` 结论
- 当前主链路已经切到 `HEVC444 + Intel-first + tcp/ipc 双 ZMQ`

影响：

- 后续功能改动如果不及时回写状态文档，会再次产生认知漂移

## 其他未合入的本地修改

截至本文档更新时，本地还有两处未合入改动：

### 1. `deploy/agent_stack_runtime/README.md`

内容：

- 新增一个直接在 `192.168.11.31` 上把 `worker_s21` 改成 `2560x1440` 并立即重启的示例命令

判断：

- 这是有用的运行示例
- 可以合入

### 2. `python_dir/input_client.py`

内容：

- `__main__` 里的 demo 默认控制端口从 `50002` 改成了 `50001`

判断：

- `WorkerInputClient.__init__()` 本身默认端口就是 `50001`
- 项目其它文档和控制端口约定也都是 `stream 1 -> 50001`
- 这更像是把 demo 入口和项目默认约定对齐
- 可以合入

## 建议的下一步

1. 把部署流程收成脚本化版本
2. 给正式部署增加标准化回滚方式
3. 如需扩容，再补：
   - 更高路数
   - 更高分辨率
   - Intel / NVIDIA 的同口径对比压测

---

Doc-Version: 0.2.0
Repo-Rev: 2841723+
