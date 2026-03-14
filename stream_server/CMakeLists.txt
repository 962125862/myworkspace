cmake_minimum_required(VERSION 3.10)
project(stream_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0")

# 查找 FFmpeg
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED 
    libavcodec>=58.0
    libavformat>=58.0
    libavutil>=56.0
    libswscale>=5.0
)

# 查找线程库
find_package(Threads REQUIRED)

# 检测硬件加速/可选特性支持（编译宏 + 额外链接/头文件）
set(HWACCEL_DEFS "")
set(HWACCEL_LIBS "")
set(HWACCEL_INCLUDES "")

# Optional: ZMQ (builtin bridge)
pkg_check_modules(ZMQ QUIET libzmq)
if(ZMQ_FOUND)
    message(STATUS "libzmq found - builtin ZMQ bridge enabled")
    list(APPEND HWACCEL_DEFS HAVE_ZMQ)
    list(APPEND HWACCEL_LIBS ${ZMQ_LIBRARIES})
    list(APPEND HWACCEL_INCLUDES ${ZMQ_INCLUDE_DIRS})
else()
    message(STATUS "libzmq not found - builtin ZMQ bridge disabled")
endif()

# Optional: libyuv (ZMQ BGR output)
find_path(LIBYUV_INCLUDE_DIR
    NAMES libyuv.h libyuv/convert_argb.h
    PATHS /usr/include /usr/local/include
)
find_library(LIBYUV_LIBRARY
    NAMES yuv libyuv
    PATHS /usr/lib /usr/lib64 /usr/local/lib /lib/x86_64-linux-gnu
)
if(LIBYUV_INCLUDE_DIR AND LIBYUV_LIBRARY)
    message(STATUS "libyuv found - BGR conversion enabled")
    list(APPEND HWACCEL_DEFS HAVE_LIBYUV)
    list(APPEND HWACCEL_LIBS ${LIBYUV_LIBRARY})
    list(APPEND HWACCEL_INCLUDES ${LIBYUV_INCLUDE_DIR})
else()
    message(STATUS "libyuv not found - ZMQ BGR output disabled")
endif()

# 检测 CUDA Toolkit 头文件路径 (hwcontext_cuda.h 需要 cuda.h)
set(CUDA_SEARCH_PATHS
    /usr/local/cuda/include
    /usr/local/cuda-12/include
    /usr/local/cuda-12.2/include
    /usr/local/cuda-11/include
    /usr/local/cuda-11.8/include
)
set(CUDA_INCLUDE_DIR "")
foreach(_cuda_path ${CUDA_SEARCH_PATHS})
    if(EXISTS "${_cuda_path}/cuda.h")
        set(CUDA_INCLUDE_DIR ${_cuda_path})
        message(STATUS "Found cuda.h in ${_cuda_path}")
        break()
    endif()
endforeach()

# 检测 FFmpeg 是否支持 CUDA hwcontext (NVDEC关键依赖)
include(CheckIncludeFile)
set(CMAKE_REQUIRED_INCLUDES ${FFMPEG_INCLUDE_DIRS} ${CUDA_INCLUDE_DIR})
set(CMAKE_REQUIRED_FLAGS "${FFMPEG_CFLAGS}")
check_include_file("libavutil/hwcontext_cuda.h" HAVE_FFMPEG_CUDA_H)
if(NOT HAVE_FFMPEG_CUDA_H)
    # 备选：直接检查文件是否存在
    foreach(_inc_dir ${FFMPEG_INCLUDE_DIRS})
        if(EXISTS "${_inc_dir}/libavutil/hwcontext_cuda.h")
            set(HAVE_FFMPEG_CUDA_H TRUE)
            break()
        endif()
    endforeach()
endif()
if(HAVE_FFMPEG_CUDA_H AND CUDA_INCLUDE_DIR)
    message(STATUS "FFmpeg CUDA hwcontext supported - NVDEC enabled")
    list(APPEND HWACCEL_DEFS HAVE_CUDA)
    list(APPEND HWACCEL_INCLUDES ${CUDA_INCLUDE_DIR})
elseif(HAVE_FFMPEG_CUDA_H)
    message(STATUS "FFmpeg has hwcontext_cuda.h but cuda.h not found - NVDEC disabled")
    message(STATUS "  Install CUDA Toolkit or set CUDA_INCLUDE_DIR")
else()
    message(STATUS "FFmpeg CUDA hwcontext NOT found - NVDEC disabled")
endif()

# 检测 Intel VA-API 支持
pkg_check_modules(VAAPI QUIET libva libva-drm)
check_include_file("libavutil/hwcontext_vaapi.h" HAVE_FFMPEG_VAAPI_H)
if(NOT HAVE_FFMPEG_VAAPI_H)
    foreach(_inc_dir ${FFMPEG_INCLUDE_DIRS})
        if(EXISTS "${_inc_dir}/libavutil/hwcontext_vaapi.h")
            set(HAVE_FFMPEG_VAAPI_H TRUE)
            break()
        endif()
    endforeach()
endif()
if(VAAPI_FOUND AND HAVE_FFMPEG_VAAPI_H)
    message(STATUS "VA-API found - Intel hwaccel enabled")
    list(APPEND HWACCEL_DEFS HAVE_VAAPI)
    list(APPEND HWACCEL_LIBS ${VAAPI_LIBRARIES})
    include_directories(${VAAPI_INCLUDE_DIRS})
else()
    message(STATUS "VA-API not found - Intel hwaccel disabled")
endif()

# 打印检测结果
message(STATUS "Hardware acceleration defines: ${HWACCEL_DEFS}")
message(STATUS "FFMPEG_INCLUDE_DIRS: ${FFMPEG_INCLUDE_DIRS}")

# 头文件目录
include_directories(include ${FFMPEG_INCLUDE_DIRS} ${HWACCEL_INCLUDES})

# 源文件
set(SOURCES
    src/main.c
    src/protocol.c
    src/stream.c
    src/server.c
    src/decoder.c
    src/h264_tap.c
    src/zmq_bridge.c
)

# 可执行文件
add_executable(stream_server ${SOURCES})

# 编译定义
target_compile_definitions(stream_server PRIVATE ${HWACCEL_DEFS})

# 链接库
target_link_libraries(stream_server
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    ${HWACCEL_LIBS}
    m  # math library
)

# 链接选项
target_link_options(stream_server PRIVATE ${FFMPEG_LDFLAGS})

# 安装
install(TARGETS stream_server DESTINATION bin)
install(PROGRAMS install_deps.sh DESTINATION .)

# 20路硬件解码压力测试 (简化版)
add_executable(test_20streams_simple test_20streams_simple.c src/decoder.c)
target_include_directories(test_20streams_simple PRIVATE include ${FFMPEG_INCLUDE_DIRS})
target_compile_definitions(test_20streams_simple PRIVATE ${HWACCEL_DEFS})
target_link_libraries(test_20streams_simple
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    ${HWACCEL_LIBS}
    m
)

# 20路硬件解码压力测试 (完整版 - 支持NVIDIA/VA-API对比)
add_executable(test_20streams_hw test_20streams_hw.c src/decoder.c)
target_include_directories(test_20streams_hw PRIVATE include ${FFMPEG_INCLUDE_DIRS})
target_compile_definitions(test_20streams_hw PRIVATE ${HWACCEL_DEFS})
target_link_libraries(test_20streams_hw
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    ${HWACCEL_LIBS}
    m
)
target_link_options(test_20streams_hw PRIVATE ${FFMPEG_LDFLAGS})

# 流接收+解码器
add_executable(stream_receiver_decode stream_receiver_decode.c src/decoder.c src/protocol.c)
target_include_directories(stream_receiver_decode PRIVATE include ${FFMPEG_INCLUDE_DIRS})
target_compile_definitions(stream_receiver_decode PRIVATE ${HWACCEL_DEFS})
target_link_libraries(stream_receiver_decode
    Threads::Threads
    ${FFMPEG_LIBRARIES}
    ${HWACCEL_LIBS}
    m
)
target_link_options(stream_receiver_decode PRIVATE ${FFMPEG_LDFLAGS})

# 测试（可选）
enable_testing()
add_subdirectory(tests)
