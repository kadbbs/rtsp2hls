#!/bin/bash

# 快速测试脚本：使用本地视频文件测试 HLS 输出

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TEST_HLS_DIR="$SCRIPT_DIR/test_output"
TEST_VIDEO="$SCRIPT_DIR/test_source.mp4"

echo "========================================"
echo "快速测试：RTSP/RTMP 转 HLS"
echo "========================================"
echo

# 检查可执行文件
if [ ! -f "$BUILD_DIR/stream_converter" ]; then
    echo "错误：可执行文件不存在，请先编译"
    echo "运行：cd $BUILD_DIR && cmake .. && make"
    exit 1
fi

echo "✓ 可执行文件存在"

# 创建测试输出目录
rm -rf "$TEST_HLS_DIR"
mkdir -p "$TEST_HLS_DIR"
echo "✓ 测试目录已创建：$TEST_HLS_DIR"

# 生成测试视频（30 秒测试图案）
if [ ! -f "$TEST_VIDEO" ]; then
    echo "正在生成测试视频..."
    ffmpeg -y -f lavfi -i testsrc=duration=30:size=640x480:rate=30 \
           -f lavfi -i sine=frequency=1000:duration=30 \
           -c:v libx264 -preset fast -crf 23 \
           -c:a aac -b:a 128k \
           -pix_fmt yuv420p \
           "$TEST_VIDEO" 2>&1 | tail -5
    
    if [ -f "$TEST_VIDEO" ]; then
        echo "✓ 测试视频已生成：$TEST_VIDEO"
    else
        echo "✗ 生成测试视频失败"
        exit 1
    fi
else
    echo "✓ 测试视频已存在：$TEST_VIDEO"
fi

echo
echo "开始转换测试..."
echo "输入：$TEST_VIDEO"
echo "输出：$TEST_HLS_DIR/playlist.m3u8"
echo

# 运行转换器（后台运行 15 秒）
cd "$BUILD_DIR"
timeout 15 ./stream_converter \
    -s 2 \
    -l 5 \
    "$TEST_VIDEO" \
    "$TEST_HLS_DIR" || true

echo
echo "========================================"
echo "测试结果"
echo "========================================"

# 检查输出
if [ -f "$TEST_HLS_DIR/playlist.m3u8" ]; then
    echo "✓ playlist.m3u8 已生成"
    echo
    echo "播放列表内容:"
    echo "----------------------------------------"
    cat "$TEST_HLS_DIR/playlist.m3u8"
    echo "----------------------------------------"
    echo
    
    # 统计 TS 文件
    ts_count=$(ls -1 "$TEST_HLS_DIR"/segment_*.ts 2>/dev/null | wc -l)
    if [ $ts_count -gt 0 ]; then
        echo "✓ TS 切片数量：$ts_count"
        echo
        echo "TS 文件列表:"
        ls -lh "$TEST_HLS_DIR"/segment_*.ts
        echo
    else
        echo "✗ 没有生成 TS 切片"
    fi
    
    # 使用 ffprobe 检查播放列表
    echo "HLS 播放列表验证:"
    ffprobe -v quiet -show_format "$TEST_HLS_DIR/playlist.m3u8" 2>/dev/null && \
        echo "✓ HLS 播放列表格式正确" || echo "⚠ HLS 播放列表可能有问题"
    
else
    echo "✗ playlist.m3u8 未生成"
    echo
    echo "日志输出:"
    if [ -f "$TEST_HLS_DIR/converter.log" ]; then
        tail -20 "$TEST_HLS_DIR/converter.log"
    fi
fi

echo
echo "========================================"
echo "测试完成"
echo "========================================"
echo
echo "你可以使用以下方式播放 HLS:"
echo "  1. VLC: vlc $TEST_HLS_DIR/playlist.m3u8"
echo "  2. HTTP 服务器:"
echo "     cd $TEST_HLS_DIR && python3 -m http.server 8080"
echo "     然后访问：http://localhost:8080/playlist.m3u8"
echo
