# avtrans

一个从 `RTSP/RTMP` 接入，对外提供 `WebRTC` 的最小骨架工程，目标是：

- 尽量少依赖第三方业务库
- 媒体接入只依赖 `FFmpeg`
- WebRTC 发布只依赖 `Google libwebrtc`
- 便于继续演进成可商用的自建媒体网关

当前仓库更偏“学习和工程骨架”，不是现成可商用成品。它把后续真正商用时最关键的模块边界先搭出来，方便你继续补全。

## 目录

- `src/ffmpeg_ingest.*`
  - RTSP/RTMP 输入
  - 基于 FFmpeg 拉流、解复用、读取音视频包
- `src/webrtc_bridge.*`
  - WebRTC 发布抽象
  - 已接入原生 `libwebrtc` 的 `PeerConnectionFactory` / `PeerConnection` / `VideoTrackSource`
- `src/signaling_server.*`
  - 极简 HTTP 服务
  - 提供静态页面和 `/healthz`
  - `/offer` 目前是接口预留
- `web/index.html`
  - 浏览器端 WebRTC 播放页示例
- `docs/architecture.md`
  - 架构、码流路径、商用注意事项

## 为什么这样拆

推荐把整个系统拆成 4 层：

1. 接入层
   - 用 FFmpeg 负责 `RTSP/RTMP` 拉流和解复用
2. 媒体桥接层
   - 判断输入是不是浏览器可接受的编码
   - 优先做“转封装/重打时间戳”
   - 只有必要时才转码
3. WebRTC 发布层
   - 用原生 `libwebrtc` 建立 `PeerConnection`
   - 将音视频帧喂给自定义 `VideoTrackSource` / `AudioDeviceModule` 或对应 sender
4. 信令和控制层
   - 交换 SDP / ICE
   - 做鉴权、流选择、会话控制

这样做的原因是：真正商用时，问题通常不在“能不能播”，而在“出问题时你能不能定位、扩展、计费、鉴权、限流、录制和运维”。

## 当前能力

当前代码已经包含：

- 一个可编译的 C++ 工程骨架
- FFmpeg 拉流、解码视频并转成 I420 帧
- 极简 HTTP 服务
- 原生 `libwebrtc` 视频发布实现
- 浏览器播放页面模板
- 中文设计文档

当前没有完全打通的部分：

- ICE/STUN/TURN
- 音频链路
- 生产级信令协议
- 更完整的转码/转封装策略

## 构建

### 只验证工程骨架

```bash
cmake -S . -B build -DENABLE_FFMPEG=OFF -DENABLE_WEBRTC=OFF
cmake --build build -j
```

### 启用 FFmpeg

系统需要安装 FFmpeg 开发包，例如 Ubuntu:

```bash
sudo apt-get install -y \
  pkg-config \
  libavformat-dev \
  libavcodec-dev \
  libavutil-dev \
  libswresample-dev \
  libswscale-dev
```

然后：

```bash
cmake -S . -B build -DENABLE_FFMPEG=ON -DENABLE_WEBRTC=OFF
cmake --build build -j
```

### 启用原生 libwebrtc

你需要本机先准备好完整 `libwebrtc` 源码树和编译产物。常见方式是自己编译一个单体库，然后传给 CMake：

```bash
cmake -S . -B build \
  -DENABLE_FFMPEG=ON \
  -DENABLE_WEBRTC=ON \
  -DWEBRTC_ROOT=/path/to/webrtc/src \
  -DWEBRTC_LIBRARY=/path/to/libwebrtc.a \
  -DWEBRTC_EXTRA_LIBS="-lpthread -ldl"
cmake --build build -j
```

说明：

- `WEBRTC_ROOT` 一般指向 `webrtc/src`
- `WEBRTC_LIBRARY` 指向你实际产出的静态库或动态库
- `WEBRTC_EXTRA_LIBS` 取决于你的 libwebrtc 构建方式，可能还需要 `X11`、`GL`、`rt`、`pthread` 等系统库

运行：

```bash
./build/avtrans --listen 0.0.0.0 --port 8080 --input rtsp://example.com/live/stream
```

打开：

```text
http://127.0.0.1:8080/
```

## 真正商用时建议的技术路线

如果目标是“能商用”，建议按下面的顺序做，而不是一开始就追求大而全：

1. 先打通单路 RTSP H264 到单浏览器 WebRTC
2. 再补信令、鉴权、STUN/TURN
3. 再补多路并发、转码、录制、监控
4. 最后再考虑集群和调度

## 许可和商用

大方向上：

- `FFmpeg` 可以商用，但要严格看你启用的配置和链接方式
- `libwebrtc` 通常可商用，但你需要核对其依赖和分发要求
- 真正敏感的是：
  - 是否启用了 GPL 组件
  - 是否引入了 x264/x265/fdk-aac 之类额外编解码器
  - 你的二进制分发方式是否触发对应许可证要求

这部分在 `docs/architecture.md` 里有更具体的提醒，但它不是法律意见，正式上线前建议让法务做一次许可证审计。
