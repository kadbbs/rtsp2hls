cmake_minimum_required(VERSION 3.16)
project(rtsp_rtmp2webrtc_hls LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FFmpeg 依赖
if(WIN32)
  # Windows: 使用 Chocolatey 安装的 FFmpeg
  # Chocolatey 将 FFmpeg 安装在 C:/ProgramData/chocolatey/lib/ffmpeg-full/tools/ffmpeg-*/
  file(GLOB FFMPEG_ROOT "C:/ProgramData/chocolatey/lib/ffmpeg-full/tools/ffmpeg-*")
  if(FFMPEG_ROOT)
    list(GET FFMPEG_ROOT 0 FFMPEG_ROOT)
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
    set(FFMPEG_LIBRARY_DIR "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBRARIES
      "${FFMPEG_LIBRARY_DIR}/avformat.lib"
      "${FFMPEG_LIBRARY_DIR}/avcodec.lib"
      "${FFMPEG_LIBRARY_DIR}/avutil.lib"
    )
  endif()
elseif(APPLE)
  # macOS: 使用 Homebrew
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
  )
else()
  # Linux: 使用 pkg-config
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

# Windows 特定配置
if(WIN32)
  target_include_directories(media_server_v2 PRIVATE
    ${FFMPEG_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(media_server_v2 PRIVATE
    ${FFMPEG_LIBRARIES}
    ws2_32
  )
  target_compile_options(media_server_v2 PRIVATE /W4)
else()
  target_include_directories(media_server_v2 PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_directories(media_server_v2 PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(media_server_v2 PRIVATE
    ${FFMPEG_LIBRARIES}
  )
  target_compile_options(media_server_v2 PRIVATE -Wall -Wextra)
endif()

# stream_converter
if(WIN32)
  target_include_directories(stream_converter PRIVATE
    ${FFMPEG_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(stream_converter PRIVATE
    ${FFMPEG_LIBRARIES}
    ws2_32
  )
else()
  target_include_directories(stream_converter PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_directories(stream_converter PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_libraries(stream_converter PRIVATE
    ${FFMPEG_LIBRARIES}
  )
endif()
