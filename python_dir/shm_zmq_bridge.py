"""ZMQ bridge: read latest NV12 frame from stream_server shared memory and serve via ZMQ.

This module mimics the ROUTER -> (inproc) -> worker-thread pattern you posted.

Shared memory layout is defined by stream_server/include/shm_frame.h (ShmFrameHeader).
The writer (stream_server) publishes frames into POSIX shm objects named:
  /stream_server_stream_01, /stream_server_stream_02, ...
which appear on Linux as:
  /dev/shm/stream_server_stream_01, ...

Protocol (recommended):
Client (DEALER or REQ) -> ROUTER multipart frames:
  DEALER: [cmd][json]
  REQ:    [cmd][json]   (ROUTER will see: [identity][b""][cmd][json])

Where:
  cmd  = b"GET_SHM_NV12"
  json = b"{\"stream_id\": 1, \"timeout_ms\": 1000, \"request_new\": true}"

Server -> client multipart frames:
  ROUTER will send back:
    DEALER: [status][meta_json][y_plane][uv_plane]
    REQ:    [b""][status][meta_json][y_plane][uv_plane]
  status=b"OK" or b"ERR"

Notes:
  - If your client is REQ, ROUTER will receive an empty delimiter frame b"".
    This code will preserve the delimiter in replies so REQ can work.
  - This code copies shm bytes into Python bytes before sending (safe for TCP).
    If you only use inproc/ipc you can optimize with memoryview + copy=False.
  - This implementation assumes little-endian host (x86_64). The shm header is
    written in host endianness by stream_server.
"""

from __future__ import annotations

import json
import os
import struct
import threading
import time
from dataclasses import dataclass
from typing import Dict, Optional, Tuple


try:
    import zmq  # type: ignore
except ModuleNotFoundError as e:  # pragma: no cover
    raise ModuleNotFoundError(
        "pyzmq is required. Install with: pip install pyzmq"
    ) from e


# ---- ShmFrameHeader parsing (stream_server/include/shm_frame.h) ----

_U32 = struct.Struct("<I")

MAGIC = 0x53534652  # 'SSFR'
VERSION = 1

OFF_MAGIC = 0
OFF_VERSION = 4
OFF_HEADER_SIZE = 6
OFF_WRITE_SEQ = 8
OFF_REQUEST_SEQ = 12
OFF_PUBLISH_SEQ = 16
OFF_WIDTH = 20
OFF_HEIGHT = 24
OFF_PIXFMT = 28
OFF_LINESIZE = 32
OFF_PLANE_OFFSET = 48
OFF_PLANE_SIZE = 64
OFF_PTS = 80
OFF_MONO_NS = 88
OFF_KEY_FRAME = 96
OFF_TOTAL_DATA_SIZE = 100
OFF_TOTAL_SHM_SIZE = 104


@dataclass
class ShmMeta:
    stream_id: int
    width: int
    height: int
    pixfmt: int
    linesize: Tuple[int, int, int, int]
    plane_offset: Tuple[int, int, int, int]
    plane_size: Tuple[int, int, int, int]
    pts: int
    mono_ns: int
    key_frame: int
    request_seq: int
    publish_seq: int


class ShmFrameReader:
    """Read frames from /dev/shm/stream_server_stream_XX.

    This reader supports the writer's on-demand publish mechanism:
      - increment request_seq
      - wait publish_seq >= request_seq
      - read header+planes with seqlock(write_seq)
    """

    def __init__(self, stream_id: int, dev_shm_dir: str = "/dev/shm"):
        self.stream_id = int(stream_id)
        # stream_server uses shm name: /stream_server_stream_%02d
        self._name = f"stream_server_stream_{self.stream_id:02d}"
        self._path = os.path.join(dev_shm_dir, self._name)
        self._fd: Optional[int] = None
        self._mm = None
        self._inode: Optional[int] = None
        self._size: Optional[int] = None

    def _maybe_reopen_if_recreated(self) -> None:
        """Reopen mmap if the shm file was unlinked/recreated.

        stream_server recreates shm objects with shm_unlink()+shm_open() on startup.
        Existing mmaps keep pointing at the old (now nameless) object, so readers
        must detect this and remap.
        """
        if self._mm is None:
            return
        try:
            st = os.stat(self._path)
        except FileNotFoundError:
            # Let subsequent open() raise a clear error.
            self.close()
            return
        if self._inode is not None and st.st_ino != self._inode:
            self.close()
            self.open()
            return
        if self._size is not None and st.st_size != self._size:
            self.close()
            self.open()
            return

    def open(self) -> None:
        if self._mm is not None:
            return
        fd = os.open(self._path, os.O_RDWR)
        st = os.fstat(fd)
        import mmap

        mm = mmap.mmap(fd, st.st_size, access=mmap.ACCESS_WRITE)

        # Basic validation
        magic = struct.unpack_from("<I", mm, OFF_MAGIC)[0]
        version = struct.unpack_from("<H", mm, OFF_VERSION)[0]
        if magic != MAGIC or version != VERSION:
            mm.close()
            os.close(fd)
            raise RuntimeError(
                f"bad shm header: magic=0x{magic:x} version={version} (expected magic=0x{MAGIC:x} v{VERSION})"
            )

        self._fd = fd
        self._mm = mm
        self._inode = st.st_ino
        self._size = st.st_size

    def close(self) -> None:
        if self._mm is not None:
            self._mm.close()
            self._mm = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
        self._inode = None
        self._size = None

    def _read_u32(self, off: int) -> int:
        return struct.unpack_from("<I", self._mm, off)[0]

    def _write_u32(self, off: int, v: int) -> None:
        struct.pack_into("<I", self._mm, off, v & 0xFFFFFFFF)

    def request_next(self) -> int:
        """Best-effort request_seq++.

        Note: this is not a true atomic fetch_add; it is usually OK if you have
        a single reader process.
        """
        cur = self._read_u32(OFF_REQUEST_SEQ)
        nxt = (cur + 1) & 0xFFFFFFFF
        self._write_u32(OFF_REQUEST_SEQ, nxt)
        return nxt

    def wait_published(self, req: int, timeout_ms: int) -> bool:
        deadline = time.monotonic() + max(0, timeout_ms) / 1000.0
        while True:
            pub = self._read_u32(OFF_PUBLISH_SEQ)
            if pub >= req:
                return True
            if timeout_ms >= 0 and time.monotonic() >= deadline:
                return False
            time.sleep(0.001)

    def wait_next_publish(self, prev_pub: int, req: int, timeout_ms: int) -> bool:
        """Wait until writer publishes a newer frame.

        This is more robust than waiting for publish_seq >= req because:
        - In normal on-demand mode: publish_seq is set to request_seq (monotonic).
        - In SHM_ALWAYS=1 mode: publish_seq is an internal counter unrelated to request_seq.
        """
        deadline = time.monotonic() + max(0, timeout_ms) / 1000.0
        while True:
            pub = self._read_u32(OFF_PUBLISH_SEQ)
            if pub != prev_pub:
                return True
            # also accept the traditional condition
            if pub >= req:
                return True
            if timeout_ms >= 0 and time.monotonic() >= deadline:
                return False
            time.sleep(0.001)

    def read_latest_nv12(self, *, timeout_ms: int = 1000, request_new: bool = True) -> Tuple[ShmMeta, bytes, bytes]:
        # Retry once to handle shm recreation while the reader process is alive.
        for attempt in range(2):
            self.open()
            self._maybe_reopen_if_recreated()

            req = self._read_u32(OFF_REQUEST_SEQ)
            if request_new:
                prev_pub = self._read_u32(OFF_PUBLISH_SEQ)
                req = self.request_next()
                ok = self.wait_next_publish(prev_pub, req, timeout_ms=timeout_ms)
                if not ok:
                    if attempt == 0:
                        # stream_server may have recreated shm; remap and retry.
                        self.close()
                        continue
                    raise TimeoutError(
                        f"timeout waiting for publish (prev_publish_seq={prev_pub}, request_seq={req})"
                    )
            break

        # Seqlock read: verify write_seq stable and even
        while True:
            self._maybe_reopen_if_recreated()
            seq1 = self._read_u32(OFF_WRITE_SEQ)
            if seq1 & 1:
                continue

            width = self._read_u32(OFF_WIDTH)
            height = self._read_u32(OFF_HEIGHT)
            pixfmt = self._read_u32(OFF_PIXFMT)
            linesize = struct.unpack_from("<4I", self._mm, OFF_LINESIZE)
            plane_offset = struct.unpack_from("<4I", self._mm, OFF_PLANE_OFFSET)
            plane_size = struct.unpack_from("<4I", self._mm, OFF_PLANE_SIZE)
            pts, mono_ns = struct.unpack_from("<QQ", self._mm, OFF_PTS)
            key_frame = self._read_u32(OFF_KEY_FRAME)

            pub = self._read_u32(OFF_PUBLISH_SEQ)

            # Validate published format
            # stream_server writer always publishes NV12 to shm.
            if pixfmt != 1:  # SHM_PIXFMT_NV12
                raise RuntimeError(f"unexpected pixfmt={pixfmt} (expected NV12=1)")

            # NV12 expected: plane0(Y) + plane1(UV)
            off0, off1 = plane_offset[0], plane_offset[1]
            sz0, sz1 = plane_size[0], plane_size[1]

            # Bounds check
            mm_len = len(self._mm)
            if off0 + sz0 > mm_len or off1 + sz1 > mm_len:
                raise RuntimeError(
                    f"plane out of bounds: len={mm_len} off0={off0} sz0={sz0} off1={off1} sz1={sz1}"
                )

            y = bytes(self._mm[off0: off0 + sz0])
            uv = bytes(self._mm[off1: off1 + sz1])

            seq2 = self._read_u32(OFF_WRITE_SEQ)
            if seq1 == seq2 and not (seq2 & 1):
                meta = ShmMeta(
                    stream_id=self.stream_id,
                    width=width,
                    height=height,
                    pixfmt=pixfmt,
                    linesize=tuple(int(x) for x in linesize),
                    plane_offset=tuple(int(x) for x in plane_offset),
                    plane_size=tuple(int(x) for x in plane_size),
                    pts=int(pts),
                    mono_ns=int(mono_ns),
                    key_frame=int(key_frame),
                    request_seq=int(req),
                    publish_seq=int(pub),
                )
                return meta, y, uv


# ---- ZMQ server/worker pattern ----


@dataclass
class CachedFrame:
    meta_json: bytes
    y: bytes
    uv: bytes
    updated_mono_ns: int


_CACHE: Dict[int, CachedFrame] = {}
_CACHE_LOCK = threading.Lock()

_STREAM_LOCKS: Dict[int, threading.Lock] = {}
_STREAM_LOCKS_LOCK = threading.Lock()


def _get_stream_lock(stream_id: int) -> threading.Lock:
    with _STREAM_LOCKS_LOCK:
        lock = _STREAM_LOCKS.get(stream_id)
        if lock is None:
            lock = threading.Lock()
            _STREAM_LOCKS[stream_id] = lock
        return lock


def _cache_put(stream_id: int, meta: ShmMeta, y: bytes, uv: bytes) -> None:
    meta_json = json.dumps(meta.__dict__, ensure_ascii=False).encode("utf-8")
    now_ns = time.monotonic_ns()
    with _CACHE_LOCK:
        _CACHE[stream_id] = CachedFrame(meta_json=meta_json, y=y, uv=uv, updated_mono_ns=now_ns)


def _cache_get(stream_id: int) -> Optional[CachedFrame]:
    with _CACHE_LOCK:
        return _CACHE.get(stream_id)


def _pump_stream(stream_id: int, *, interval_ms: int = 0) -> None:
    """Continuously request/publish frames into cache.

    This makes multi-client usage sane: only the pump bumps request_seq.
    Downstream clients should use GET_LATEST_NV12 (no request_seq writes).
    """

    reader = ShmFrameReader(stream_id)
    while True:
        try:
            with _get_stream_lock(stream_id):
                meta, y, uv = reader.read_latest_nv12(timeout_ms=1000, request_new=True)
            _cache_put(stream_id, meta, y, uv)
        except FileNotFoundError:
            time.sleep(0.2)
        except Exception:
            # Keep the service alive; transient decode/resize/shm recreation issues.
            time.sleep(0.05)

        if interval_ms > 0:
            time.sleep(interval_ms / 1000.0)


def zmq_server(
    *,
    bind_addr: str = "tcp://*:5555",
    backend_in: str = "inproc://shm_backend_in",
    backend_out: str = "inproc://shm_backend_out",
    n_workers: int = 1,
    pump_streams: Optional[Tuple[int, ...]] = None,
    pump_interval_ms: int = 0,
) -> None:
    context = zmq.Context.instance()

    router = context.socket(zmq.ROUTER)
    router.bind(bind_addr)

    pull_from_worker = context.socket(zmq.PULL)
    pull_from_worker.bind(backend_out)

    push_to_worker = context.socket(zmq.PUSH)
    push_to_worker.bind(backend_in)

    poller = zmq.Poller()
    poller.register(router, zmq.POLLIN)
    poller.register(pull_from_worker, zmq.POLLIN)

    for _ in range(max(1, int(n_workers))):
        threading.Thread(
            target=work, args=(backend_in, backend_out), daemon=True
        ).start()

    if pump_streams:
        for sid in pump_streams:
            threading.Thread(
                target=_pump_stream,
                args=(int(sid),),
                kwargs={"interval_ms": int(pump_interval_ms)},
                daemon=True,
            ).start()

    while True:
        events = dict(poller.poll(timeout=1000))

        if router in events:
            frames = router.recv_multipart()
            # forward as-is (keeps identity + optional delimiter)
            push_to_worker.send_multipart(frames)

        if pull_from_worker in events:
            resp_frames = pull_from_worker.recv_multipart()
            router.send_multipart(resp_frames)


def _split_router_envelope(frames):
    """Return (identity, delimiter_or_None, rest_frames)."""
    if not frames:
        return None, None, []
    identity = frames[0]
    rest = list(frames[1:])
    delim = None
    if rest and rest[0] == b"":
        delim = rest.pop(0)
    return identity, delim, rest


def work(backend_in: str, backend_out: str) -> None:
    context = zmq.Context.instance()

    work_pull = context.socket(zmq.PULL)
    work_pull.connect(backend_in)

    work_push = context.socket(zmq.PUSH)
    work_push.connect(backend_out)

    readers: Dict[int, ShmFrameReader] = {}

    while True:
        msg = work_pull.recv_multipart()
        identity, delim, rest = _split_router_envelope(msg)
        if identity is None:
            continue

        if len(rest) < 1:
            continue

        cmd = rest[0]
        payload = rest[1] if len(rest) >= 2 else b"{}"

        resp = [identity]
        if delim is not None:
            resp.append(b"")

        try:
            if cmd == b"GET_SHM_NV12":
                try:
                    req_obj = json.loads(payload.decode("utf-8")) if payload else {}
                except Exception:
                    req_obj = {}

                stream_id = int(req_obj.get("stream_id", 1))
                timeout_ms = int(req_obj.get("timeout_ms", 1000))
                request_new = bool(req_obj.get("request_new", True))

                r = readers.get(stream_id)
                if r is None:
                    r = ShmFrameReader(stream_id)
                    readers[stream_id] = r

                with _get_stream_lock(stream_id):
                    meta, y, uv = r.read_latest_nv12(timeout_ms=timeout_ms, request_new=request_new)

                _cache_put(stream_id, meta, y, uv)
                cached = _cache_get(stream_id)
                resp.extend([b"OK", cached.meta_json, cached.y, cached.uv])

            elif cmd == b"GET_LATEST_NV12":
                try:
                    req_obj = json.loads(payload.decode("utf-8")) if payload else {}
                except Exception:
                    req_obj = {}
                stream_id = int(req_obj.get("stream_id", 1))
                max_age_ms = req_obj.get("max_age_ms")
                cached = _cache_get(stream_id)
                if not cached:
                    resp.extend([b"ERR", b"no cached frame; start pump or call GET_SHM_NV12 first"])
                else:
                    if max_age_ms is not None:
                        age_ms = (time.monotonic_ns() - cached.updated_mono_ns) / 1e6
                        if age_ms > float(max_age_ms):
                            resp.extend([b"ERR", f"cached frame too old: {age_ms:.1f}ms".encode("utf-8")])
                        else:
                            resp.extend([b"OK", cached.meta_json, cached.y, cached.uv])
                    else:
                        resp.extend([b"OK", cached.meta_json, cached.y, cached.uv])

            elif cmd == b"PING":
                resp.extend([b"OK", b"{}"])

            else:
                resp.extend([b"ERR", f"unknown cmd: {cmd!r}".encode("utf-8")])

        except FileNotFoundError:
            resp.extend([b"ERR", b"shm not found. Start stream_server with ENABLE_SHM=1 and ensure /dev/shm/stream_server_stream_XX exists."])
        except TimeoutError as e:
            resp.extend([b"ERR", f"timeout: {e}".encode("utf-8")])
        except Exception as e:
            resp.extend([b"ERR", f"exception: {type(e).__name__}: {e}".encode("utf-8")])

        work_push.send_multipart(resp)


if __name__ == "__main__":  # pragma: no cover
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="tcp://*:5555")
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--pump-streams", default="", help="comma-separated stream ids to pump into cache, e.g. 1,2")
    ap.add_argument("--pump-interval-ms", type=int, default=0)
    args = ap.parse_args()

    pump_streams: Optional[Tuple[int, ...]] = None
    if args.pump_streams.strip():
        pump_streams = tuple(int(x) for x in args.pump_streams.split(",") if x.strip())

    zmq_server(
        bind_addr=args.bind,
        n_workers=args.workers,
        pump_streams=pump_streams,
        pump_interval_ms=args.pump_interval_ms,
    )
