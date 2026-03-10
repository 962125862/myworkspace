#!/usr/bin/env bash
#
# 20路并发视频流压力测试脚本
# 部署在 192.168.11.31 上执行
#

set -euo pipefail

# 配置
SERVER_HOST="192.168.11.31"
SERVER_PORT="19000"
TEST_DURATION="35"  # 测试持续时间（秒），包含启动缓冲
NUM_STREAMS="20"    # 并发流数量
BUILD_DIR="build"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log() {
    echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"
}

log_info() {
    echo -e "${GREEN}[INFO]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*"
}

# 检查依赖
check_dependencies() {
    log "检查依赖..."
    
    # 检查 nvidia-smi
    if ! command -v nvidia-smi &> /dev/null; then
        log_warn "nvidia-smi 未找到，GPU 监控可能不可用"
    else
        log_info "nvidia-smi 已安装"
    fi
    
    # 检查 FFmpeg
    if ! command -v ffmpeg &> /dev/null; then
        log_error "FFmpeg 未安装"
        exit 1
    fi
    
    log_info "依赖检查完成"
}

# 构建 stream_server
build_server() {
    log "构建 stream_server..."
    
    cd "$(dirname "$0")"
    
    if [[ ! -d "$BUILD_DIR" ]]; then
        mkdir -p "$BUILD_DIR"
    fi
    
    cd "$BUILD_DIR"
    
    # 运行 CMake
    if [[ ! -f "Makefile" ]]; then
        cmake .. -DCMAKE_BUILD_TYPE=Release
    fi
    
    # 编译
    make -j$(nproc)
    
    if [[ ! -f "stream_server" ]]; then
        log_error "编译失败，stream_server 未生成"
        exit 1
    fi
    
    log_info "stream_server 构建成功"
    cd ..
}

# 监控 GPU 状态 (后台进程)
monitor_gpu() {
    local output_file="$1"
    local duration="$2"
    
    log "启动 GPU 监控 (持续 ${duration} 秒)..."
    
    # 创建监控脚本
    cat > /tmp/gpu_monitor.sh << 'EOF'
#!/bin/bash
output_file="$1"
duration="$2"
start_time=$(date +%s)

# 写入表头
echo "timestamp,gpu_utilization,memory_utilization,memory_used,memory_total,temperature" > "$output_file"

while true; do
    current_time=$(date +%s)
    elapsed=$((current_time - start_time))
    
    if [[ $elapsed -ge $duration ]]; then
        break
    fi
    
    # 获取 GPU 状态
    if command -v nvidia-smi &> /dev/null; then
        stats=$(nvidia-smi --query-gpu=timestamp,utilization.gpu,utilization.memory,memory.used,memory.total,temperature.gpu --format=csv,noheader,nounits 2>/dev/null | head -1)
        if [[ -n "$stats" ]]; then
            echo "$stats" >> "$output_file"
        fi
    fi
    
    sleep 1
done
EOF
    chmod +x /tmp/gpu_monitor.sh
    
    # 启动后台监控
    /tmp/gpu_monitor.sh "$output_file" "$duration" &
    GPU_MONITOR_PID=$!
    
    log_info "GPU 监控已启动 (PID: $GPU_MONITOR_PID)"
}

# 启动 stream_server
start_server() {
    log "启动 stream_server (端口 $SERVER_PORT)..."
    
    # 检查端口是否被占用
    if lsof -Pi :$SERVER_PORT -sTCP:LISTEN -t >/dev/null 2>&1; then
        log_warn "端口 $SERVER_PORT 已被占用，尝试释放..."
        lsof -Pi :$SERVER_PORT -sTCP:LISTEN -t | xargs kill -9 2>/dev/null || true
        sleep 1
    fi
    
    # 启动服务器（压力测试模式）
    export STRESS_TEST=1
    export STRESS_COPIES=$NUM_STREAMS
    
    ./$BUILD_DIR/stream_server \
        --host 0.0.0.0 \
        --port $SERVER_PORT \
        --connections $NUM_STREAMS \
        --stats-interval 5 \
        > /tmp/stream_server.log 2>&1 &
    
    SERVER_PID=$!
    
    # 等待服务器启动
    sleep 2
    
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        log_error "stream_server 启动失败"
        cat /tmp/stream_server.log
        exit 1
    fi
    
    log_info "stream_server 已启动 (PID: $SERVER_PID)"
    log_info "日志文件: /tmp/stream_server.log"
}

# 停止 stream_server
stop_server() {
    log "停止 stream_server..."
    
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 $SERVER_PID 2>/dev/null; then
        kill -TERM $SERVER_PID 2>/dev/null || true
        sleep 2
        
        # 强制终止
        if kill -0 $SERVER_PID 2>/dev/null; then
            kill -9 $SERVER_PID 2>/dev/null || true
        fi
    fi
    
    # 清理残留进程
    pkill -f "stream_server" 2>/dev/null || true
    
    log_info "stream_server 已停止"
}

# 生成性能报告
generate_report() {
    log "生成性能报告..."
    
    local report_file="stress_test_report_$(date +%Y%m%d_%H%M%S).txt"
    local gpu_log="/tmp/gpu_stats.csv"
    local server_log="/tmp/stream_server.log"
    
    # 从服务器日志提取报告
    if [[ -f "$server_log" ]]; then
        # 提取最后的报告部分
        tail -100 "$server_log" | grep -A 50 "压力测试报告" > "/tmp/report_section.txt" || true
    fi
    
    # 分析 GPU 数据
    local avg_gpu_util=0
    local max_gpu_util=0
    local avg_mem_util=0
    local max_mem_util=0
    local max_temp=0
    
    if [[ -f "$gpu_log" ]] && [[ $(wc -l < "$gpu_log") -gt 1 ]]; then
        # 计算平均值和最大值
        avg_gpu_util=$(tail -n +2 "$gpu_log" | awk -F',' '{sum+=$2; count++} END {if(count>0) printf "%.1f", sum/count}')
        max_gpu_util=$(tail -n +2 "$gpu_log" | awk -F',' 'BEGIN{max=0} {if($2>max) max=$2} END {print max}')
        avg_mem_util=$(tail -n +2 "$gpu_log" | awk -F',' '{sum+=$3; count++} END {if(count>0) printf "%.1f", sum/count}')
        max_mem_util=$(tail -n +2 "$gpu_log" | awk -F',' 'BEGIN{max=0} {if($3>max) max=$3} END {print max}')
        max_temp=$(tail -n +2 "$gpu_log" | awk -F',' 'BEGIN{max=0} {if($6>max) max=$6} END {print max}')
    fi
    
    # 生成报告
    cat > "$report_file" << EOF
================================================================================
              STREAM SERVER - 20路并发压力测试报告
================================================================================
测试时间: $(date '+%Y-%m-%d %H:%M:%S')
测试主机: $SERVER_HOST
测试端口: $SERVER_PORT
并发流数: $NUM_STREAMS
测试时长: ${TEST_DURATION} 秒

================================================================================
                           GPU 性能统计
================================================================================
平均 GPU 利用率: ${avg_gpu_util}%
峰值 GPU 利用率: ${max_gpu_util}%
平均显存利用率: ${avg_mem_util}%
峰值显存利用率: ${max_mem_util}%
峰值温度: ${max_temp}°C

GPU 详细数据: $gpu_log

================================================================================
                           服务器日志
================================================================================
EOF

    if [[ -f "$server_log" ]]; then
        echo "" >> "$report_file"
        tail -200 "$server_log" >> "$report_file"
    fi
    
    cat >> "$report_file" << EOF

================================================================================
                           测试完成
================================================================================
EOF

    log_info "性能报告已保存: $report_file"
    
    # 显示报告
    echo ""
    cat "$report_file"
}

# 清理函数
cleanup() {
    log "清理资源..."
    
    # 停止 GPU 监控
    if [[ -n "${GPU_MONITOR_PID:-}" ]] && kill -0 $GPU_MONITOR_PID 2>/dev/null; then
        kill $GPU_MONITOR_PID 2>/dev/null || true
    fi
    
    # 停止服务器
    stop_server
    
    # 清理临时文件
    rm -f /tmp/gpu_monitor.sh
}

# 主函数
main() {
    log "=========================================="
    log "  20路并发视频流压力测试"
    log "=========================================="
    log "目标服务器: $SERVER_HOST:$SERVER_PORT"
    log "并发流数: $NUM_STREAMS"
    log "测试时长: ${TEST_DURATION} 秒"
    log "=========================================="
    
    # 设置清理钩子
    trap cleanup EXIT INT TERM
    
    # 执行测试步骤
    check_dependencies
    build_server
    
    # 启动 GPU 监控
    monitor_gpu "/tmp/gpu_stats.csv" "$TEST_DURATION"
    
    # 启动服务器
    start_server
    
    log "=========================================="
    log "  等待推流连接..."
    log "  请在本机执行: ./deploy/mlctl.sh up worker00"
    log "=========================================="
    
    # 等待测试完成
    log "测试进行中，等待 ${TEST_DURATION} 秒..."
    sleep $TEST_DURATION
    
    log "测试时间到，正在生成报告..."
    
    # 生成报告
    generate_report
    
    log "=========================================="
    log "  压力测试完成"
    log "=========================================="
}

# 运行主函数
main "$@"
