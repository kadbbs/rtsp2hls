#!/bin/bash

# 完整测试：使用 MediaMTX RTSP 服务器测试 RTSP 转 HLS

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TEST_HLS_DIR="$SCRIPT_DIR/test_rtsp_output"
TEST_VIDEO="$SCRIPT_DIR/test_source.mp4"
MEDIAMTX_DIR="$SCRIPT_DIR/mediamtx"

echo "========================================"
echo "完整测试：RTSP → HLS"
echo "========================================"
echo

# 检查可执行文件
if [ ! -f "$BUILD_DIR/stream_converter" ]; then
    echo "错误：可执行文件不存在"
    exit 1
fi

# 生成测试视频
if [ ! -f "$TEST_VIDEO" ]; then
    echo "生成测试视频..."
    ffmpeg -y -f lavfi -i testsrc=duration=60:size=640x480:rate=30 \
           -f lavfi -i sine=frequency=1000:duration=60 \
           -c:v libx264 -preset fast -crf 23 \
           -c:a aac -b:a 128k \
           "$TEST_VIDEO" 2>/dev/null
fi

echo "✓ 测试视频已准备：$TEST_VIDEO"

# 下载 MediaMTX
if [ ! -f "$MEDIAMTX_DIR/mediamtx" ]; then
    echo "下载 MediaMTX RTSP 服务器..."
    mkdir -p "$MEDIAMTX_DIR"
    cd "$MEDIAMTX_DIR"
    wget -q https://github.com/bluenviron/mediamtx/releases/download/v1.7.0/mediamtx_v1.7.0_linux_amd64.tar.gz
    tar xzf mediamtx_v1.7.0_linux_amd64.tar.gz
    rm mediamtx_v1.7.0_linux_amd64.tar.gz
    echo "✓ MediaMTX 已下载"
fi

# 创建 HLS 输出目录
rm -rf "$TEST_HLS_DIR"
mkdir -p "$TEST_HLS_DIR"

echo
echo "启动顺序:"
echo "  1. 启动 MediaMTX RTSP 服务器"
echo "  2. 推送测试视频到 RTSP"
echo "  3. 运行 stream_converter 拉流转 HLS"
echo

# 清理函数
cleanup() {
    echo
    echo "清理中..."
    [ ! -z "$MEDIAMTX_PID" ] && kill $MEDIAMTX_PID 2>/dev/null || true
    [ ! -z "$PUSHER_PID" ] && kill $PUSHER_PID 2>/dev/null || true
    [ ! -z "$CONVERTER_PID" ] && kill $CONVERTER_PID 2>/dev/null || true
    echo "清理完成"
}

trap cleanup EXIT

# 启动 MediaMTX
echo "启动 MediaMTX..."
cd "$MEDIAMTX_DIR"
./mediamtx > "$TEST_HLS_DIR/mediamtx.log" 2>&1 &
MEDIAMTX_PID=$!
sleep 3

if ! kill -0 $MEDIAMTX_PID 2>/dev/null; then
    echo "错误：MediaMTX 启动失败"
    cat "$TEST_HLS_DIR/mediamtx.log"
    exit 1
fi
echo "✓ MediaMTX 已启动 (PID: $MEDIAMTX_PID)"

# 推送视频到 RTSP
echo "推送视频到 RTSP..."
cd "$SCRIPT_DIR"
ffmpeg -re -stream_loop -1 -i "$TEST_VIDEO" \
       -c copy -f rtsp rtsp://localhost:8554/live \
       > "$TEST_HLS_DIR/pusher.log" 2>&1 &
PUSHER_PID=$!
sleep 2
echo "✓ RTSP 推流已启动 (PID: $PUSHER_PID)"

# 验证 RTSP 流可用
echo "验证 RTSP 流..."
ffprobe -v quiet -show_format rtsp://localhost:8554/live && \
    echo "✓ RTSP 流可用" || echo "⚠ RTSP 流可能未就绪"

# 运行 stream_converter
echo
echo "启动 stream_converter..."
cd "$BUILD_DIR"
./stream_converter \
    -s 2 \
    -l 5 \
    rtsp://localhost:8554/live \
    "$TEST_HLS_DIR" \
    > "$TEST_HLS_DIR/converter.log" 2>&1 &
CONVERTER_PID=$!
echo "✓ stream_converter 已启动 (PID: $CONVERTER_PID)"

# 等待 HLS 生成
echo
echo "等待 HLS 文件生成..."
for i in {1..20}; do
    if [ -f "$TEST_HLS_DIR/playlist.m3u8" ]; then
        echo "✓ HLS 播放列表已生成"
        break
    fi
    sleep 1
done

if [ ! -f "$TEST_HLS_DIR/playlist.m3u8" ]; then
    echo "错误：HLS 播放列表未生成"
    echo "日志:"
    tail -20 "$TEST_HLS_DIR/converter.log"
    exit 1
fi

# 等待 TS 切片
sleep 5

# 检查结果
echo
echo "========================================"
echo "测试结果"
echo "========================================"

if [ -f "$TEST_HLS_DIR/playlist.m3u8" ]; then
    echo "✓ playlist.m3u8 存在"
    echo
    cat "$TEST_HLS_DIR/playlist.m3u8"
    echo
    
    ts_count=$(ls -1 "$TEST_HLS_DIR"/segment_*.ts 2>/dev/null | wc -l)
    echo "✓ TS 切片数量：$ts_count"
    
    if [ $ts_count -gt 0 ]; then
        ls -lh "$TEST_HLS_DIR"/segment_*.ts
    fi
else
    echo "✗ playlist.m3u8 不存在"
fi

echo
echo "========================================"
echo "测试完成"
echo "========================================"
echo
echo "日志文件位置:"
echo "  MediaMTX:    $TEST_HLS_DIR/mediamtx.log"
echo "  推流：       $TEST_HLS_DIR/pusher.log"
echo "  转换器：     $TEST_HLS_DIR/converter.log"
echo
echo "按 Ctrl+C 停止所有服务"

wait
