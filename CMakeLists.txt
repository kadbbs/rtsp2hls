cmake_minimum_required(VERSION 3.16)
project(rtsp_rtmp2webrtc_hls LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FFmpeg 依赖（拉流 + HLS 复用只需这三个库）
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
  libavformat
  libavcodec
  libavutil
)

# 主程序 1: RTSP/RTMP 拉流转 HLS（旧版本，保留）
add_executable(stream_converter
  src/main.cpp
  src/stream_converter.cpp
  src/webrtc_sender_stub.cpp
)

# 主程序 2: RTSP/RTMP 服务器 → WebRTC/HLS（新版本，带 HTTP）
add_executable(media_server_v2
  src/media_server_main_v2.cpp
  src/media_server.cpp
  src/rtsp_server.cpp
  src/rtsp_session.cpp
  src/hls_muxer.cpp
  src/http_server.cpp
  src/webrtc_sender_stub.cpp
)

target_include_directories(media_server_v2 PRIVATE
  ${FFMPEG_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_directories(media_server_v2 PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(media_server_v2 PRIVATE
  ${FFMPEG_LIBRARIES}
)

target_compile_options(media_server_v2 PRIVATE -Wall -Wextra)

target_include_directories(stream_converter PRIVATE
  ${FFMPEG_INCLUDE_DIRS}
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_directories(stream_converter PRIVATE ${FFMPEG_LIBRARY_DIRS})

target_link_libraries(stream_converter PRIVATE
  ${FFMPEG_LIBRARIES}
)

# 编译选项
target_compile_options(stream_converter PRIVATE -Wall -Wextra)
