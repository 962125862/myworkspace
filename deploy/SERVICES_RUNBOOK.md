# Services Runbook (ml_worker -> stream_server -> shm_zmq_bridge)

This document describes how to run the full pipeline:

- Producer: `ml_worker` (Docker via `deploy/mlctl.sh`) pushes H.264 bytestream over TCP
- Receiver/decoder: `stream_server` (runs on 192.168.11.31)
- Frame bridge: `shm_zmq_bridge.py` reads `/dev/shm/stream_server_stream_XX` and serves frames over ZMQ
- Consumer: ZMQ DEALER clients (Linux/Windows) fetch latest frames

Defaults used in this repo setup:

- `stream_server` listens on `0.0.0.0:19000`
- ZMQ bridge listens on `tcp://*:5566`
- Shared memory objects: `/dev/shm/stream_server_stream_01`, `/dev/shm/stream_server_stream_02`, ...

## 1. Start On 192.168.11.31 (stream_server)

### 1.1 Build

```bash
cd /home/gejun/work/my_ml_work/stream_server
make -j
```

Expected binary:

```bash
ls -l /home/gejun/work/my_ml_work/stream_server/build/stream_server
```

### 1.2 Run (Intel iGPU Decode + SHM)

```bash
cd /home/gejun/work/my_ml_work/stream_server
DECODE_BACKEND=intel ENABLE_SHM=1 \
  ./build/stream_server -h 0.0.0.0 -p 19000 -c 20 -s 5
```

Verify it is listening:

```bash
ss -lntp | grep ':19000'
```

Verify SHM exists (after stream starts):

```bash
ls -l /dev/shm/stream_server_stream_01
```

### 1.3 Optional: Run As user systemd Service

Create `~/.config/systemd/user/stream_server_19000.service`:

```ini
[Unit]
Description=my_ml_work stream_server (19000)
After=network.target

[Service]
Type=simple
WorkingDirectory=/home/gejun/work/my_ml_work/stream_server
Environment=ENABLE_SHM=1
Environment=DECODE_BACKEND=intel
ExecStart=/home/gejun/work/my_ml_work/stream_server/build/stream_server -h 0.0.0.0 -p 19000 -c 20 -s 5
Restart=always
RestartSec=1

[Install]
WantedBy=default.target
```

Enable and start:

```bash
systemctl --user daemon-reload
systemctl --user enable --now stream_server_19000.service
systemctl --user status stream_server_19000.service --no-pager -l
```

## 2. Start On Local Machine (ml_worker via mlctl)

### 2.1 Ensure worker config

Edit `deploy/workers/worker00.conf` and ensure:

- `TCP_HOST="192.168.11.31"`
- `TCP_PORT="19000"`
- `STREAM_ID="1"` (or other per-stream value)

### 2.2 Build image (once)

```bash
cd /home/gejun/work/my_ml_work
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./deploy/build_image.sh
```

### 2.3 Start streaming

```bash
cd /home/gejun/work/my_ml_work
./deploy/mlctl.sh up worker00
./deploy/mlctl.sh logs worker00
```

Note: Sunshine may reject start if the host concurrent stream limit is reached.

## 3. Start On 192.168.11.31 (ZMQ SHM Bridge)

### 3.1 Python env

On 192.168.11.31, use the existing venv:

```bash
source /home/my_server/bin/activate
python -c 'import zmq, numpy, cv2; print("ok")'
```

### 3.2 Run bridge (recommended: pump + cache)

This makes multi-client consumption safe by avoiding multiple readers racing on `request_seq`.

```bash
cd /home/gejun/work/my_ml_work
source /home/my_server/bin/activate

python python_dir/shm_zmq_bridge.py \
  --bind tcp://*:5566 \
  --workers 1 \
  --pump-streams 1
```

Verify listening:

```bash
ss -lntp | grep ':5566'
```

### 3.3 Optional: Run bridge as user systemd Service

Create `~/.config/systemd/user/shm_zmq_bridge.service`:

```ini
[Unit]
Description=ZMQ bridge for stream_server shm frames
After=network.target

[Service]
Type=simple
WorkingDirectory=/home/gejun/work/my_ml_work
Environment=PYTHONUNBUFFERED=1
ExecStart=/bin/bash -lc "source /home/my_server/bin/activate && exec python /home/gejun/work/my_ml_work/python_dir/shm_zmq_bridge.py --bind tcp://*:5566 --workers 1 --pump-streams 1"
Restart=always
RestartSec=1

[Install]
WantedBy=default.target
```

Enable and start:

```bash
systemctl --user daemon-reload
systemctl --user enable --now shm_zmq_bridge.service
systemctl --user status shm_zmq_bridge.service --no-pager -l
```

## 4. Consumer (Linux)

Fetch one frame:

```bash
python3 python_dir/shm_zmq_bridge_client.py \
  --addr tcp://192.168.11.31:5566 \
  --stream-id 1 \
  --out-prefix /tmp/frame
```

Save PNG using OpenCV (run on 192.168.11.31 venv, or any machine with cv2 installed):

```bash
python python_dir/shm_zmq_bridge_client_save_png.py \
  --addr tcp://192.168.11.31:5566 \
  --stream-id 1 \
  --out /tmp/frame.png
```

## 5. Consumer (Windows)

Windows does not need access to `/dev/shm`. It only connects to the ZMQ TCP endpoint.

### 5.1 Install Python deps

On Windows PowerShell:

```powershell
py -m pip install pyzmq numpy opencv-python
```

### 5.2 Run client

```powershell
py python_dir\shm_zmq_bridge_client.py --addr tcp://192.168.11.31:5566 --stream-id 1 --out-prefix C:\\Temp\\frame
```

To save PNG via OpenCV:

```powershell
py python_dir\shm_zmq_bridge_client_save_png.py --addr tcp://192.168.11.31:5566 --stream-id 1 --out C:\\Temp\\frame.png
```

### 5.3 Firewall

Ensure 192.168.11.31 allows inbound TCP on port `5566`.

## 6. Multi-stream Notes

- Use distinct `STREAM_ID` per upstream stream (`worker00.conf: STREAM_ID=1`, `worker01.conf: STREAM_ID=2`, ...).
- `stream_server` publishes SHM per stream: `/dev/shm/stream_server_stream_01`, `/02`, ...
- Start bridge with `--pump-streams 1,2,3` to cache multiple streams.
- Consumers should use the cached endpoint (client defaults to it) so multiple clients do not race on SHM `request_seq`.

