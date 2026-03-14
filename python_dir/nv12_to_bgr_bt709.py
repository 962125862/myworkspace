"""NV12 to BGR conversion with BT.709 colorspace support.

BT.709 是 HDTV 标准，相比 BT.601（SDTV）能更准确还原：
- 白色：不会偏粉/偏绿
- 绿色：更饱和，接近真实
- 黄色：不会偏橙

提供多种实现：
1. libyuv (推荐): SIMD 优化 (SSSE3/AVX2/NEON)，最快
2. PyAV: 使用 ffmpeg sws_scale，准确
3. Numpy: 纯 Python 实现，无额外依赖

Usage:
    from nv12_to_bgr_bt709 import nv12_to_bgr_bt709

    # 方法1: libyuv (推荐，最快，需要编译 libyuv)
    bgr = nv12_to_bgr_bt709(w, h, y, uv, method='libyuv')

    # 方法2: PyAV (需要 pip install av)
    bgr = nv12_to_bgr_bt709(w, h, y, uv, method='pyav')

    # 方法3: Numpy (无额外依赖)
    bgr = nv12_to_bgr_bt709(w, h, y, uv, method='numpy')

    # 方法4: OpenCV (需要 opencv-python，注意：默认是 BT.601)
    bgr = nv12_to_bgr_bt709(w, h, y, uv, method='opencv')
"""

from __future__ import annotations

import numpy as np
from typing import Optional


def nv12_to_bgr_bt709(
    w: int,
    h: int,
    y: bytes,
    uv: bytes,
    method: str = 'libyuv',
    output_bgr: bool = True
) -> np.ndarray:
    """
    Convert NV12 to BGR/RGB using BT.709 colorspace.

    Args:
        w: width
        h: height
        y: Y plane bytes (w * h bytes)
        uv: UV plane bytes (w * h / 2 bytes, interleaved UV)
        method: 'libyuv' (fastest), 'pyav', 'numpy', or 'opencv'
        output_bgr: True for BGR (OpenCV default), False for RGB

    Returns:
        numpy array of shape (h, w, 3), dtype uint8
    """
    if method == 'libyuv':
        return _nv12_to_bgr_libyuv(w, h, y, uv, output_bgr)
    elif method == 'pyav':
        return _nv12_to_bgr_pyav(w, h, y, uv, output_bgr)
    elif method == 'numpy':
        return _nv12_to_bgr_numpy(w, h, y, uv, output_bgr)
    elif method == 'opencv':
        return _nv12_to_bgr_opencv(w, h, y, uv, output_bgr)
    else:
        raise ValueError(f"Unknown method: {method}. Use 'libyuv', 'pyav', 'numpy', or 'opencv'.")


def _nv12_to_bgr_libyuv(w: int, h: int, y: bytes, uv: bytes, output_bgr: bool) -> np.ndarray:
    """
    使用 libyuv 进行 NV12 -> BGR/RGB 转换，支持 BT.709。

    libyuv 是 Google 的高性能 YUV 转换库，使用 SIMD (SSSE3/AVX2/NEON) 优化。
    这是性能最好的方法。

    需要先编译 libyuv_wrapper 共享库:
        cd python_dir && gcc -O3 -fPIC -shared -o libyuv_wrapper.so \
            nv12_to_bgr_libyuv.c -lyuv -I/usr/include/libyuv
    """
    import ctypes
    import os
    import sys

    # 根据平台选择库文件
    if sys.platform == 'win32':
        lib_name = 'libyuv_wrapper.dll'
        lib_paths = [
            os.path.join(os.path.dirname(__file__), lib_name),
            os.path.join(os.path.dirname(__file__), '..', lib_name),
        ]
    elif sys.platform == 'darwin':
        lib_name = 'libyuv_wrapper.dylib'
        lib_paths = [
            os.path.join(os.path.dirname(__file__), lib_name),
            '/usr/local/lib/' + lib_name,
            '/opt/homebrew/lib/' + lib_name,
        ]
    else:  # Linux
        lib_name = 'libyuv_wrapper.so'
        lib_paths = [
            os.path.join(os.path.dirname(__file__), lib_name),
            '/usr/local/lib/' + lib_name,
            '/usr/lib/x86_64-linux-gnu/' + lib_name,
            '/usr/lib/' + lib_name,
        ]

    lib = None
    for path in lib_paths:
        if os.path.exists(path):
            lib = ctypes.CDLL(path)
            break

    if lib is None:
        raise ImportError(
            f"{lib_name} not found. Please compile it:\n"
            f"  Linux:   gcc -O3 -fPIC -shared -o {lib_name} nv12_to_bgr_libyuv.c -lyuv\n"
            f"  Windows: cl /LD /O2 nv12_to_bgr_libyuv.c libyuv.lib /Fe:{lib_name}\n"
            f"  macOS:   gcc -O3 -fPIC -shared -o {lib_name} nv12_to_bgr_libyuv.c -lyuv"
        )

    # 设置函数签名
    # int nv12_to_bgr_bt709_libyuv(
    #     const uint8_t* y, int y_stride,
    #     const uint8_t* uv, int uv_stride,
    #     uint8_t* dst, int dst_stride,
    #     int width, int height, int output_bgr)
    lib.nv12_to_bgr_bt709_libyuv.argtypes = [
        ctypes.c_void_p, ctypes.c_int,  # y, y_stride
        ctypes.c_void_p, ctypes.c_int,  # uv, uv_stride
        ctypes.c_void_p, ctypes.c_int,  # dst, dst_stride
        ctypes.c_int, ctypes.c_int, ctypes.c_int  # width, height, output_bgr
    ]
    lib.nv12_to_bgr_bt709_libyuv.restype = ctypes.c_int

    # 准备输入
    y_arr = np.frombuffer(y, dtype=np.uint8)
    uv_arr = np.frombuffer(uv, dtype=np.uint8)

    # 分配输出缓冲区 (BGR24 或 RGB24)
    dst = np.empty((h, w, 3), dtype=np.uint8)

    # 调用 libyuv
    ret = lib.nv12_to_bgr_bt709_libyuv(
        y_arr.ctypes.data_as(ctypes.c_void_p), w,  # y, y_stride
        uv_arr.ctypes.data_as(ctypes.c_void_p), w,  # uv, uv_stride
        dst.ctypes.data_as(ctypes.c_void_p), w * 3,  # dst, dst_stride
        w, h, 1 if output_bgr else 0  # width, height, output_bgr
    )

    if ret != 0:
        raise RuntimeError(f"libyuv conversion failed with code {ret}")

    return dst


def _nv12_to_bgr_pyav(w: int, h: int, y: bytes, uv: bytes, output_bgr: bool) -> np.ndarray:
    """
    使用 PyAV (ffmpeg) 进行 NV12 -> BGR/RGB 转换，支持 BT.709。

    这是最准确的方法，使用 ffmpeg 的 sws_scale，完整支持色彩空间转换。
    """
    try:
        import av  # type: ignore
    except ImportError:
        raise ImportError(
            "PyAV is required for this method. Install with: pip install av"
        )

    # 构建 NV12 frame
    y_arr = np.frombuffer(y, dtype=np.uint8).reshape((h, w))
    uv_arr = np.frombuffer(uv, dtype=np.uint8).reshape((h // 2, w))

    # 创建 AVFrame
    frame = av.VideoFrame.from_ndarray(
        np.vstack([y_arr, uv_arr]),
        format='nv12',
        width=w,
        height=h
    )

    # 设置色彩空间为 BT.709
    frame.colorspace = av.video.reformatter.Colorspace.PRIMARY_BT709
    frame.color_range = av.video.reformatter.ColorRange.JPEG  # full range (0-255)

    # 转换为 BGR24 或 RGB24
    target_format = 'bgr24' if output_bgr else 'rgb24'

    # 使用 VideoReformatter 进行转换
    reformatter = av.video.reformatter.VideoReformatter()
    reformatted = reformatter.reformat(
        frame,
        format=target_format,
        width=w,
        height=h,
        src_colorspace=None,  # 使用 frame 的 colorspace
        dst_colorspace=None,
    )

    return reformatted.to_ndarray()


def _nv12_to_bgr_numpy(w: int, h: int, y: bytes, uv: bytes, output_bgr: bool) -> np.ndarray:
    """
    使用 numpy 手动实现 NV12 -> RGB BT.709 转换。

    BT.709 转换公式 (Full Range, 0-255):
    R = 1.164 * (Y - 16) + 1.793 * (V - 128)
    G = 1.164 * (Y - 16) - 0.213 * (U - 128) - 0.533 * (V - 128)
    B = 1.164 * (Y - 16) + 2.112 * (U - 128)

    这个实现使用了定点数运算和向量化操作来优化性能。
    """
    # 解析 Y 和 UV
    y_arr = np.frombuffer(y, dtype=np.uint8).reshape((h, w)).astype(np.int32)
    uv_arr = np.frombuffer(uv, dtype=np.uint8).reshape((h // 2, w))

    # 分离 U 和 V (NV12: interleaved UVUVUV...)
    u_arr = uv_arr[:, 0::2].astype(np.int32)  # 奇数列是 U
    v_arr = uv_arr[:, 1::2].astype(np.int32)  # 偶数列是 V

    # 上采样 U 和 V 到全分辨率 (nearest neighbor，最快)
    u_up = np.repeat(np.repeat(u_arr, 2, axis=0), 2, axis=1)
    v_up = np.repeat(np.repeat(v_arr, 2, axis=0), 2, axis=1)

    # BT.709 转换系数 (Full Range)
    # 使用定点数加速 (乘以 1024，然后右移 10 位)
    Y_SCALE = 1192   # 1.164 * 1024
    V_R = 1836       # 1.793 * 1024
    U_G = 218        # 0.213 * 1024
    V_G = 546        # 0.533 * 1024
    U_B = 2167       # 2.112 * 1024

    # 减去偏移
    y_offset = y_arr - 16
    u_offset = u_up - 128
    v_offset = v_up - 128

    # 限制 Y 范围 (避免负值)
    y_offset = np.clip(y_offset, 0, 235 - 16)

    # 计算 RGB (使用定点数)
    r = (Y_SCALE * y_offset + V_R * v_offset) >> 10
    g = (Y_SCALE * y_offset - U_G * u_offset - V_G * v_offset) >> 10
    b = (Y_SCALE * y_offset + U_B * u_offset) >> 10

    # 组合成 RGB 图像
    if output_bgr:
        rgb = np.stack([b, g, r], axis=-1, dtype=np.uint8)
    else:
        rgb = np.stack([r, g, b], axis=-1, dtype=np.uint8)

    # 限制范围
    np.clip(rgb, 0, 255, out=rgb)

    return rgb


def _nv12_to_bgr_opencv(w: int, h: int, y: bytes, uv: bytes, output_bgr: bool) -> np.ndarray:
    """
    使用 OpenCV 进行 NV12 -> BGR 转换。

    警告: OpenCV 的 cvtColor 默认使用 BT.601 色彩空间，不是 BT.709！
    这会导致白色偏粉、绿色不饱和等问题。
    仅作为对比参考使用。
    """
    try:
        import cv2  # type: ignore
    except ImportError:
        raise ImportError(
            "OpenCV is required for this method. Install with: pip install opencv-python"
        )

    y_arr = np.frombuffer(y, dtype=np.uint8).reshape((h, w))
    uv_arr = np.frombuffer(uv, dtype=np.uint8).reshape((h // 2, w))
    nv12 = np.vstack([y_arr, uv_arr])

    bgr = cv2.cvtColor(nv12, cv2.COLOR_YUV2BGR_NV12)

    if not output_bgr:
        bgr = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)

    return bgr


# ============ 高性能版本 (使用预分配缓冲区) ============

class NV12ToBGRBT709Converter:
    """
    高性能转换器，预分配缓冲区，避免重复内存分配。

    适用于实时视频流处理场景。
    """

    def __init__(self, w: int, h: int, method: str = 'libyuv', output_bgr: bool = True):
        self.w = w
        self.h = h
        self.method = method
        self.output_bgr = output_bgr

        # 预分配缓冲区
        self._y_buf = np.empty((h, w), dtype=np.uint8)
        self._uv_buf = np.empty((h // 2, w), dtype=np.uint8)
        self._dst_buf = np.empty((h, w, 3), dtype=np.uint8)
        self._u_up = np.empty((h, w), dtype=np.int32)
        self._v_up = np.empty((h, w), dtype=np.int32)

        if method == 'libyuv':
            self._lib = self._load_libyuv_wrapper()
        elif method == 'pyav':
            try:
                import av  # type: ignore
                self._av = av
            except ImportError:
                raise ImportError("PyAV is required. Install with: pip install av")

    def _load_libyuv_wrapper(self):
        import ctypes
        import os
        import sys

        # 根据平台选择库文件扩展名
        if sys.platform == 'win32':
            lib_name = 'libyuv_wrapper.dll'
            lib_paths = [
                os.path.join(os.path.dirname(__file__), lib_name),
                os.path.join(os.path.dirname(__file__), '..', lib_name),
            ]
        elif sys.platform == 'darwin':
            lib_name = 'libyuv_wrapper.dylib'
            lib_paths = [
                os.path.join(os.path.dirname(__file__), lib_name),
                '/usr/local/lib/' + lib_name,
                '/opt/homebrew/lib/' + lib_name,
            ]
        else:  # Linux
            lib_name = 'libyuv_wrapper.so'
            lib_paths = [
                os.path.join(os.path.dirname(__file__), lib_name),
                '/usr/local/lib/' + lib_name,
                '/usr/lib/x86_64-linux-gnu/' + lib_name,
                '/usr/lib/' + lib_name,
            ]

        for path in lib_paths:
            if os.path.exists(path):
                lib = ctypes.CDLL(path)
                lib.nv12_to_bgr_bt709_libyuv.argtypes = [
                    ctypes.c_void_p, ctypes.c_int,
                    ctypes.c_void_p, ctypes.c_int,
                    ctypes.c_void_p, ctypes.c_int,
                    ctypes.c_int, ctypes.c_int, ctypes.c_int
                ]
                lib.nv12_to_bgr_bt709_libyuv.restype = ctypes.c_int
                return lib

        raise ImportError(
            f"{lib_name} not found. Falling back to numpy method.\n"
            f"Linux: gcc -O3 -fPIC -shared -o {lib_name} nv12_to_bgr_libyuv.c -lyuv\n"
            f"Windows: cl /LD /O2 nv12_to_bgr_libyuv.c libyuv.lib /Fe:{lib_name}\n"
            f"macOS: gcc -O3 -fPIC -shared -o {lib_name} nv12_to_bgr_libyuv.c -lyuv"
        )

    def convert(self, y: bytes, uv: bytes) -> np.ndarray:
        """执行转换，返回 BGR/RGB 数组。"""
        if self.method == 'libyuv':
            return self._convert_libyuv(y, uv)
        elif self.method == 'pyav':
            return self._convert_pyav(y, uv)
        elif self.method == 'numpy':
            return self._convert_numpy(y, uv)
        else:
            return nv12_to_bgr_bt709(self.w, self.h, y, uv, self.method, self.output_bgr)

    def _convert_libyuv(self, y: bytes, uv: bytes) -> np.ndarray:
        import ctypes

        np.copyto(self._y_buf, np.frombuffer(y, dtype=np.uint8).reshape((self.h, self.w)))
        np.copyto(self._uv_buf, np.frombuffer(uv, dtype=np.uint8).reshape((self.h // 2, self.w)))

        ret = self._lib.nv12_to_bgr_bt709_libyuv(
            self._y_buf.ctypes.data_as(ctypes.c_void_p), self.w,
            self._uv_buf.ctypes.data_as(ctypes.c_void_p), self.w,
            self._dst_buf.ctypes.data_as(ctypes.c_void_p), self.w * 3,
            self.w, self.h, 1 if self.output_bgr else 0
        )

        if ret != 0:
            raise RuntimeError(f"libyuv conversion failed with code {ret}")

        return self._dst_buf.copy()

    def _convert_pyav(self, y: bytes, uv: bytes) -> np.ndarray:
        import av  # type: ignore

        np.copyto(self._y_buf, np.frombuffer(y, dtype=np.uint8).reshape((self.h, self.w)))
        np.copyto(self._uv_buf, np.frombuffer(uv, dtype=np.uint8).reshape((self.h // 2, self.w)))

        frame = self._av.VideoFrame.from_ndarray(
            np.vstack([self._y_buf, self._uv_buf]),
            format='nv12',
            width=self.w,
            height=self.h
        )
        frame.colorspace = self._av.video.reformatter.Colorspace.PRIMARY_BT709
        frame.color_range = self._av.video.reformatter.ColorRange.JPEG

        target_format = 'bgr24' if self.output_bgr else 'rgb24'
        reformatter = self._av.video.reformatter.VideoReformatter()
        reformatted = reformatter.reformat(frame, format=target_format)

        return reformatted.to_ndarray()

    def _convert_numpy(self, y: bytes, uv: bytes) -> np.ndarray:
        # 复制到预分配缓冲区
        np.copyto(self._y_buf, np.frombuffer(y, dtype=np.uint8).reshape((self.h, self.w)))
        uv_arr = np.frombuffer(uv, dtype=np.uint8).reshape((self.h // 2, self.w))

        y_int = self._y_buf.astype(np.int32)
        u_arr = uv_arr[:, 0::2].astype(np.int32)
        v_arr = uv_arr[:, 1::2].astype(np.int32)

        # 上采样 (np.repeat 不支持 out 参数)
        self._u_up[:] = np.repeat(np.repeat(u_arr, 2, axis=0), 2, axis=1)
        self._v_up[:] = np.repeat(np.repeat(v_arr, 2, axis=0), 2, axis=1)

        # BT.709 系数
        Y_SCALE = 1192
        V_R = 1836
        U_G = 218
        V_G = 546
        U_B = 2167

        y_offset = np.clip(y_int - 16, 0, 219)
        u_offset = self._u_up - 128
        v_offset = self._v_up - 128

        r = np.clip((Y_SCALE * y_offset + V_R * v_offset) >> 10, 0, 255).astype(np.uint8)
        g = np.clip((Y_SCALE * y_offset - U_G * u_offset - V_G * v_offset) >> 10, 0, 255).astype(np.uint8)
        b = np.clip((Y_SCALE * y_offset + U_B * u_offset) >> 10, 0, 255).astype(np.uint8)

        if self.output_bgr:
            return np.stack([b, g, r], axis=-1)
        else:
            return np.stack([r, g, b], axis=-1)


# ============ 测试和基准 ============

def benchmark(w: int = 1920, h: int = 1080, iterations: int = 100) -> dict:
    """
    基准测试不同方法的性能。

    Returns:
        dict: 各方法的平均耗时 (ms)
    """
    import time

    # 生成测试数据
    np.random.seed(42)
    y = np.random.randint(16, 235, (h, w), dtype=np.uint8).tobytes()
    uv = np.random.randint(16, 240, (h // 2, w), dtype=np.uint8).tobytes()

    results = {}

    # 测试 libyuv (最快)
    try:
        converter = NV12ToBGRBT709Converter(w, h, method='libyuv')
        converter.convert(y, uv)  # 预热

        t0 = time.perf_counter()
        for _ in range(iterations):
            converter.convert(y, uv)
        t1 = time.perf_counter()
        results['libyuv'] = (t1 - t0) * 1000 / iterations
    except (ImportError, RuntimeError):
        results['libyuv'] = None

    # 测试 PyAV
    try:
        converter = NV12ToBGRBT709Converter(w, h, method='pyav')
        converter.convert(y, uv)  # 预热

        t0 = time.perf_counter()
        for _ in range(iterations):
            converter.convert(y, uv)
        t1 = time.perf_counter()
        results['pyav'] = (t1 - t0) * 1000 / iterations
    except ImportError:
        results['pyav'] = None

    # 测试 numpy
    converter = NV12ToBGRBT709Converter(w, h, method='numpy')
    converter.convert(y, uv)  # 预热

    t0 = time.perf_counter()
    for _ in range(iterations):
        converter.convert(y, uv)
    t1 = time.perf_counter()
    results['numpy'] = (t1 - t0) * 1000 / iterations

    # 测试 OpenCV (对比，注意默认是 BT.601)
    try:
        import cv2  # type: ignore
        converter = NV12ToBGRBT709Converter(w, h, method='opencv')
        converter.convert(y, uv)  # 预热

        t0 = time.perf_counter()
        for _ in range(iterations):
            converter.convert(y, uv)
        t1 = time.perf_counter()
        results['opencv'] = (t1 - t0) * 1000 / iterations
    except ImportError:
        results['opencv'] = None

    return results


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="NV12 to BGR BT.709 conversion")
    ap.add_argument("--benchmark", action="store_true", help="Run benchmark")
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--iterations", type=int, default=100)
    args = ap.parse_args()

    if args.benchmark:
        print(f"Benchmarking {args.width}x{args.height} conversion...")
        results = benchmark(args.width, args.height, args.iterations)

        # 按性能排序输出
        sorted_methods = sorted(
            [(k, v) for k, v in results.items() if v is not None],
            key=lambda x: x[1]
        )
        print("\nResults (sorted by speed):")
        for method, ms in sorted_methods:
            fps = 1000.0 / ms if ms > 0 else 0
            print(f"  {method:10s}: {ms:6.2f} ms/frame ({fps:6.1f} fps)")

        # 显示不可用的方法
        unavailable = [k for k, v in results.items() if v is None]
        if unavailable:
            print(f"\nNot available: {', '.join(unavailable)}")

        # 性能对比
        if sorted_methods:
            fastest = sorted_methods[0][0]
            print(f"\nFastest: {fastest}")
            for method, ms in sorted_methods[1:]:
                ratio = ms / sorted_methods[0][1]
                print(f"  {method} is {ratio:.1f}x slower than {fastest}")
    else:
        print("NV12 to BGR BT.709 Conversion")
        print("============================")
        print("")
        print("Usage:")
        print("  python nv12_to_bgr_bt709.py --benchmark")
        print("  python nv12_to_bgr_bt709.py --benchmark --width 1920 --height 1080")
        print("")
        print("Methods (sorted by speed):")
        print("  1. libyuv  - SIMD optimized (SSSE3/AVX2/NEON), fastest")
        print("  2. pyav    - ffmpeg sws_scale, accurate")
        print("  3. numpy   - pure Python, no extra deps")
        print("  4. opencv  - BT.601 only (for comparison)")
        print("")
        print("To use libyuv:")
        print("  sudo apt install libyuv-dev")
        print("  cd python_dir && gcc -O3 -fPIC -shared -o libyuv_wrapper.so nv12_to_bgr_libyuv.c -lyuv")
        print("")
        print("Example:")
        print("  from nv12_to_bgr_bt709 import nv12_to_bgr_bt709")
        print("  bgr = nv12_to_bgr_bt709(w, h, y, uv, method='libyuv')")
