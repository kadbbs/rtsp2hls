#!/bin/bash

# 一键启动所有服务

RTSP_PORT=${1:-8555}
HTTP_PORT=${2:-8080}
HLS_DIR=${3:-/tmp/hls_output}

echo "========================================"
echo "RTSP/RTMP 转 HLS - 一键启动"
echo "========================================"
echo
echo "配置:"
echo "  RTSP 端口：$RTSP_PORT"
echo "  HTTP 端口：$HTTP_PORT"
echo "  HLS 目录：$HLS_DIR"
echo

# 清理旧进程
pkill -f "media_server_v2" 2>/dev/null
pkill -f "ffmpeg.*rtsp" 2>/dev/null

# 创建目录
mkdir -p "$HLS_DIR"

# 启动媒体服务器
echo "启动媒体服务器..."
cd /home/bs/code/rtsp_rtmp2webrtc_hls/build
./media_server_v2 --rtsp-port $RTSP_PORT --hls-dir $HLS_DIR --http-port $HTTP_PORT &
SERVER_PID=$!

sleep 3

# 检查服务器是否启动
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "错误：媒体服务器启动失败"
    exit 1
fi

echo "✓ 媒体服务器已启动 (PID: $SERVER_PID)"
echo
echo "========================================"
echo "推流说明"
echo "========================================"
echo
echo "现在在另一个终端推流:"
echo
echo "  # 文件推流"
echo "  ffmpeg -re -i video.mp4 -c copy -f rtsp rtsp://localhost:$RTSP_PORT/live/mystream"
echo
echo "  # Windows 摄像头推流"
echo "  ffmpeg -f dshow -rtbufsize 1500M -i \"video=Integrated Camera\" ^"
echo "         -c:v libx264 -preset ultrafast ^"
echo "         -f rtsp rtsp://你的 IP:$RTSP_PORT/live/camera"
echo
echo "推流后，HLS 文件将生成在：$HLS_DIR"
echo
echo "Web 播放地址：http://你的 IP:$HTTP_PORT/player.html"
echo
echo "按 Ctrl+C 停止所有服务"
echo "========================================"

# 等待退出
wait $SERVER_PID
