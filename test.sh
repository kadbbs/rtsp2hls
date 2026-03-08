#!/bin/bash

# RTSP/RTMP 转 HLS 测试脚本

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
TEST_HLS_DIR="$SCRIPT_DIR/test_hls_output"
TEST_VIDEO="$SCRIPT_DIR/test_video.mp4"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# 检查可执行文件
check_executable() {
    if [ ! -f "$BUILD_DIR/stream_converter" ]; then
        echo_error "可执行文件不存在，请先编译项目"
        echo "运行：cd $SCRIPT_DIR && mkdir -p build && cd build && cmake .. && make"
        exit 1
    fi
    echo_info "可执行文件检查通过"
}

# 创建测试 HLS 输出目录
setup_test_dir() {
    rm -rf "$TEST_HLS_DIR"
    mkdir -p "$TEST_HLS_DIR"
    echo_info "测试目录已创建：$TEST_HLS_DIR"
}

# 生成测试视频（使用 ffmpeg 生成测试图案）
generate_test_video() {
    echo_info "生成测试视频..."
    ffmpeg -y -f lavfi -i testsrc=duration=30:size=640x480:rate=30 \
           -f lavfi -i sine=frequency=1000:duration=30 \
           -c:v libx264 -preset fast -crf 23 \
           -c:a aac -b:a 128k \
           -pix_fmt yuv420p \
           "$TEST_VIDEO" 2>/dev/null
    
    if [ -f "$TEST_VIDEO" ]; then
        echo_info "测试视频已生成：$TEST_VIDEO"
        ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$TEST_VIDEO" | \
            xargs -I {} echo_info "视频时长：{} 秒"
    else
        echo_error "生成测试视频失败"
        exit 1
    fi
}

# 启动 FFmpeg 作为 RTMP 服务器（使用 ffmpeg 的 tee  muxer 模拟）
start_rtmp_source() {
    echo_info "启动 RTMP 测试源..."
    
    # 使用 ffmpeg 循环播放测试视频并输出到 RTMP
    ffmpeg -re -stream_loop -1 -i "$TEST_VIDEO" \
           -c copy -f flv rtmp://localhost:1935/live/test \
           2>&1 | tee "$TEST_HLS_DIR/rtmp_source.log" &
    
    RTMP_PID=$!
    echo_info "RTMP 源已启动 (PID: $RTMP_PID)"
    sleep 2
}

# 启动 HTTP 服务器提供 HLS
start_http_server() {
    echo_info "启动 HTTP 服务器（端口 8080）..."
    cd "$TEST_HLS_DIR"
    python3 -m http.server 8080 > "$TEST_HLS_DIR/http_server.log" 2>&1 &
    HTTP_PID=$!
    echo_info "HTTP 服务器已启动 (PID: $HTTP_PID)"
    echo_info "HLS 播放地址：http://localhost:8080/playlist.m3u8"
}

# 运行 stream_converter
run_converter() {
    local input_url="$1"
    
    echo_info "启动 stream_converter..."
    echo_info "输入 URL: $input_url"
    echo_info "HLS 输出：$TEST_HLS_DIR"
    
    cd "$BUILD_DIR"
    ./stream_converter \
        -s 2 \
        -l 5 \
        -i test_stream \
        "$input_url" \
        "$TEST_HLS_DIR" \
        2>&1 | tee "$TEST_HLS_DIR/converter.log" &
    
    CONVERTER_PID=$!
    echo_info "stream_converter 已启动 (PID: $CONVERTER_PID)"
}

# 等待 HLS 文件生成
wait_for_hls() {
    echo_info "等待 HLS 文件生成..."
    local max_wait=30
    local waited=0
    
    while [ $waited -lt $max_wait ]; do
        if [ -f "$TEST_HLS_DIR/playlist.m3u8" ]; then
            echo_info "HLS 播放列表已生成"
            
            # 等待至少一个 TS 切片
            while [ $waited -lt $max_wait ]; do
                if ls "$TEST_HLS_DIR"/segment_*.ts 1> /dev/null 2>&1; then
                    echo_info "TS 切片已生成"
                    return 0
                fi
                sleep 1
                waited=$((waited + 1))
            done
        fi
        sleep 1
        waited=$((waited + 1))
    done
    
    echo_error "等待 HLS 文件超时"
    return 1
}

# 检查 HLS 输出
check_hls_output() {
    echo_info "检查 HLS 输出..."
    
    if [ -f "$TEST_HLS_DIR/playlist.m3u8" ]; then
        echo_info "✓ playlist.m3u8 存在"
        echo "  内容预览:"
        head -10 "$TEST_HLS_DIR/playlist.m3u8" | sed 's/^/    /'
    else
        echo_error "✗ playlist.m3u8 不存在"
        return 1
    fi
    
    local ts_count=$(ls -1 "$TEST_HLS_DIR"/segment_*.ts 2>/dev/null | wc -l)
    if [ $ts_count -gt 0 ]; then
        echo_info "✓ TS 切片数量：$ts_count"
        ls -lh "$TEST_HLS_DIR"/segment_*.ts | awk '{print "    " $9 " - " $5}' | head -5
    else
        echo_error "✗ 没有生成 TS 切片"
        return 1
    fi
    
    return 0
}

# 清理
cleanup() {
    echo_warn "清理测试环境..."
    
    # 杀死所有后台进程
    if [ ! -z "$CONVERTER_PID" ]; then
        kill $CONVERTER_PID 2>/dev/null || true
        echo_info "已停止 stream_converter"
    fi
    
    if [ ! -z "$RTMP_PID" ]; then
        kill $RTMP_PID 2>/dev/null || true
        echo_info "已停止 RTMP 源"
    fi
    
    if [ ! -z "$HTTP_PID" ]; then
        kill $HTTP_PID 2>/dev/null || true
        echo_info "已停止 HTTP 服务器"
    fi
    
    # 清理测试视频（可选）
    # rm -f "$TEST_VIDEO"
    
    echo_info "清理完成"
}

# 设置信号处理
trap cleanup EXIT INT TERM

# 主测试流程
main() {
    echo_info "=========================================="
    echo_info "RTSP/RTMP 转 HLS 测试"
    echo_info "=========================================="
    echo
    
    # 检查可执行文件
    check_executable
    
    # 创建测试目录
    setup_test_dir
    
    # 生成测试视频
    generate_test_video
    
    echo
    echo_info "选择测试模式:"
    echo "  1) 使用本地文件测试（推荐，快速）"
    echo "  2) 使用 RTMP 测试（需要 ffmpeg 推流）"
    echo "  3) 使用外部 RTSP 源"
    echo
    
    read -p "请选择测试模式 (1/2/3): " -n 1 -r
    echo
    
    case $REPLY in
        1)
            # 使用本地文件测试
            echo_info "使用本地文件测试..."
            run_converter "$TEST_VIDEO"
            wait_for_hls
            check_hls_output
            ;;
        2)
            # 使用 RTMP 测试
            echo_info "使用 RTMP 测试..."
            start_rtmp_source
            run_converter "rtmp://localhost:1935/live/test"
            wait_for_hls
            check_hls_output
            ;;
        3)
            # 使用外部 RTSP 源
            read -p "请输入 RTSP URL: " rtsp_url
            run_converter "$rtsp_url"
            wait_for_hls
            check_hls_output
            ;;
        *)
            echo_error "无效选择"
            exit 1
            ;;
    esac
    
    echo
    echo_info "=========================================="
    echo_info "测试完成！"
    echo_info "=========================================="
    echo
    echo_info "HLS 输出目录：$TEST_HLS_DIR"
    echo_info "播放地址（如果启动了 HTTP 服务器）: http://localhost:8080/playlist.m3u8"
    echo
    echo_info "按 Ctrl+C 退出"
    
    # 保持运行，让用户可以查看日志
    wait
}

# 运行测试
main
