#!/bin/bash

# 网络诊断脚本

echo "========================================"
echo "网络诊断"
echo "========================================"
echo

# 1. 检查本地网络接口
echo "1. 本机 IP 地址:"
ip addr show | grep "inet " | grep -v "127.0.0.1"
echo

# 2. 测试到 Windows 的连通性
WINDOWS_IP="192.168.1.5"
echo "2. 测试到 $WINDOWS_IP 的连通性:"
ping -c 3 "$WINDOWS_IP" && echo "✓ 网络通" || echo "✗ 网络不通"
echo

# 3. 测试 RTSP 端口
echo "3. 测试 RTSP 端口 (8554):"
timeout 2 bash -c "cat < /dev/null > /dev/tcp/$WINDOWS_IP/8554" 2>/dev/null && \
    echo "✓ 端口 8554 开放" || echo "✗ 端口 8554 未开放"
echo

# 4. 使用 ffprobe 测试
echo "4. 使用 ffprobe 测试 RTSP 流:"
timeout 5 ffprobe -v quiet -show_format "rtsp://$WINDOWS_IP:8554/mystream" 2>&1 && \
    echo "✓ RTSP 流可用" || echo "✗ RTSP 流不可用"
echo

# 5. 使用 VLC 测试（如果已安装）
if command -v vlc &> /dev/null; then
    echo "5. 使用 VLC 测试（命令行）:"
    timeout 5 vlc "rtsp://$WINDOWS_IP:8554/mystream" --run-time 2 vlc://quit 2>&1 | head -5
    echo
fi

echo "========================================"
echo "诊断完成"
echo "========================================"
echo
echo "如果网络不通，请检查:"
echo "  1. Windows 防火墙是否开放 8554 端口"
echo "  2. Windows 和 Linux 是否在同一网段"
echo "  3. MediaMTX 是否正确启动"
echo
echo "Windows 防火墙设置:"
echo "  以管理员身份运行 PowerShell:"
echo "  New-NetFirewallRule -DisplayName \"RTSP Server\" -Direction Inbound -LocalPort 8554 -Protocol TCP -Action Allow"
