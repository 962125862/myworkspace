#!/usr/bin/env bash
# test_both_backends.sh
# 在 192.168.11.50 (本机) 上执行
# 自动测试 VA-API 和 NVIDIA 两个后端的单路 60fps 解码
#
# 用法:
#   cd ~/work/my_ml_work/stream_server
#   ./test_both_backends.sh
#   ./test_both_backends.sh vaapi      # 只测 VA-API
#   ./test_both_backends.sh nvidia     # 只测 NVIDIA
#   ./test_both_backends.sh 30         # 自定义测试时长(秒)

set -uo pipefail

REMOTE="192.168.11.31"
REMOTE_DIR="~/work/my_ml_work/stream_server"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPLOY_DIR="$(cd "$SCRIPT_DIR/../deploy" 2>/dev/null && pwd)"
PORT=19000
TEST_DURATION=15

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }
info() { echo -e "${BLUE}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

# ==================== 前置检查 ====================

check_prereqs() {
    info "前置检查..."

    # SSH 连通性
    if ! ssh -o ConnectTimeout=3 "$REMOTE" 'echo ok' >/dev/null 2>&1; then
        fail "无法 SSH 连接 $REMOTE"
        echo "  请确认: ssh $REMOTE 'echo ok'"
        exit 1
    fi
    pass "SSH 连接 $REMOTE 正常"

    # 远程 stream_server 可执行文件
    if ! ssh -n "$REMOTE" "test -x $REMOTE_DIR/build/stream_server" 2>/dev/null; then
        fail "远程 stream_server 不存在"
        echo "  请先同步并编译:"
        echo "  rsync -avz --exclude='build/' stream_server/ $REMOTE:$REMOTE_DIR/"
        echo "  ssh $REMOTE 'cd $REMOTE_DIR/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc)'"
        exit 1
    fi
    pass "远程 stream_server 已编译"

    # 本地 mlctl.sh
    if [[ ! -x "$DEPLOY_DIR/mlctl.sh" ]]; then
        fail "找不到 mlctl.sh: $DEPLOY_DIR/mlctl.sh"
        exit 1
    fi
    pass "本地 mlctl.sh 存在"

    # Docker
    if ! docker info >/dev/null 2>&1; then
        fail "Docker 不可用"
        exit 1
    fi
    pass "Docker 可用"

    # ml-worker 镜像
    if ! docker image inspect ml-worker:latest >/dev/null 2>&1; then
        warn "ml-worker:latest 镜像不存在，推流可能失败"
    else
        pass "ml-worker 镜像存在"
    fi

    echo ""
}

# ==================== 清理 ====================

cleanup() {
    info "清理资源..."
    "$DEPLOY_DIR/mlctl.sh" down worker00 2>/dev/null || true
    ssh -n "$REMOTE" 'pkill -f "build/stream_server" 2>/dev/null; true' 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT INT TERM

# ==================== 远程服务器控制 ====================

start_remote_server() {
    local backend="$1"
    local log_file="/tmp/${backend}_test.log"

    # 先杀旧进程 + 等端口释放
    ssh -n "$REMOTE" 'pkill -f "build/stream_server" 2>/dev/null; true'
    sleep 2

    # 写启动脚本到远程，避免 SSH 后台进程挂起
    ssh -n "$REMOTE" "cat > /tmp/start_stream_server.sh << 'REMOTEOF'
#!/bin/bash
cd $REMOTE_DIR
export DECODE_BACKEND=$backend
rm -f $log_file
exec stdbuf -oL ./build/stream_server -p $PORT -c 20 -s 5 > $log_file 2>&1
REMOTEOF
chmod +x /tmp/start_stream_server.sh"

    # 通过 nohup + 完全脱离终端启动
    ssh -n -f "$REMOTE" "nohup /tmp/start_stream_server.sh < /dev/null > /dev/null 2>&1 &"
    sleep 3

    # 验证
    if ! ssh -n "$REMOTE" "ss -tuln | grep -q :$PORT"; then
        fail "服务器启动失败"
        echo "  远程日志:"
        ssh -n "$REMOTE" "cat $log_file" 2>/dev/null || true
        return 1
    fi

    return 0
}

stop_remote_server() {
    ssh -n "$REMOTE" 'pkill -f "build/stream_server" 2>/dev/null; true'
    sleep 2
}

# ==================== Worker 控制 ====================

start_worker() {
    "$DEPLOY_DIR/mlctl.sh" up worker00 2>/dev/null
    sleep 3

    # 确认 worker 在运行
    if ! docker ps --format '{{.Names}}' | grep -qx 'mlw-worker00'; then
        fail "Worker 容器未运行"
        docker logs mlw-worker00 2>&1 | tail -10
        return 1
    fi

    # 确认 TCP 连接成功
    local logs
    logs=$(docker logs mlw-worker00 2>&1)
    if echo "$logs" | grep -q "state=connected"; then
        return 0
    fi

    # 再等几秒
    sleep 3
    return 0
}

stop_worker() {
    "$DEPLOY_DIR/mlctl.sh" down worker00 2>/dev/null || true
    sleep 1
}

# ==================== 核心测试 ====================

test_backend() {
    local backend="$1"
    local log_file="/tmp/${backend}_test.log"

    echo ""
    echo "============================================"
    echo "  Testing: $backend (${TEST_DURATION}s)"
    echo "============================================"

    # 1. 启动服务器
    info "[1/5] 启动远程服务器 ($backend)..."
    if ! start_remote_server "$backend"; then
        return 1
    fi
    pass "服务器启动成功"

    # 2. 启动推流
    info "[2/5] 启动 Docker Worker 推流..."
    if ! start_worker; then
        stop_remote_server
        return 1
    fi
    pass "Worker 推流已启动"

    # 3. 等待解码稳定 + 采集
    info "[3/5] 等待 ${TEST_DURATION}s 采集统计..."
    sleep "$TEST_DURATION"

    # 4. 采集结果
    info "[4/5] 采集结果..."

    local server_log
    server_log=$(ssh -n "$REMOTE" "tail -30 $log_file" 2>/dev/null)

    local worker_log
    worker_log=$(docker logs mlw-worker00 2>&1 | tail -5)

    # 提取最后一次统计
    local fps frames decoded
    fps=$(echo "$server_log" | grep -oP 'FPS: \K[0-9.]+' | tail -1)
    frames=$(echo "$server_log" | grep -oP 'Frames: \K[0-9]+' | tail -1)
    decoded=$(echo "$server_log" | grep -oP 'Decoded: \K[0-9]+' | tail -1)

    local worker_fps
    worker_fps=$(echo "$worker_log" | grep -oP 'fps=\K[0-9.]+' | tail -1)

    echo ""
    echo "  ┌─────────────────────────────────────┐"
    echo "  │ 后端:       $backend"
    echo "  │ 解码 FPS:   ${fps:-N/A}"
    echo "  │ 接收帧数:   ${frames:-N/A}"
    echo "  │ 解码帧数:   ${decoded:-N/A}"
    echo "  │ 推流 FPS:   ${worker_fps:-N/A}"
    echo "  └─────────────────────────────────────┘"

    # 5. 清理
    info "[5/5] 清理..."
    stop_worker
    stop_remote_server

    # 判定
    if [[ -n "$fps" ]] && (( $(echo "$fps >= 55.0" | bc -l 2>/dev/null || echo 0) )); then
        pass "$backend: ${fps} fps (Frames=$frames, Decoded=$decoded)"
        return 0
    else
        # bc 不可用时 简单判断
        if [[ -n "$fps" && "${fps%%.*}" -ge 55 ]] 2>/dev/null; then
            pass "$backend: ${fps} fps (Frames=$frames, Decoded=$decoded)"
            return 0
        fi
        fail "$backend: ${fps:-0} fps (预期 >= 55)"
        echo "  服务器最后日志:"
        echo "$server_log" | tail -10 | sed 's/^/    /'
        return 1
    fi
}

# ==================== 主流程 ====================

main() {
    local backends=("vaapi" "nvidia")

    # 解析参数
    for arg in "$@"; do
        case "$arg" in
            vaapi|intel)
                backends=("vaapi")
                ;;
            nvidia|cuda)
                backends=("nvidia")
                ;;
            [0-9]*)
                TEST_DURATION="$arg"
                ;;
        esac
    done

    echo "=========================================="
    echo "  Stream Server - Backend Test"
    echo "=========================================="
    echo "  远程主机:   $REMOTE"
    echo "  测试时长:   ${TEST_DURATION}s / 后端"
    echo "  测试后端:   ${backends[*]}"
    echo "=========================================="
    echo ""

    check_prereqs

    declare -A results
    for backend in "${backends[@]}"; do
        if test_backend "$backend"; then
            results[$backend]="PASS"
        else
            results[$backend]="FAIL"
        fi
    done

    echo ""
    echo "=========================================="
    echo "  测试结果"
    echo "=========================================="
    for backend in "${backends[@]}"; do
        if [[ "${results[$backend]}" == "PASS" ]]; then
            pass "$backend: 60fps OK"
        else
            fail "$backend: FAILED"
        fi
    done
    echo "=========================================="

    # 返回码: 全部通过返回 0
    for backend in "${backends[@]}"; do
        [[ "${results[$backend]}" == "PASS" ]] || exit 1
    done
    exit 0
}

main "$@"
