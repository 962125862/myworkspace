cmake_minimum_required(VERSION 3.28)
project(my_ml_work C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-embedded/cmake"
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)

add_subdirectory(third_party/moonlight-common-c third_party/moonlight-common-c-build)
add_subdirectory(third_party/moonlight-embedded/libgamestream third_party/libgamestream-build)

add_executable(ml_worker
        src/main.c
        src/decoder_ffmpeg.c
        src/video_callbacks.c
        src/connection_callbacks.c
)

target_include_directories(ml_worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-common-c/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moonlight-embedded/libgamestream
)

target_link_libraries(ml_worker PRIVATE
        moonlight-common-c
        gamestream
        PkgConfig::AVCODEC
        PkgConfig::AVUTIL
)