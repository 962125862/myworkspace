"""Mock H264 tap client

用法:
  python3 mock_h264_tap_client.py --host 127.0.0.1 --port 19090 --stream 1 --out out.h264

说明:
  - 连接 stream_server 的 H264 tap，发送订阅命令，然后把收到的数据原样写入文件
  - 可配合 ffplay 验证:
      ffplay -fflags nobuffer -flags low_delay -f h264 -i out.h264
"""

import argparse
import socket


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19090)
    ap.add_argument("--stream", type=int, default=1)
    ap.add_argument("--out", default="out.h264")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port), timeout=5)
    s.sendall(f"SUB {args.stream}\n".encode())
    s.settimeout(5)

    with open(args.out, "wb") as f:
        while True:
            try:
                data = s.recv(1 << 20)
            except socket.timeout:
                continue
            if not data:
                break
            f.write(data)
            f.flush()


if __name__ == "__main__":
    main()

