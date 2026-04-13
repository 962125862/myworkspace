# Stream Server Benchmark 2026-04-13

Date: `2026-04-13`
Host: `192.168.11.31`

## Scope

This change set targets the `stream_server` BGR conversion path and related
observability.

Changed runtime code:

- fix a use-after-free in `zmq_bridge.c`
- add cached `SwsContext` reuse for `ctx == NULL` conversion calls
- add a dedicated `VUYX -> BGR24` fast path for Intel VA-API output
- add x86 SIMD dispatch for the `VUYX -> BGR24` fast path
- add process-level conversion-path stats to the stream stats log

Changed tooling:

- `test_20streams_hw.c` now supports forcing `intel|nvidia|cpu|auto`
- `python_dir/zmq_multi_stream_bgr_perf.py` adds paced multi-stream IPC BGR
  benchmark coverage

Not changed:

- no `ml_worker` changes
- no on-wire TLV protocol changes
- no service file changes

## Before / After

Intel `VUYX -> BGR24` conversion timing from live service stats:

- before this optimization: `vuyx_fast avg=1.972 ms`
- after this optimization: `vuyx_fast avg=0.885 ms`

Reduction:

- absolute: `1.087 ms`
- relative: about `55.1%`

## Live 20-stream IPC BGR benchmark

Method:

- real `stream 1-20` on `192.168.11.31`
- `ipc:///tmp/stream_server_bgr.sock`
- `20` concurrent IPC clients
- paced at `30 fps` per stream for `30s`
- compare `--decode-backend intel` vs `--decode-backend nvidia`

### Intel

- active stream fps avg: `31.7`
- service `Dec`: `0.303 ms`
- service `Xfer`: `2.011 ms`
- service `Convert`: `0.885 ms` (`vuyx_fast`)
- service-side BGR critical path approx: `2.896 ms` (`Xfer + Convert`)
- IPC success throughput: `343.43 fps` total, about `17.17 fps/stream`
- IPC avg RTT: `58.246 ms`
- IPC p95 RTT: `64.837 ms`
- avg frame age: `1.075 ms`
- process avg CPU: `134.8%`
- process avg RSS: `534.7 MB`
- Intel GT frequency proxy avg: `43.2%`

### NVIDIA

- active stream fps avg: `31.5`
- service `Dec`: `1.551 ms`
- service `Xfer`: `0.906 ms`
- service `Convert`: `0.624 ms` (`libyuv`)
- service-side BGR critical path approx: `1.530 ms` (`Xfer + Convert`)
- IPC success throughput: `595.53 fps` total, about `29.78 fps/stream`
- IPC avg RTT: `11.475 ms`
- IPC p95 RTT: `27.278 ms`
- avg frame age: `28.772 ms`
- process avg CPU: `140.6%`
- process avg RSS: `916.1 MB`
- NVIDIA GPU util avg: `17.0%`
- NVIDIA GPU memory avg: `4460 MB`

## Why Intel stops around 17 fps/stream

For `GET_LATEST_BGR`, `STREAM_DEFER_HW_DOWNLOAD=on` means the BGR request path
is dominated by:

1. `av_hwframe_transfer_data()` from GPU to CPU
2. CPU-side pixel conversion to `BGR24`

The bridge handles requests in a single ZMQ ROUTER thread, so total throughput
is effectively bounded by per-request `Xfer + Convert` cost.

Measured on this host:

- Intel: about `2.011 + 0.885 = 2.896 ms/request`
- NVIDIA: about `0.906 + 0.624 = 1.530 ms/request`

That makes the Intel BGR request path about `1.89x` slower than NVIDIA under
this workload, which matches the observed IPC throughput gap.

The low Intel `frame age` does not mean the path is faster. It means the bridge
often converts a very recent frame when it finally services the queued request.
The higher Intel RTT comes from waiting in the single bridge thread.

## Regression check

Observed:

- build and deployment succeeded
- live `stream 1-20` remained decodable on both Intel and NVIDIA
- no protocol compatibility impact
- no functional regression observed in `GET_LATEST_BGR`

Known existing limitation not addressed here:

- the bridge is still single-threaded
- `request_new` is still ignored by the server side
