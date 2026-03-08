#!/bin/bash

# 简单的 HLS 生成器 - 直接拉流模式

if [ $# -lt 2 ]; then
    echo "用法：$0 <输入源> <HLS 输出目录>"
    echo
    echo "输入源可以是:"
    echo "  - 文件：video.mp4"
    echo "  - 摄像头：/dev/video0"
    echo "  - 屏幕：:0.0+100,100"
    exit 1
fi

INPUT="$1"
OUTPUT_DIR="$2"

# 创建输出目录
mkdir -p "$OUTPUT_DIR"

echo "========================================"
echo "HLS 生成器"
echo "========================================"
echo "输入：$INPUT"
echo "输出：$OUTPUT_DIR/playlist.m3u8"
echo "========================================"
echo

# 检测输入类型
if [ -f "$INPUT" ]; then
    echo "检测到文件输入，使用 -re 参数（实时速度）"
    ffmpeg -y -re \
        -i "$INPUT" \
        -c:v libx264 -preset ultrafast -crf 23 \
        -c:a aac -b:a 128k \
        -f hls \
        -hls_time 2 \
        -hls_list_size 5 \
        -hls_flags delete_segments \
        "$OUTPUT_DIR/playlist.m3u8"
elif [ -e "$INPUT" ]; then
    echo "检测到设备输入（摄像头/屏幕）"
    ffmpeg -y \
        -i "$INPUT" \
        -c:v libx264 -preset ultrafast -crf 23 \
        -c:a aac -b:a 128k \
        -f hls \
        -hls_time 2 \
        -hls_list_size 5 \
        -hls_flags delete_segments \
        "$OUTPUT_DIR/playlist.m3u8"
else
    echo "错误：输入源不存在：$INPUT"
    exit 1
fi
