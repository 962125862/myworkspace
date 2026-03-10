#!/bin/bash
# NVIDIA NVDEC 解码环境安装脚本
# 适用于 Ubuntu 22.04/24.04

set -e

echo "========================================"
echo "  NVIDIA NVDEC 解码环境安装"
echo "========================================"
echo ""

# 检测系统
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    VER=$VERSION_ID
else
    echo "Cannot detect OS"
    exit 1
fi

echo "检测到系统: $OS $VER"
echo ""

# 1. 基础编译工具
echo "[1/6] 安装基础编译工具..."
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    wget

# 2. FFmpeg 开发库 (必须支持 CUDA hwcontext)
echo ""
echo "[2/6] 安装 FFmpeg 开发库..."
sudo apt install -y \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libavdevice-dev \
    ffmpeg

# 验证 FFmpeg 是否支持 CUDA
echo ""
echo "验证 FFmpeg CUDA 支持..."
if ffmpeg -hwaccels 2>/dev/null | grep -q cuda; then
    echo "  ✓ FFmpeg 支持 CUDA hwaccel"
else
    echo "  ⚠ FFmpeg 可能不支持 CUDA hwaccel"
    echo "    如果解码失败，可能需要从源码编译 FFmpeg"
fi

# 3. NVIDIA 驱动
echo ""
echo "[3/6] 检查 NVIDIA 驱动..."
if command -v nvidia-smi &> /dev/null; then
    NVIDIA_VERSION=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
    echo "  ✓ NVIDIA 驱动已安装: $NVIDIA_VERSION"
    nvidia-smi
else
    echo "  ⚠ NVIDIA 驱动未安装"
    echo ""
    echo "  安装 NVIDIA 驱动："
    echo "    sudo apt install -y nvidia-driver-535  # 或最新版本"
    echo "    sudo reboot"
    echo ""
    echo "  或使用官方安装程序："
    echo "    https://www.nvidia.com/Download/index.aspx"
fi

# 4. CUDA Toolkit (可选，FFmpeg 会动态加载)
echo ""
echo "[4/6] 检查 CUDA Toolkit..."
if command -v nvcc &> /dev/null; then
    CUDA_VERSION=$(nvcc --version | grep "release" | awk '{print $6}' | cut -c2-)
    echo "  ✓ CUDA Toolkit 已安装: $CUDA_VERSION"
else
    echo "  ⚠ CUDA Toolkit 未安装 (可选)"
    echo ""
    echo "  安装 CUDA Toolkit："
    echo "    wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb"
    echo "    sudo dpkg -i cuda-keyring_1.1-1_all.deb"
    echo "    sudo apt update"
    echo "    sudo apt install -y cuda-toolkit-12-3"
fi

# 5. FFmpeg NVDEC 头文件 (如果系统 FFmpeg 不支持)
echo ""
echo "[5/6] 检查 FFmpeg NVDEC 支持..."
if [ -f /usr/include/libavutil/hwcontext_cuda.h ]; then
    echo "  ✓ hwcontext_cuda.h 已存在"
else
    echo "  ⚠ hwcontext_cuda.h 不存在"
    echo "    FFmpeg 开发包可能不完整"
fi

# 6. 编译项目
echo ""
echo "[6/6] 编译项目..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake .. 2>&1 | tee cmake.log

if grep -q "HAVE_CUDA" cmake.log; then
    echo ""
    echo "  ✓ CUDA 支持已启用"
else
    echo ""
    echo "  ⚠ CUDA 支持未启用，请检查 FFmpeg 版本"
fi

make -j$(nproc)

echo ""
echo "========================================"
echo "  安装完成"
echo "========================================"
echo ""
echo "运行测试："
echo "  cd $BUILD_DIR"
echo "  ./test_20streams_hw /path/to/video.h264"
echo ""
echo "或者只测试 CUDA 解码："
echo "  ./test_20streams_simple /path/to/video.h264"
