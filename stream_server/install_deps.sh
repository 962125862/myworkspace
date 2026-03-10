#!/bin/bash
# 安装 stream_server 依赖

echo "=== Stream Server Dependencies Installation ==="
echo ""

# 检测系统
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
else
    echo "Cannot detect OS"
    exit 1
fi

echo "Detected OS: $OS"
echo ""

# 基础编译工具
echo "[1/5] Installing build tools..."
if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    sudo apt update
    sudo apt install -y \
        build-essential \
        cmake \
        pkg-config \
        git
elif [ "$OS" = "centos" ] || [ "$OS" = "rhel" ] || [ "$OS" = "fedora" ]; then
    sudo yum groupinstall -y "Development Tools"
    sudo yum install -y cmake pkgconfig
fi

# FFmpeg 开发库
echo ""
echo "[2/5] Installing FFmpeg libraries..."
if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    sudo apt install -y \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libswscale-dev \
        libavdevice-dev
fi

# Intel VA-API (可选)
echo ""
echo "[3/5] Installing Intel VA-API support (optional)..."
if [ "$OS" = "ubuntu" ] || [ "$OS" = "debian" ]; then
    sudo apt install -y \
        vainfo \
        libva-dev \
        libva-drm2 \
        libva-x11-2 \
        i965-va-driver \
        intel-media-va-driver
fi

# NVIDIA CUDA (可选，需要 NVIDIA GPU)
echo ""
echo "[4/5] NVIDIA CUDA support (optional)..."
echo "    For NVIDIA GPU, install:"
echo "    - nvidia-driver (latest)"
echo "    - cuda-toolkit"
echo "    - libnvidia-encode/decode"
echo ""
echo "    Or use official NVIDIA repo:"
echo "    https://developer.nvidia.com/cuda-downloads"

# 验证安装
echo ""
echo "[5/5] Verifying installation..."
echo ""
echo "FFmpeg version:"
ffmpeg -version 2>/dev/null | head -1 || echo "  ffmpeg not found"

echo ""
echo "VA-API info:"
vainfo 2>/dev/null | head -10 || echo "  vainfo not installed or no Intel GPU"

echo ""
echo "NVIDIA GPU:"
nvidia-smi 2>/dev/null | head -10 || echo "  No NVIDIA GPU or driver not installed"

echo ""
echo "=== Installation Complete ==="
echo ""
echo "To build stream_server:"
echo "  cd stream_server/build"
echo "  cmake .."
echo "  make -j4"
echo ""
echo "To run:"
echo "  ./build/stream_server -p 19000 -c 20"
