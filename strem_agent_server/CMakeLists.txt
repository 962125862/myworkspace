cmake_minimum_required(VERSION 3.28)

# This directory builds the strem_agent_server binary.
# Note: The source code is POSIX/Linux-oriented (sockets, pthread, unistd, etc.).
# CMake here primarily standardizes builds on Linux/WSL/MSYS2. Native MSVC support
# would require a portability layer.

project(strem_agent_server C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(strem_agent_server
  src/auth.c
  src/h264_tap.c
  src/main.c
  src/tcp_util.c
  src/tlv_protocol.c
  src/udp_forward.c
)

target_include_directories(strem_agent_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (MSVC)
  target_compile_options(strem_agent_server PRIVATE /W4)
else()
  target_compile_options(strem_agent_server PRIVATE -Wall -Wextra)
endif()

find_package(Threads REQUIRED)
target_link_libraries(strem_agent_server PRIVATE Threads::Threads)

