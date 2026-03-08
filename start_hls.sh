#!/bin/bash

# 手动启动 HLS 生成脚本

if [ $# -lt 2 ]; then
    echo "用法：$0 <RTSP 流地址> <HLS 输出目录>"
    echo
    echo "示例:"
    echo "  $0 rtsp://localhost:8555/live/mystream /tmp/hls_output/live/mystream"
    exit 1
fi

RTSP_URL="$1"
HLS_DIR="$2"

# 创建输出目录
mkdir -p "$HLS_DIR"

echo "启动 HLS 生成..."
echo "  输入：$RTSP_URL"
echo "  输出：$HLS_DIR/playlist.m3u8"
echo

# 启动 FFmpeg
ffmpeg -y \
    -rtsp_transport tcp \
    -timeout 5000000 \
    -i "$RTSP_URL" \
    -c copy \
    -f hls \
    -hls_time 2 \
    -hls_list_size 5 \
    -hls_flags delete_segments \
    "$HLS_DIR/playlist.m3u8"
