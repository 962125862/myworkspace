import os
import time
import mmap
import ctypes as C

import cv2
import numpy as np

SHM_PATH = "/dev/shm/ml_stream_00"
ML_SHM_MAGIC = 0x4D4C5955
ML_SHM_PIXFMT_I420 = 1


class MlShmHeader(C.Structure):
    _fields_ = [
        ("magic", C.c_uint32),
        ("version", C.c_uint32),
        ("header_bytes", C.c_uint32),
        ("slot_header_bytes", C.c_uint32),

        ("width", C.c_uint32),
        ("height", C.c_uint32),
        ("pix_fmt", C.c_uint32),
        ("slot_count", C.c_uint32),

        ("stride_y", C.c_uint32),
        ("stride_u", C.c_uint32),
        ("stride_v", C.c_uint32),
        ("reserved0", C.c_uint32),

        ("bytes_y", C.c_uint32),
        ("bytes_u", C.c_uint32),
        ("bytes_v", C.c_uint32),
        ("reserved1", C.c_uint32),

        ("color_space", C.c_uint32),
        ("color_range", C.c_uint32),
        ("fps", C.c_uint32),
        ("status", C.c_uint32),

        ("writer_pid", C.c_uint32),
        ("last_error_code", C.c_int32),

        ("slot_bytes", C.c_uint64),
        ("total_bytes", C.c_uint64),
        ("latest_frame_id", C.c_uint64),
        ("heartbeat_ns", C.c_uint64),

        ("current_slot", C.c_uint32),
        ("reserved", C.c_uint32 * 9),
    ]



class MlShmSlotHeader(C.Structure):
    _fields_ = [
        ("seq", C.c_uint32),
        ("valid", C.c_uint32),
        ("frame_id", C.c_uint64),
        ("monotonic_ns", C.c_uint64),
        ("reserved", C.c_uint32 * 10),
    ]


def open_shm(path=SHM_PATH):
    fd = os.open(path, os.O_RDONLY)
    size = os.fstat(fd).st_size
    mm = mmap.mmap(fd, size, access=mmap.ACCESS_READ)
    os.close(fd)

    hdr = MlShmHeader.from_buffer_copy(mm, 0)

    if hdr.magic != ML_SHM_MAGIC:
        raise RuntimeError(f"bad magic: 0x{hdr.magic:08x}")
    if hdr.pix_fmt != ML_SHM_PIXFMT_I420:
        raise RuntimeError(f"unsupported pix_fmt={hdr.pix_fmt}")

    print(
        f"opened shm: {path}\n"
        f"  size={size}\n"
        f"  width={hdr.width} height={hdr.height}\n"
        f"  slot_count={hdr.slot_count}\n"
        f"  stride_y={hdr.stride_y} stride_u={hdr.stride_u} stride_v={hdr.stride_v}\n"
        f"  bytes_y={hdr.bytes_y} bytes_u={hdr.bytes_u} bytes_v={hdr.bytes_v}"
    )

    return mm


def read_latest_i420(mm):
    hdr = MlShmHeader.from_buffer_copy(mm, 0)

    slot_off = hdr.header_bytes + hdr.current_slot * hdr.slot_bytes
    data_off = slot_off + hdr.slot_header_bytes
    total_data = hdr.bytes_y + hdr.bytes_u + hdr.bytes_v

    sh1 = MlShmSlotHeader.from_buffer_copy(mm, slot_off)

    if sh1.valid != 1:
        return None

    if sh1.seq & 1:
        # writer is writing this slot
        return None

    # 拷贝一份 payload 到本地 bytes，避免后续被改
    payload = mm[data_off:data_off + total_data]

    sh2 = MlShmSlotHeader.from_buffer_copy(mm, slot_off)

    # 双读 seq，防止撕裂
    if sh1.seq != sh2.seq or (sh2.seq & 1) or sh2.valid != 1 or sh1.frame_id != sh2.frame_id:
        return None

    buf = np.frombuffer(payload, dtype=np.uint8)

    y0 = 0
    y1 = hdr.bytes_y
    u1 = y1 + hdr.bytes_u
    v1 = u1 + hdr.bytes_v

    y = buf[y0:y1].reshape(hdr.height, hdr.stride_y)[:, :hdr.width]
    u = buf[y1:u1].reshape(hdr.height // 2, hdr.stride_u)[:, :hdr.width // 2]
    v = buf[u1:v1].reshape(hdr.height // 2, hdr.stride_v)[:, :hdr.width // 2]

    return hdr, sh2, y, u, v

def i420_to_bgr(y, u, v):
    h, w = y.shape
    i420 = np.empty((h * 3 // 2, w), dtype=np.uint8)
    i420[:h, :] = y
    i420[h:h + h // 4, :] = u.reshape(-1, w)
    i420[h + h // 4:, :] = v.reshape(-1, w)
    bgr = cv2.cvtColor(i420, cv2.COLOR_YUV2BGR_I420)
    return bgr
def main():
    mm = open_shm()
    last_frame_id = -1
    fps_t0 = time.time()
    fps_count = 0

    while True:
        item = read_latest_i420(mm)
        if item is None:
            time.sleep(0.002)
            continue

        hdr, sh, y, u, v = item

        if sh.frame_id == last_frame_id:
            time.sleep(0.001)
            continue

        last_frame_id = sh.frame_id
        fps_count += 1

        now = time.time()
        if now - fps_t0 >= 1.0:
            print(f"reader fps ~= {fps_count}, latest_frame_id={sh.frame_id}")
            fps_t0 = now
            fps_count = 0
        frame_bgr=i420_to_bgr(y, u, v)
        # 先只显示 Y 平面（灰度）
        cv2.imshow("Y", frame_bgr)

        key = cv2.waitKey(1) & 0xFF
        if key == 27 or key == ord('q'):
            break

    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
