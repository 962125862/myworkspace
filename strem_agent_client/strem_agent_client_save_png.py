"""Headless client: connect to agent video TCP, decode first frame, save PNG, exit.

Useful for testing without a GUI window.
"""

from __future__ import annotations

import argparse
import socket
import subprocess

import cv2  # type: ignore


def main() -> int:
    # Prefer PyAV, but provide an ffmpeg(1) fallback so this tool works even if
    # PyAV cannot be installed.
    av = None
    try:  # pragma: no cover
        import av as _av  # type: ignore

        av = _av
    except Exception:
        av = None

    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--video-port", type=int, default=31234)
    ap.add_argument("--stream-id", type=int, default=1)
    ap.add_argument("--token", default="")
    ap.add_argument("--out", default="/tmp/agent_frame.png")
    ap.add_argument("--timeout-sec", type=float, default=10.0)
    args = ap.parse_args()

    s = socket.create_connection((args.host, int(args.video_port)), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    if args.token:
        s.sendall(f"AUTH {args.token}\n".encode("utf-8"))
    s.sendall(f"SUB {args.stream_id}\n".encode("utf-8"))

    # create_connection(timeout=...) sets socket recv timeout too.
    # If upstream hasn't started streaming yet, keep waiting.
    s.settimeout(1.0)

    s.settimeout(1.0)
    deadline = __import__("time").time() + float(args.timeout_sec)
    buf = bytearray(1024 * 1024)

    PNG_SIG = b"\x89PNG\r\n\x1a\n"

    def try_extract_one_png(buf2: bytearray) -> bytes | None:
        sig_i = buf2.find(PNG_SIG)
        if sig_i < 0:
            if len(buf2) > len(PNG_SIG):
                del buf2[:-len(PNG_SIG)]
            return None
        if sig_i > 0:
            del buf2[:sig_i]
        i = len(PNG_SIG)
        while True:
            if len(buf2) < i + 8:
                return None
            ln = int.from_bytes(buf2[i : i + 4], "big")
            typ = bytes(buf2[i + 4 : i + 8])
            chunk_total = 4 + 4 + ln + 4
            if len(buf2) < i + chunk_total:
                return None
            i += chunk_total
            if typ == b"IEND":
                png = bytes(buf2[:i])
                del buf2[:i]
                return png

    if av is not None:
        codec = av.CodecContext.create("h264", "r")

        # PyAV API compatibility: prefer CodecContext.parse when available.
        use_codec_parse = hasattr(codec, "parse")
        parser = None
        if not use_codec_parse:
            if not hasattr(av, "Parser"):
                raise RuntimeError(
                    "This PyAV build has neither CodecContext.parse nor av.Parser. "
                    "Please upgrade PyAV."
                )
            parser = av.Parser.create(codec.name)

        while __import__("time").time() < deadline:
            try:
                n = s.recv_into(buf)
            except socket.timeout:
                continue
            if n <= 0:
                break
            data = bytes(memoryview(buf)[:n])


            packets = codec.parse(data) if use_codec_parse else parser.parse(data)  # type: ignore[union-attr]
            for pkt in packets:
                try:
                    frames = codec.decode(pkt)
                except Exception:
                    # Late-join commonly hits decode errors until an IDR arrives.
                    frames = []
                for fr in frames:
                    img = fr.to_ndarray(format="bgr24")
                    ok = cv2.imwrite(args.out, img)
                    if not ok:
                        raise SystemExit(f"cv2.imwrite failed: {args.out}")
                    print(f"OK saved {args.out} {img.shape[1]}x{img.shape[0]}")
                    return 0
    else:
        # ffmpeg fallback: h264->single png
        cmd = [
            "ffmpeg",
            "-loglevel",
            "error",
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
            "-probesize",
            "32",
            "-analyzeduration",
            "0",
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-frames:v",
            "1",
            "-f",
            "image2pipe",
            "-vcodec",
            "png",
            "pipe:1",
        ]
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=0,
        )
        assert proc.stdin is not None
        assert proc.stdout is not None

        out_buf = bytearray()

        while __import__("time").time() < deadline:
            try:
                n = s.recv_into(buf)
            except socket.timeout:
                n = 0
            if n > 0:
                try:
                    proc.stdin.write(memoryview(buf)[:n])
                    proc.stdin.flush()
                except BrokenPipeError:
                    break

            chunk = proc.stdout.read(65536)
            if chunk:
                out_buf += chunk
                png = try_extract_one_png(out_buf)
                if png is not None:
                    arr = __import__("numpy").frombuffer(png, dtype=__import__("numpy").uint8)
                    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                    if img is None:
                        break
                    ok = cv2.imwrite(args.out, img)
                    if not ok:
                        raise SystemExit(f"cv2.imwrite failed: {args.out}")
                    print(f"OK saved {args.out} {img.shape[1]}x{img.shape[0]}")
                    try:
                        proc.terminate()
                    except Exception:
                        pass
                    return 0

        try:
            proc.terminate()
        except Exception:
            pass

    raise SystemExit("timeout: no frame decoded")


if __name__ == "__main__":
    raise SystemExit(main())
