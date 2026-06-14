# 压缩流旁路录像

本文描述当前 `stream_server` 上的压缩流录像方案，以及 2026-06-14 在 `31 / 35 / 888` 三台机器上的实际部署状态。

当前有效链路：

```text
192.168.11.31 stream_server
  -> 直接写 raw HEVC/H264 切片到 35 NFS
192.168.11.35 hevc-store
  -> 只提供 NFS 共享存储和容量清理
192.168.11.7 888 LXC
  -> 挂载 35 NFS，VAAPI 二次转码，转码成功后删除 raw
```

## 架构

录像旁路放在 `stream_server` 收到 `VIDEO_DATA` 后、解码前：

```text
ml_worker
  -> stream_server
       -> compressed_recorder queue -> writer thread -> raw segments
       -> stream_decode_video -> last_frame -> ZMQ BGR
```

主接收线程只做 `enqueue`，不直接写文件。writer 线程负责 NAL 识别、关键帧切片、文件写入和 `.tmp` rename。

## 启动参数

`stream_server` 默认不录像；设置 `--compressed-record-dir` 或 `COMPRESSED_RECORD_DIR` 后才启动 recorder。

```bash
./stream_server/build/stream_server \
  -h 0.0.0.0 \
  -p 9000 \
  -c 30 \
  --zmq-bridge-bind tcp://0.0.0.0:5566 \
  --ml-worker-ctrl-map-file /home/gejun/work/my_ml_work/deploy/stream_server_ctrl_map.txt \
  --compressed-record-dir /data/stream_records \
  --compressed-record-segment-sec 1800 \
  --compressed-record-idr-interval-sec 30 \
  --compressed-record-queue-mb 256 \
  -v
```

上述示例是通用参数；当前 31 实际部署用 systemd drop-in 注入环境变量，见后文“31 实际部署”。

等价环境变量：

```text
COMPRESSED_RECORD_DIR
COMPRESSED_RECORD_STREAMS  # 可选；不设置时录全部 stream
COMPRESSED_RECORD_SEGMENT_SEC
COMPRESSED_RECORD_IDR_INTERVAL_SEC
COMPRESSED_RECORD_QUEUE_MB
COMPRESSED_RECORD_REQUIRE_NFS  # 1 表示 record dir 必须已经挂在 NFS 上，否则跳过写入
```

## 文件布局

```text
/data/stream_records/raw/s01/20260614_093000.hevc.tmp
/data/stream_records/raw/s01/20260614_093000.hevc
/data/stream_records/raw/s02/20260614_093000.hevc
```

`.tmp` 表示正在写。关闭并完成的切片会 rename 成 `.h264` 或 `.hevc`。

## 切片规则

- 代码默认每 30 分钟一个切片。
- 当前 31 实际部署为 `COMPRESSED_RECORD_SEGMENT_SEC=180`，即每 3 分钟目标切片一次。
- 切片必须从关键帧开始。
- H264 缓存 SPS/PPS，HEVC 缓存 VPS/SPS/PPS。
- 到切片时间后，`stream_server` 先请求上游 `REQ_IDR`。
- 旧文件继续写，直到下一个关键帧到来。
- 新文件开头写参数集，再写当前关键帧 payload。
- 如果 IDR 长时间不来，不硬切，继续写旧文件并退避重试。

## 周期 IDR

recorder 默认每路每 30 秒请求一次 `REQ_IDR`，用于让长切片内部也有可 seek 的关键帧。

- `--compressed-record-idr-interval-sec 30`：默认值。
- `--compressed-record-idr-interval-sec 0`：关闭周期 IDR，只保留开录、切片和解码恢复 IDR。
- 多路会按 `stream_id` 在一个周期窗口内错峰；20 路、30 秒周期时，约每 1.5 秒发出一路 IDR 请求。
- 切片 rollover 正在等待关键帧时，周期 IDR 会跳过，避免重复请求。

HEVC 识别：

```text
nal_type = (nal_header_byte0 >> 1) & 0x3f
VPS=32 SPS=33 PPS=34
IDR=19/20 CRA=21 BLA=16-18
```

H264 识别：

```text
nal_type = nal_header & 0x1f
SPS=7 PPS=8 IDR=5
```

## 直接写 NFS

当前 31 -> 35 路径：

```text
31 NFS mount: /mnt/hevc_store_35
35 store:     /home/gejun/hevc_store
35 raw:       /home/gejun/hevc_store/raw
35 output:    /home/gejun/hevc_store/transcoded
```

31 的 NFS 挂载使用 systemd automount：

```text
192.168.11.35:/home/gejun/hevc_store /mnt/hevc_store_35 nfs4 rw,noatime,vers=4.2,soft,timeo=50,retrans=1,_netdev,nofail,noauto,x-systemd.automount,x-systemd.idle-timeout=300,x-systemd.mount-timeout=10s 0 0
```

因此：

- 35 先启动、31 后启动：35 只提供 NFS export 和转码目录，等待文件进入。
- 31 先启动、35 未启动：`stream_server` 主链路继续运行；录像检测到 `/mnt/hevc_store_35` 不是 NFS 时直接跳过写入。
- 写入过程中如果 open/write/rename 失败，recorder 会重新看挂载状态；如果 NFS 已经不在挂载表里，就暂停录像，等待挂载恢复。
- 不再启用 31 本地缓存、同步、兜底清理这套逻辑；录像数据允许在 NFS 不可用期间丢失。

```bash
COMPRESSED_RECORD_DIR=/mnt/hevc_store_35
COMPRESSED_RECORD_REQUIRE_NFS=1
```

31 上有一个独立 timer 每分钟尝试挂载：

```bash
sudo systemctl enable --now hevc-store-mount.timer
```

timer 只负责 `mount /mnt/hevc_store_35`，失败就等下一分钟；不参与录像写入和主链路。

## 31 实际部署

机器：

```text
IP:       192.168.11.31
hostname: t10
user:     gejun
role:     主链路 stream_server + compressed recorder
```

`stream_server_9000.service` 已启用并运行：

```text
/home/gejun/work/my_ml_work/stream_server/build/stream_server \
  -h 0.0.0.0 \
  -p 9000 \
  -c 30 \
  --zmq-bridge-bind tcp://0.0.0.0:5566 \
  --ml-worker-ctrl-map-file /home/gejun/work/my_ml_work/deploy/stream_server_ctrl_map.txt \
  -v
```

录像参数通过 systemd drop-in `/etc/systemd/system/stream_server_9000.service.d/compressed-record.conf` 注入：

```ini
[Service]
Environment=COMPRESSED_RECORD_DIR=/mnt/hevc_store_35
Environment=COMPRESSED_RECORD_STREAMS=1-20
Environment=COMPRESSED_RECORD_SEGMENT_SEC=180
Environment=COMPRESSED_RECORD_IDR_INTERVAL_SEC=30
Environment=COMPRESSED_RECORD_QUEUE_MB=1024
Environment=COMPRESSED_RECORD_REQUIRE_NFS=1
```

NFS 挂载：

```text
192.168.11.35:/home/gejun/hevc_store /mnt/hevc_store_35 nfs4 rw,noatime,vers=4.2,soft,timeo=50,retrans=1,_netdev,nofail,noauto,x-systemd.automount,x-systemd.idle-timeout=300,x-systemd.mount-timeout=10s 0 0
```

31 上启用：

```bash
sudo systemctl enable --now stream_server_9000.service
sudo systemctl enable --now hevc-store-mount.timer
```

31 的 `stream_server` 主解码链路继续输出 ZMQ BGR；recorder 是解码前压缩流旁路，writer 队列满、NFS 未挂载或写入失败都只影响录像，不阻塞主链路。

## 二次转码

888 LXC 上的后处理脚本：

```bash
/home/gejun/bin/transcode_stream_records.sh
```

仓库源文件：

```bash
./deploy/transcode_stream_records.sh
```

默认策略：

- 35 只提供 NFS 共享存储，不承担转码。
- 888 挂载 35 的 NFS 到 `/mnt/hevc_store_35`，扫描 `/mnt/hevc_store_35/raw` 下已关闭的 `.h264/.hevc`。
- 按 stream 目录分组，例如 `s01`、`s02`。
- 同一路按文件名时间排序，当前凑够 `5` 个片段才转码一次。
- 3 分钟切片时，5 个片段约合并成 15 分钟 MP4。
- 输出 `/mnt/hevc_store_35/transcoded/sXX/*.mp4`，实际落在 35 的 `/home/gejun/hevc_store/transcoded/sXX/*.mp4`。
- 输入按 `30fps` 解释，输出降到 `10fps`。
- 888 使用 Intel VAAPI，编码器 `hevc_vaapi`，码率 `1200k`，输出 `HEVC Main / yuv420p`。
- 当前并行度 `STREAM_TRANSCODE_PARALLEL=2`，一次最多同时跑 2 个 ffmpeg batch，给 VAAPI 和 NFS 留余量。
- 合并使用裸流字节拼接输入给 ffmpeg，不创建大 merged 中间文件。
- 输出先写 `.mp4.tmp`，成功后 rename 成 `.mp4`。
- 成功后给 batch 内每个源文件写 `.transcoded` 标记。
- 当前 `STREAM_TRANSCODE_DELETE_SOURCE=1`，转码成功后删除 batch 内 raw 源文件和同步标记，避免 35 空间持续增长。

888 上 timer 启用：

```bash
sudo systemctl enable --now hevc-transcode.timer
```

888 上也启用 NFS 挂载检查 timer：

```bash
sudo systemctl enable --now hevc-store-mount.timer
```

如果 888 先启动、35 还没起来，`hevc-store-mount.timer` 每分钟尝试挂载一次；`hevc-transcode.service` 发现 `/mnt/hevc_store_35/raw` 不是 NFS 时直接跳过，不把转码服务卡死或打成持续故障。35 恢复后，下一轮自动继续转码。

35 上不部署 `hevc-transcode.service/timer`，避免和 888 重复处理同一批 raw 文件。

## 888 实际部署

机器：

```text
PVE host: 192.168.11.10
CT ID:    888
IP:       192.168.11.7
hostname: hvce-vaapi
user:     gejun, uid/gid 1000
role:     VAAPI 二次转码
```

LXC 配置：

```text
features: nesting=1
/dev/dri bind mount 到 CT
gejun 加入 video 和 render 设备对应组
```

已安装依赖：

```text
ffmpeg
vainfo
intel-media-va-driver-non-free
nfs-common
intel-gpu-tools
```

APT 源已切到中科大 Ubuntu noble：

```text
https://mirrors.ustc.edu.cn/ubuntu/
```

VAAPI 环境：

```text
LIBVA_DRIVER_NAME=iHD
VAAPI device: /dev/dri/renderD128
validated: HEVC Rext/yuv444p decode works, hevc_vaapi encode works
```

NFS 挂载：

```text
192.168.11.35:/home/gejun/hevc_store /mnt/hevc_store_35 nfs4 rw,noatime,vers=4.2,soft,timeo=50,retrans=1,_netdev,nofail,x-systemd.automount,x-systemd.mount-timeout=10s 0 0
```

部署文件：

```text
/home/gejun/bin/transcode_stream_records.sh
/home/gejun/bin/ensure_hevc_store_mount.sh
/etc/systemd/system/hevc-transcode.service
/etc/systemd/system/hevc-transcode.timer
/etc/systemd/system/hevc-store-mount.service
/etc/systemd/system/hevc-store-mount.timer
```

启用服务：

```bash
sudo systemctl enable --now hevc-store-mount.timer
sudo systemctl enable --now hevc-transcode.timer
```

`hevc-transcode.service` 当前参数：

```ini
Environment=LIBVA_DRIVER_NAME=iHD
Environment=STREAM_TRANSCODE_SRC=/mnt/hevc_store_35/raw
Environment=STREAM_TRANSCODE_DST=/mnt/hevc_store_35/transcoded
Environment=STREAM_TRANSCODE_LOG_DIR=/mnt/hevc_store_35/logs
Environment=STREAM_TRANSCODE_REQUIRE_NFS=1
Environment=STREAM_TRANSCODE_MIN_AGE_SEC=120
Environment=STREAM_TRANSCODE_INPUT_FPS=30
Environment=STREAM_TRANSCODE_OUTPUT_FPS=10
Environment=STREAM_TRANSCODE_BITRATE=1200k
Environment=STREAM_TRANSCODE_ENCODER=hevc_vaapi
Environment=STREAM_TRANSCODE_VAAPI_DEVICE=/dev/dri/renderD128
Environment=STREAM_TRANSCODE_BATCH_SIZE=5
Environment=STREAM_TRANSCODE_PARALLEL=2
Environment=STREAM_TRANSCODE_DELETE_SOURCE=1
Environment=STREAM_TRANSCODE_MAX_BATCHES=0
```

转码输出：

```text
/mnt/hevc_store_35/transcoded/sXX/<first>__<last>.mp4
```

行为：

- 每路凑够 5 个已关闭 raw 切片再转码。
- 当前 31 是 3 分钟 raw 切片，因此一个 MP4 约 15 分钟。
- 输出 `HEVC Main / yuv420p / 10fps / 1200k`。
- 并行 2 路，给 VAAPI 和 NFS 留余量。
- 成功后删除 batch 内 raw 源文件、`.synced` 和 `.transcoded` 标记。
- NFS 未挂载时直接跳过，等待 mount timer 后续恢复。
- 35 中途掉线时当前 `.mp4.tmp` 失败，raw 不会误删，恢复后重试。

## 35 容量清理

35 作为共享存储，启用容量清理 timer：

```bash
sudo systemctl enable --now hevc-store-prune.timer
```

默认策略：

- 检查 `/home/gejun/hevc_store` 所在文件系统。
- 已用空间不超过 `600GB` 时不删除成品。
- 已用空间超过 `600GB` 后，删除当前时间 3 天前的转码成品 `.mp4`。
- 删除 MP4 时同步删除对应转码日志。
- 清理超过 6 小时的 `.mp4.tmp` 半成品。
- 已用空间超过 `800GB` 后，先删除不是今天生成的转码成品 `.mp4`。
- 已用空间超过 `800GB` 后，进入保命清理：删除最老的 closed raw `.h264/.hevc`，直到估算使用空间低于 `750GB`。
- 不删除正在写入的 raw `.tmp`。

## 35 实际部署

机器：

```text
IP:       192.168.11.35
hostname: hevc-store
user:     gejun
role:     NFS 共享存储 + 容量清理
```

目录：

```text
/home/gejun/hevc_store/raw
/home/gejun/hevc_store/transcoded
/home/gejun/hevc_store/logs
```

NFS export：

```text
/home/gejun/hevc_store 192.168.11.31(rw,sync,no_subtree_check) 192.168.11.7(rw,sync,no_subtree_check)
```

实际 exportfs：

```text
192.168.11.31 rw,sync,root_squash
192.168.11.7  rw,sync,root_squash
```

35 不运行转码：

```text
hevc-transcode.service: not found
hevc-transcode.timer:   not found
```

容量清理部署文件：

```text
/home/gejun/bin/prune_hevc_store_capacity.sh
/etc/systemd/system/hevc-store-prune.service
/etc/systemd/system/hevc-store-prune.timer
```

启用服务：

```bash
sudo systemctl enable --now hevc-store-prune.timer
```

清理参数：

```ini
Environment=HEVC_STORE_PRUNE_STORE_PATH=/home/gejun/hevc_store
Environment=HEVC_STORE_PRUNE_TRANSCODE_DIR=/home/gejun/hevc_store/transcoded
Environment=HEVC_STORE_PRUNE_RAW_DIR=/home/gejun/hevc_store/raw
Environment=HEVC_STORE_PRUNE_LOG_DIR=/home/gejun/hevc_store/logs
Environment=HEVC_STORE_PRUNE_CODED_TRIGGER_USED_GB=600
Environment=HEVC_STORE_PRUNE_CODED_RETENTION_DAYS=3
Environment=HEVC_STORE_PRUNE_RAW_TRIGGER_USED_GB=800
Environment=HEVC_STORE_PRUNE_RAW_TARGET_USED_GB=750
Environment=HEVC_STORE_PRUNE_TMP_MAX_AGE_MIN=360
```

当前 35 曾尝试使用 Tesla P4-2Q 做转码，但 P4 不支持 HEVC Rext/yuv444p 硬解，因此 35 只保留存储职责；HEVC444 二次转码放在 888 的 Intel VAAPI 上。

## 启动顺序和故障行为

- `31` 先启动、`35` 未启动：`stream_server` 主链路继续，recorder 发现 `/mnt/hevc_store_35` 不是 NFS 时跳过录像包。
- `35` 后启动：31 的 `hevc-store-mount.timer` 每分钟重试挂载，挂上后继续写新 raw。
- `888` 先启动、`35` 未启动：888 的 `hevc-store-mount.timer` 每分钟重试挂载，`hevc-transcode.service` 发现 raw 目录不是 NFS 时直接退出 0。
- 转码过程中 `35` 掉线：当前 ffmpeg 失败，`.mp4.tmp` 不会 rename 成正式 MP4，raw 不会删除；恢复后下一轮重试。
- `35` 只做 NFS 和容量清理；不参与转码计算。
- `888` 重启后，mount timer 和 transcode timer 自动恢复。

## 故障隔离

- recorder 队列有上限。
- 队列满时丢录像包，只增加 dropped 计数。
- NFS 未挂载时丢录像包，只增加 `mount_skipped` 计数。
- writer 卡顿不阻塞 `stream_decode_video`。
- 录像失败不影响 ZMQ BGR 主链路。

## 解码停滞自恢复

`stream_server` 还会检查中途解码停滞：

```text
received_delta >= 30
decoded_delta == 0
decoded 超过 2 秒没变化
```

满足条件时按退避限频向对应 `ml_worker` 发 `REQ_IDR`。

---

Doc-Version: 0.2.0
Repo-Rev: current
