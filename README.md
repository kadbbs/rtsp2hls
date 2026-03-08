# RTSP/RTMP 转 HLS 媒体服务器

Linux C++17 项目，实现 RTSP 推流接收并输出为 **HLS**，内置 HTTP 服务器提供 Web 播放。

## 功能特性

- ✅ RTSP 推流接收（支持 H.264 编码）
- ✅ HLS 输出（m3u8 + TS 切片）
- ✅ 内置 HTTP 服务器（提供 HLS 和 Web 播放器）
- ✅ 自动从 SDP 提取 SPS/PPS
- ✅ 断线自动重连
- ✅ WebRTC 接口（待实现）
- 🚧 RTMP 推流（框架已搭建）

## 项目结构

```
rtsp_rtmp2webrtc_hls/
├── CMakeLists.txt              # CMake 构建配置
├── README.md                   # 本文件
├── include/
│   ├── media_server.hpp        # 媒体服务器主类
│   ├── rtsp_server.hpp         # RTSP 服务器实现
│   ├── rtsp_session.hpp        # RTP 会话管理
│   ├── hls_muxer.hpp           # HLS 复用器
│   ├── http_server.hpp         # HTTP 服务器
│   ├── stream_converter.hpp    # 拉流转换器（旧版本）
│   └── webrtc_sender.hpp       # WebRTC 发送器接口
└── src/
    ├── media_server_main_v2.cpp # 主程序入口
    ├── media_server.cpp         # 媒体服务器逻辑
    ├── rtsp_server.cpp          # RTSP 协议实现
    ├── rtsp_session.cpp         # RTP 接收实现
    ├── hls_muxer.cpp            # HLS 复用器实现
    ├── http_server.cpp          # HTTP 服务器实现
    └── webrtc_sender_stub.cpp   # WebRTC 空实现
```

## 依赖

- CMake >= 3.16
- FFmpeg (libavformat, libavcodec, libavutil)
- C++17 编译器（GCC 9+ 或 Clang 10+）

### Ubuntu/Debian 安装依赖

```bash
sudo apt-get update
sudo apt-get install -y cmake g++ pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev
```

## 编译

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译成功后生成可执行文件 `media_server_v2`。

## 使用方法

### 启动媒体服务器

```bash
./build/media_server_v2 --rtsp-port 8555 --hls-dir ./hls_output --http-port 8080
```

### 命令行选项

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `--rtsp-port <port>` | RTSP 端口 | 8554 |
| `--hls-dir <dir>` | HLS 输出目录 | ./hls_output |
| `--http-port <port>` | HTTP 端口 | 8080 |
| `--no-http` | 禁用 HTTP 服务器 | - |
| `--no-hls` | 禁用 HLS 输出 | - |

### 推流

使用 FFmpeg 推流到服务器：

```bash
# 本地文件推流
ffmpeg -re -i input.mp4 -c copy -f rtsp rtsp://localhost:8555/live/mystream

# 摄像头推流（Linux v4l2）
ffmpeg -re -f v4l2 -i /dev/video0 -c:v libx264 -f rtsp rtsp://localhost:8555/live/camera
```

### 播放 HLS

#### 1. 使用内置播放器（推荐）

浏览器访问：`http://localhost:8080/player_v2.html`

输入流路径（如 `live/mystream`）并点击播放。

#### 2. 使用 VLC 播放

```bash
vlc http://localhost:8080/live_mystream/playlist.m3u8
```

#### 3. 使用 nginx 提供 HLS

```nginx
server {
    listen 80;
    location /hls/ {
        root /path/to/hls_output;
        add_header Cache-Control no-cache;
        types {
            application/vnd.apple.mpegurl m3u8;
            video/mp2t ts;
        }
    }
}
```

## 技术架构

```
┌─────────────┐
│ FFmpeg      │ 推流客户端
└──────┬──────┘
       │ RTSP
       ▼
┌─────────────────┐
│  RTSP 服务器     │ 端口 8555
│  - ANNOUNCE     │
│  - SETUP        │
│  - RECORD       │
└──────┬──────────┘
       │ RTP
       ▼
┌─────────────────┐
│  RTSP Session   │ RTP 接收和解包
│  - H.264 解包    │
│  - SPS/PPS 提取  │
└──────┬──────────┘
       │
       ▼
┌─────────────────┐
│  MediaServer    │ 流管理和分发
└──────┬──────────┘
       │
       ├──────────────┬──────────────┐
       ▼              ▼              ▼
┌─────────────┐ ┌───────────┐ ┌──────────┐
│ HLS Muxer   │ │ HTTP      │ │ WebRTC   │
│ (FFmpeg)    │ │ Server    │ │ (Stub)   │
└──────┬──────┘ └─────┬─────┘ └────┬─────┘
       │              │            │
       ▼              ▼            ▼
┌──────────────┐ ┌──────────┐ ┌──────────┐
│ m3u8 + ts    │ │ 网页播放 │ │ 待实现   │
└──────────────┘ └──────────┘ └──────────┘
```

## 常见问题

### 1. 编译失败：找不到 FFmpeg

确保已安装 FFmpeg 开发包：

```bash
pkg-config --modversion libavformat  # 应输出版本号
```

### 2. HLS 播放卡顿

- 增加切片时长（修改 HLSMuxer 构造函数参数）
- 增加播放列表大小
- 检查网络带宽

### 3. RTSP 连接失败

- 检查防火墙端口（默认 8555）
- 确认服务器已启动
- 检查推流地址是否正确

### 4. 播放器显示"Found no media in fragment"

确保推流使用 `-c copy` 参数，不要重新编码：

```bash
ffmpeg -re -i input.mp4 -c copy -f rtsp rtsp://localhost:8555/live/mystream
```

## WebRTC 集成指南

当前项目提供了 WebRTC 发送器的**接口定义**（`include/webrtc_sender.hpp`），但默认实现是空的（`src/webrtc_sender_stub.cpp`）。

### 实现步骤

1. **选择 WebRTC 库**
   - [libdatachannel](https://github.com/paullouisageneau/libdatachannel)：轻量级 C++17 WebRTC 库（推荐）
   - [libwebrtc](https://webrtc.org/)：官方 C++ 实现，功能完整但体积大

2. **创建实现文件**
   
   在 `src/webrtc_sender_impl.cpp` 中实现 `IWebRTCSender` 接口。

3. **修改 CMakeLists.txt**
   
   将 `webrtc_sender_stub.cpp` 替换为你的实现文件。

## 许可证

本项目采用 MIT 许可证。详见 [LICENSE](LICENSE) 文件。

## 贡献

欢迎提交 Issue 和 Pull Request！