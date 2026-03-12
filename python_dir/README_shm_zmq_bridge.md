# shm_zmq_bridge 用法说明

本目录提供一个 **ZMQ 桥接服务**，用于把 `stream_server` 写入 POSIX 共享内存（/dev/shm）里的最新 NV12 帧取出，并通过 ZMQ 返回给下游进程。

相关文件：

- 服务端：`python_dir/shm_zmq_bridge.py`
- 客户端示例（DEALER）：`python_dir/shm_zmq_bridge_client.py`

## 1. 前置条件

1) 启动 `stream_server` 时启用共享内存发布：

```bash
./stream_server --enable-shm
```

2) 确认对应流的共享内存对象存在：

```bash
ls -l /dev/shm/stream_server_stream_01
```

> `stream_server` 默认按 stream_id 创建：`/stream_server_stream_%02d`，因此落在 `/dev/shm/stream_server_stream_01` 这种形式。

3) 安装 pyzmq：

```bash
pip install pyzmq
```

## 2. 启动服务端（ROUTER + inproc 工作线程）

默认监听 `tcp://*:5555`：

```bash
python3 python_dir/shm_zmq_bridge.py
```

代码里支持参数（需要你自行在 main 里改/或导入调用）：

- `bind_addr`：对外绑定地址，默认 `tcp://*:5555`
- `backend_in/backend_out`：inproc 管道名，默认：
  - `inproc://shm_backend_in`
  - `inproc://shm_backend_out`
- `n_workers`：工作线程数（默认 1，原因见“并发注意事项”）

## 3. 请求协议

### 3.1 DEALER 客户端（推荐）

DEALER 发送：

```
[cmd][json]
```

其中：

- `cmd = b"GET_SHM_NV12"`
- `json = b"{\"stream_id\":1,\"timeout_ms\":1000,\"request_new\":true}"`

服务端返回：

```
[status][meta_json][y_plane][uv_plane]
```

- `status`：`b"OK"` / `b"ERR"`
- `meta_json`：包含 `width/height/pts/key_frame/...`
- `y_plane`：NV12 的 Y 平面（紧凑 w*h bytes）
- `uv_plane`：NV12 的 UV 平面（紧凑 w*h/2 bytes）

### 3.2 REQ 客户端

REQ 的路由帧结构里有空分隔帧，ROUTER 侧看到：

```
[identity][b""][cmd][json]
```

服务端会保留该分隔帧以兼容 REQ。

## 4. 客户端示例（DEALER）

拉取一帧并保存到 `/tmp/frame.*`：

```bash
python3 python_dir/shm_zmq_bridge_client.py \
  --addr tcp://127.0.0.1:5555 \
  --stream-id 1 \
  --out-prefix /tmp/frame
```

生成文件：

- `/tmp/frame.meta.json`
- `/tmp/frame.y`
- `/tmp/frame.uv`
- `/tmp/frame.nv12`（拼接后的 raw nv12）

使用 ffplay 预览（把 meta.json 里的 width/height 填进去）：

```bash
ffplay -f rawvideo -pixel_format nv12 -video_size 1280x720 -i /tmp/frame.nv12
```

## 5. 并发注意事项

`stream_server` 的 shm 发布支持 **按需发布**：reader 通过 `request_seq++` 请求“下一帧”，writer 发布后更新 `publish_seq`。

如果多个 reader 线程/进程同时对同一个 stream 的 `request_seq` 做非原子自增，可能导致请求序列竞争。

因此 `shm_zmq_bridge.py` 默认 `n_workers=1`，并且在 worker 内部对同一个 `stream_id` 使用锁串行化读流程。

如果你希望更高吞吐：

- 可以把 `request_new=false`（只读当前已发布的最新帧，不触发 request_seq++）
- 或为每个 stream_id 启动单独桥接进程
