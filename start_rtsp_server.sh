#!/bin/bash

# 使用 FFmpeg 启动一个简单的 RTSP 测试服务器

VIDEO_SOURCE="${1:-test_source.mp4}"
RTSP_PORT="${2:-8554}"

echo "启动 RTSP 测试服务器..."
echo "  视频源：$VIDEO_SOURCE"
echo "  RTSP 端口：$RTSP_PORT"
echo "  RTSP 地址：rtsp://localhost:$RTSP_PORT/live/test"
echo

# 检查视频文件
if [ ! -f "$VIDEO_SOURCE" ]; then
    echo "错误：视频文件不存在：$VIDEO_SOURCE"
    echo "先生成测试视频..."
    ffmpeg -y -f lavfi -i testsrc=duration=300:size=640x480:rate=30 \
           -f lavfi -i sine=frequency=1000:duration=300 \
           -c:v libx264 -preset fast -crf 23 \
           -c:a aac -b:a 128k \
           -pix_fmt yuv420p \
           "$VIDEO_SOURCE" 2>/dev/null
fi

# 启动 RTSP 服务器（使用 FFmpeg 的 tee muxer 模拟）
# 注意：FFmpeg 本身不能直接作为 RTSP 服务器
# 这里我们使用 v4l2loopback 或者建议使用其他 RTSP 服务器软件

echo "提示：FFmpeg 不能直接作为 RTSP 服务器"
echo
echo "推荐的 RTSP 服务器选项："
echo "  1. MediaMTX (原 rtsp-simple-server):"
echo "     wget https://github.com/bluenviron/mediamtx/releases/download/v1.7.0/mediamtx_v1.7.0_linux_amd64.tar.gz"
echo "     tar xzf mediamtx_v1.7.0_linux_amd64.tar.gz"
echo "     ./mediamtx"
echo "     然后推流：ffmpeg -re -i $VIDEO_SOURCE -c copy -f rtsp rtsp://localhost:$RTSP_PORT/live"
echo
echo "  2. SRS (Simple RTMP/RTSP Server):"
echo "     docker run --rm -p 1935:1935 -p 8554:8554 registry.cn-hangzhou.aliyuncs.com/ossrs/srs:5"
echo
echo "  3. 使用真实网络摄像头"
echo
