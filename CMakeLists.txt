cmake_minimum_required(VERSION 3.16)
project(rtsp_rtmp2webrtc_hls LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FFmpeg 依赖
if(WIN32)
  # Windows: 使用 find_package 查找 FFmpeg
  find_package(FFmpeg COMPONENTS avformat avcodec avutil QUIET)
  if(NOT FFmpeg_FOUND)
    # 尝试从环境变量获取 FFmpeg 路径
    if(DEFINED ENV{FFMPEG_DIR})
      set(FFMPEG_DIR $ENV{FFMPEG_DIR})
    endif()
    
    find_path(FFMPEG_INCLUDE_DIR NAMES libavformat/avformat.h)
    find_library(FFMPEG_avformat_LIBRARY NAMES avformat)
    find_library(FFMPEG_avcodec_LIBRARY NAMES avcodec)
    find_library(FFMPEG_avutil_LIBRARY NAMES avutil)
    
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIR})
    set(FFMPEG_LIBRARIES ${FFMPEG_avformat_LIBRARY} ${FFMPEG_avcodec_LIBRARY} ${FFMPEG_avutil_LIBRARY})
  endif()
else()
  # Linux/macOS: 使用 pkg-config
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
  )
endif()

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
