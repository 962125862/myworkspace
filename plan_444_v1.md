# plan_444_v1

## 目标

让当前链路分阶段支持 Sunshine 的 YUV444：

1. `ml_worker` 发起串流时可按启动参数动态请求 4:2:0 / 4:4:4。
2. 保持现有默认行为不变，默认仍走当前稳定路径。
3. 后续补齐 `stream_server` 解码、格式转换和下游消费对 4:4:4 的支持。

## 当前现状

- `ml_worker` 当前把 `supportedVideoFormats` 固定为 `VIDEO_FORMAT_H264`。
- 当前 CLI 只有 `--colorspace` / `--range`，没有码流 profile / chroma / bit depth 入口。
- `video_callbacks` 本身只是转发编码后码流，对 codec/chroma 没有协商控制。
- `stream_server` 当前只把解码后的 `NV12` / `YUV420P` 识别为有效格式。
- `stream_server` 的 VAAPI 路径把 `sw_format` 固定成了 `AV_PIX_FMT_NV12`。
- 现有测试/文档默认都以 H.264 4:2:0 为主。

## 阶段划分

### Phase 1: ml_worker 参数化协商

目标：先把“请求 444”能力做出来，不改 TLV 协议。

计划：

- 在 `ml_worker` CLI 增加参数：
  - `--codec <h264|hevc|av1>`
  - `--chroma <420|444>`
  - `--bitdepth <8|10>`
- 根据参数构造 `STREAM_CONFIGURATION.supportedVideoFormats`。
- 默认值保持兼容：
  - `codec=h264`
  - `chroma=420`
  - `bitdepth=8`
- 启动时打印：
  - 请求的视频格式掩码
  - 是否请求 444 / 10-bit
  - 主机 `serverCodecModeSupport`
- 在 `video_callbacks` 中打印实际协商到的 `videoFormat`，便于排查 Sunshine 是否真的返回 444 profile。

交付结果：

- `ml_worker` 可以显式请求 `H.264 High 4:4:4 8-bit`。
- 如果用户显式请求 `HEVC/AV1 444`，当前仓库可以发起协商，但下游仍未完整支持，需要在日志和文档中明确。

### Phase 2: stream_server 解码补齐

目标：让下游能正确吃到 4:4:4 解码帧。

计划：

- `DecodeFormat` 新增 `DECODE_FMT_YUV444P`。
- 识别并接受：
  - `AV_PIX_FMT_YUV444P`
  - 视需要扩展 `AV_PIX_FMT_YUV444P10LE`
- 更新 `plane_height_for_pix_fmt()`：
  - `YUV444P` 的 Y/U/V 三个 plane 高度都为 `height`。
- 更新 `decoder_decode()` / `decoder_flush()` 的格式映射。
- 对 CPU 解码先打通，再补硬解下载路径。

交付结果：

- H.264 High 4:4:4 8-bit 至少在 CPU 解码路径可用。
- `frame->format` 不再落到 `DECODE_FMT_NONE`。

### Phase 3: 硬解和格式转换

目标：保证 VAAPI / CUDA 路径和 BGRA 转换可用。

计划：

- VAAPI 不再把 `frames_ctx->sw_format` 固定为 `AV_PIX_FMT_NV12`。
- 检查 CUDA 下载后的 `sw_frame->format`，接受 `YUV444P`。
- `decoder_convert_format()` 补 `YUV444P -> BGRA`。
- 如果要支持 10-bit，再补 `YUV444P10LE -> BGRA` 或先降位深转换。

交付结果：

- 4:4:4 流在硬解路径可落到系统内存。
- YOLO / 预览链路可继续复用 BGRA 输入。

### Phase 4: 协议与消费端感知

目标：让下游知道当前流的 codec / chroma / bit depth。

计划：

- 评估是否扩展 `STREAM_START` 元信息：
  - codec
  - chroma
  - bit depth
- 如果扩展协议，需同时更新 producer / consumer。
- 若短期不改协议，则在接收端通过 codec parser/bitstream probe 自动探测。

交付结果：

- `stream_server` 不再假设“所有输入都是 H.264 4:2:0”。

## 风险

- H.264 4:4:4、HEVC 4:4:4、AV1 4:4:4 的宿主支持差异较大，必须依赖 `serverCodecModeSupport` 和实际 RTSP 协商结果。
- 当前 `stream_server` 仍是 H.264-only 解码入口，若 Phase 1 开放 HEVC/AV1，请求成功后下游会失败。
- 10-bit 4:4:4 会进一步牵涉格式转换、推理输入和显示链路，不能和 8-bit 4:4:4 混为一谈。

## 验证计划

### Phase 1 验证

- 启动 `ml_worker`：
  - `--codec h264 --chroma 444 --bitdepth 8`
- 观察启动日志中的：
  - 请求 mask
  - `serverCodecModeSupport`
  - `worker_setup()` 收到的实际 `videoFormat`
- 用 H264 tap 抓流后 `ffprobe` 检查输出是否为 `yuv444p` / High 4:4:4。

### Phase 2 验证

- 用抓到的 4:4:4 H.264 码流喂 `stream_server/stream_receiver_decode` 或最小解码测试。
- 确认解码后 `DecodedFrame.format == DECODE_FMT_YUV444P`。

### Phase 3 验证

- 分别验证 CPU / VAAPI / CUDA 路径。
- 验证 `decoder_convert_format()` 输出的 BGRA 没有色偏或平面错位。

## 本次落地范围

本次先完成 Phase 1：

- 新增计划文档。
- `ml_worker` 支持按参数动态请求 444。
- 补充协商日志，明确当前请求和实际协商结果。

本次不完成：

- `stream_server` 的 4:4:4 解码。
- 协议扩展。
- 10-bit 消费链路。
