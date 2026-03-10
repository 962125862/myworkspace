# Tests

# 解码器源文件
set(DECODER_SOURCES ../src/decoder.c)

# 硬件解码检测测试
add_executable(test_hw_decode test_hw_decode.c ${DECODER_SOURCES})
target_include_directories(test_hw_decode PRIVATE ../include ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(test_hw_decode
    ${FFMPEG_LIBRARIES}
    m
)

# 解码性能测试
add_executable(test_decode_perf test_decode_perf.c ${DECODER_SOURCES})
target_include_directories(test_decode_perf PRIVATE ../include ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(test_decode_perf
    ${FFMPEG_LIBRARIES}
    m
)

# 10秒负载测试
add_executable(test_10sec_load test_10sec_load.c ${DECODER_SOURCES})
target_include_directories(test_10sec_load PRIVATE ../include ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(test_10sec_load
    ${FFMPEG_LIBRARIES}
    m
)

# 10路流基准测试
add_executable(benchmark_10streams benchmark_10streams.c ${DECODER_SOURCES})
target_include_directories(benchmark_10streams PRIVATE ../include ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(benchmark_10streams
    ${FFMPEG_LIBRARIES}
    m
    pthread
)

# 20路硬件解码压力测试 (已移到上级 CMakeLists.txt)
