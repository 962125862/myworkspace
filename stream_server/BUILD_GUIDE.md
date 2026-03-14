# Stream Server 编译指南

## 快速编译

```bash
# 清理旧构建（遇到缓存问题时执行）
rm -rf build CMakeCache.txt CMakeFiles cmake_install.cmake CTestTestfile.cmake Makefile

# 配置项目
cmake -S . -B build

# 编译
cmake --build build -j$(nproc)
```

## 常见问题

### 1. `Error: could not load cache`

**原因**：build 目录中存在旧的 CMake 缓存，与当前 CMakeLists.txt 不匹配。

**解决**：
```bash
rm -rf build CMakeCache.txt CMakeFiles
cmake -S . -B build
cmake --build build -j$(nproc)
```

### 2. `CMAKE_MAKE_PROGRAM is not set` / `Ninja not found`

**原因**：系统未安装 Ninja。

**解决**：使用默认的 Makefiles 生成器：
```bash
cmake -S . -B build  # 不指定 -G Ninja
cmake --build build -j$(nproc)
```

或安装 Ninja：
```bash
sudo apt install ninja-build
```

### 3. `CMAKE_C_COMPILER not set`

**原因**：C 编译器未正确检测。

**解决**：
```bash
# 确保安装了编译工具链
sudo apt install build-essential cmake

# 显式指定编译器
cmake -S . -B build -DCMAKE_C_COMPILER=gcc
```

### 4. FFmpeg 相关头文件找不到

**原因**：缺少 FFmpeg 开发包。

**解决**：
```bash
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev pkg-config
```

### 5. 硬件加速未启用

**检查编译输出**，应看到：
```
-- FFmpeg CUDA hwcontext supported - NVDEC enabled
-- VA-API found - Intel hwaccel enabled
```

**如果未启用**：

| 加速类型 | 依赖安装 |
|---------|---------|
| NVDEC (NVIDIA) | `sudo apt install nvidia-cuda-toolkit` 或安装 CUDA Toolkit |
| VA-API (Intel) | `sudo apt install libva-dev libva-drm2` |

## 编译输出

成功编译后生成以下可执行文件：

| 文件 | 说明 |
|-----|------|
| `stream_server` | 主服务器 |
| `stream_receiver_decode` | 流接收+解码器 |
| `test_20streams_hw` | 20路硬件解码测试 |
| `test_20streams_simple` | 20路简化测试 |
| `test_hw_decode` | 硬件解码测试 |
| `test_decode_perf` | 解码性能测试 |
| `benchmark_10streams` | 10流基准测试 |

## 远程编译示例

```bash
# SSH 到目标机器编译
ssh user@192.168.x.x "cd /path/to/stream_server && rm -rf build && cmake -S . -B build && cmake --build build -j4"
```

## 依赖清单

```bash
# 基础依赖
sudo apt install build-essential cmake pkg-config

# FFmpeg
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev

# 线程支持
sudo apt install pthread

# 可选：ZMQ 支持
sudo apt install libzmq3-dev

# 可选：NVIDIA 硬件解码
sudo apt install nvidia-cuda-toolkit

# 可选：Intel 硬件解码
sudo apt install libva-dev libva-drm2
```
