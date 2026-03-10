#!/bin/bash
# 测试接收端和推流端

echo "=== Stream Server Test ==="

# 在 31 机器上启动接收端
echo "[1] Starting receiver on 192.168.11.31:9000..."
ssh gejun@192.168.11.31 "cd ~/stream_test/stream_server && timeout 20 ./build/stream_server -p 9000 -c 20 -s 2" &
RECV_PID=$!

# 等待接收端启动
sleep 3

# 检查端口
echo "[2] Checking port..."
ssh gejun@192.168.11.31 "ss -tlnp | grep 9000"

# 启动推流
echo "[3] Starting streamer..."
cd /home/gejun/work/my_ml_work
timeout 12 ./build/ml_worker stream --host 192.168.11.50 --app Desktop --tcp-host 192.168.11.31 --tcp-port 9000 --stream-id 1 2>&1 &
STREAM_PID=$!

# 等待推流结束
wait $STREAM_PID

# 等待接收端输出统计
sleep 3

# 清理
echo "[4] Cleaning up..."
kill $RECV_PID 2>/dev/null
ssh gejun@192.168.11.31 "pkill -f stream_server" 2>/dev/null

echo "=== Test Complete ==="
