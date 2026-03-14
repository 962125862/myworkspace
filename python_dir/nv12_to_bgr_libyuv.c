/**
 * @file nv12_to_bgr_libyuv.c
 * @brief NV12 -> BGR/RGB 转换，使用 libyuv 和 BT.709 色彩空间
 *
 * 编译方法:
 *   # Ubuntu/Debian
 *   sudo apt install libyuv-dev
 *
 *   gcc -O3 -fPIC -shared -o libyuv_wrapper.so nv12_to_bgr_libyuv.c -lyuv
 *
 * 或者从源码编译 libyuv:
 *   git clone https://chromium.googlesource.com/libyuv/libyuv
 *   cd libyuv
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
 *   make -j && sudo make install
 *
 * 使用方法 (Python):
 *   import ctypes
 *   lib = ctypes.CDLL('./libyuv_wrapper.so')
 *   lib.nv12_to_bgr_bt709_libyuv(y_ptr, y_stride, uv_ptr, uv_stride,
 *                                 dst_ptr, dst_stride, w, h, output_bgr)
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* libyuv 头文件 */
#include <libyuv.h>

/**
 * @brief NV12 -> BGR24/RGB24 转换，使用 BT.709 full range 色彩空间
 *
 * @param src_y         Y 平面指针
 * @param src_stride_y  Y 平面 stride (通常等于 width)
 * @param src_uv        UV 平面指针 (interleaved UVUV...)
 * @param src_stride_uv UV 平面 stride (通常等于 width)
 * @param dst_bgr       输出缓冲区 (BGR24 或 RGB24)
 * @param dst_stride    输出 stride (width * 3)
 * @param width         图像宽度
 * @param height        图像高度
 * @param output_bgr    1: 输出 BGR (OpenCV 格式), 0: 输出 RGB
 * @return int          0 成功, -1 失败
 *
 * 使用 kYuvF709Constants (BT.709 full range, 0-255)
 * 这能正确还原白色、绿色、黄色
 */
int nv12_to_bgr_bt709_libyuv(
    const uint8_t* src_y, int src_stride_y,
    const uint8_t* src_uv, int src_stride_uv,
    uint8_t* dst_bgr, int dst_stride,
    int width, int height,
    int output_bgr
) {
    if (!src_y || !src_uv || !dst_bgr || width <= 0 || height <= 0) {
        return -1;
    }

    /* NV12 是 Y 平面 + interleaved UV 平面
     * libyuv 的 NV12ToARGBMatrix 需要:
     *   - src_y, src_stride_y
     *   - src_uv (interleaved), src_stride_uv
     *   - dst_argb, dst_stride_argb
     *   - yuvconstants (色彩空间矩阵)
     *
     * 对于 BGR 输出，使用 kYvuF709Constants (注意是 Yvu 不是 Yuv)
     * 这会输出 BGR 顺序而不是 RGB
     */

    if (output_bgr) {
        /* 输出 BGR (OpenCV 格式)
         * 使用 NV21ToRGB24Matrix 配合 kYvuF709Constants
         * NV12ToRAWMatrix 是宏: NV21ToRGB24Matrix(a,b,c,d,e,f,g##VU,h,i)
         * g##VU = kYvuF709Constants
         */
        return NV21ToRGB24Matrix(
            src_y, src_stride_y,
            src_uv, src_stride_uv,
            dst_bgr, dst_stride,
            &kYvuF709Constants,  /* BT.709 full range, 输出 BGR */
            width, height
        );
    } else {
        /* 输出 RGB
         * 使用 NV12ToRGB24Matrix 配合 kYuvF709Constants
         */
        return NV12ToRGB24Matrix(
            src_y, src_stride_y,
            src_uv, src_stride_uv,
            dst_bgr, dst_stride,
            &kYuvF709Constants,  /* BT.709 full range, 输出 RGB */
            width, height
        );
    }
}

/**
 * @brief NV12 -> ARGB/ABGR 转换，使用 BT.709 full range
 *
 * @param src_y         Y 平面指针
 * @param src_stride_y  Y 平面 stride
 * @param src_uv        UV 平面指针
 * @param src_stride_uv UV 平面 stride
 * @param dst_argb      输出缓冲区 (ARGB 或 ABGR, 每像素 4 字节)
 * @param dst_stride    输出 stride (width * 4)
 * @param width         图像宽度
 * @param height        图像高度
 * @param output_abgr   1: 输出 ABGR, 0: 输出 ARGB
 * @return int          0 成功, -1 失败
 */
int nv12_to_argb_bt709_libyuv(
    const uint8_t* src_y, int src_stride_y,
    const uint8_t* src_uv, int src_stride_uv,
    uint8_t* dst_argb, int dst_stride,
    int width, int height,
    int output_abgr
) {
    if (!src_y || !src_uv || !dst_argb || width <= 0 || height <= 0) {
        return -1;
    }

    if (output_abgr) {
        /* 输出 ABGR (RGBA 字节序: B,G,R,A) */
        return NV21ToARGBMatrix(
            src_y, src_stride_y,
            src_uv, src_stride_uv,
            dst_argb, dst_stride,
            &kYvuF709Constants,
            width, height
        );
    } else {
        /* 输出 ARGB (RGBA 字节序: B,G,R,A) */
        return NV12ToARGBMatrix(
            src_y, src_stride_y,
            src_uv, src_stride_uv,
            dst_argb, dst_stride,
            &kYuvF709Constants,
            width, height
        );
    }
}

/**
 * @brief NV12 -> I420 转换 (无色彩空间转换，只是格式转换)
 *
 * 有时候需要先转成 I420 再用其他工具处理
 */
int nv12_to_i420_libyuv(
    const uint8_t* src_y, int src_stride_y,
    const uint8_t* src_uv, int src_stride_uv,
    uint8_t* dst_y, int dst_stride_y,
    uint8_t* dst_u, int dst_stride_u,
    uint8_t* dst_v, int dst_stride_v,
    int width, int height
) {
    return NV12ToI420(
        src_y, src_stride_y,
        src_uv, src_stride_uv,
        dst_y, dst_stride_y,
        dst_u, dst_stride_u,
        dst_v, dst_stride_v,
        width, height
    );
}

/* ============ 性能测试辅助函数 ============ */

/**
 * @brief 批量转换多帧，用于性能测试
 */
int nv12_to_bgr_batch_libyuv(
    const uint8_t* src_y, int src_stride_y,
    const uint8_t* src_uv, int src_stride_uv,
    uint8_t* dst_bgr, int dst_stride,
    int width, int height,
    int output_bgr,
    int num_frames
) {
    for (int i = 0; i < num_frames; i++) {
        int ret = nv12_to_bgr_bt709_libyuv(
            src_y + i * src_stride_y * height,
            src_stride_y,
            src_uv + i * src_stride_uv * height / 2,
            src_stride_uv,
            dst_bgr + i * dst_stride * height,
            dst_stride,
            width, height, output_bgr
        );
        if (ret != 0) return ret;
    }
    return 0;
}
