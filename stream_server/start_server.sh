#!/bin/bash
# Stream Server 启动脚本
# 用于在 192.168.11.31 上启动多路视频流接收服务器

set -e

# 配置
PORT=19000
MAX_STREAMS=20
STATS_INTERVAL=5
BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log() {
    echo -e "${GREEN}[$(date '+%Y-%m-%d %H:%M:%S')]${NC} $*"
}

warn() {
    echo -e "${YELLOW}[$(date '+%Y-%m-%d %H:%M:%S')] WARNING:${NC} $*"
}

error() {
    echo -e "${RED}[$(date '+%Y-%m-%d %H:%M:%S')] ERROR:${NC} $*"
}

# 检查可执行文件
if [[ ! -f "$BUILD_DIR/stream_server" ]]; then
    error "stream_server 未找到，请先构建:"
    error "  cd $BUILD_DIR && make -j4"
    exit 1
fi

# 检查端口是否被占用
if ss -tuln | grep -q ":$PORT "; then
    warn "端口 $PORT 已被占用"
    read -p "是否强制关闭占用进程? [y/N]: " ans
    if [[ "$ans" == "y" || "$ans" == "Y" ]]; then
        sudo fuser -k ${PORT}/tcp 2>/dev/null || true
        sleep 1
    else
        exit 1
    fi
fi

# 检查 GPU 环境
log "检查 GPU 环境..."

# NVIDIA
if command -v nvidia-smi &> /dev/null; then
    log "NVIDIA GPU  detected:"
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || warn "无法获取 NVIDIA GPU 信息"
else
    warn "nvidia-smi 未找到，NVIDIA 解码不可用"
fi

# Intel
if [[ -e /dev/dri/renderD128 ]]; then
    log "Intel GPU 设备 found: /dev/dri/renderD128"
    if [[ -n "${LIBVA_DRIVER_NAME:-}" ]]; then
        log "VA-API 驱动: $LIBVA_DRIVER_NAME"
    fi
else
    warn "Intel GPU 设备未找到"
fi

# 启动服务器
log "启动 Stream Server..."
log "  端口: $PORT"
log "  最大流数: $MAX_STREAMS"
log "  统计间隔: ${STATS_INTERVAL}s"
log ""
log "等待来自 192.168.11.50 的连接..."
log "按 Ctrl+C 停止服务器"
log ""

cd "$BUILD_DIR"
exec ./stream_server -p $PORT -c $MAX_STREAMS -s $STATS_INTERVAL -v
