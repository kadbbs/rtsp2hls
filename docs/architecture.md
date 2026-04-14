# 架构说明

## 1. 目标

做一个媒体网关：

- 输入：`RTSP` / `RTMP`
- 输出：`WebRTC`
- 业务依赖尽量少
- 核心只用：
  - `FFmpeg`
  - `libwebrtc`

## 2. 最小可行数据路径

```text
RTSP/RTMP Source
      |
      v
  FFmpeg demux
      |
      v
Codec/Timebase normalize
      |
      v
WebRTC Track Sender
      |
      v
 Browser PeerConnection
```

## 3. 为什么“尽量不使用第三方库”仍然不能等于“零依赖”

要对浏览器输出 WebRTC，至少还需要这些能力：

- SDP 交换
- ICE candidate 交换
- STUN/TURN
- DTLS/SRTP
- RTP/RTCP

如果你不用现成媒体服务器，这些里最现实的做法就是：

- 媒体面直接依赖 `libwebrtc`
- 控制面自己写最小版 HTTP 信令

这样“第三方业务库”基本没有，但不是“什么都不用”。

## 4. 编码兼容性建议

浏览器对 WebRTC 的兼容有现实限制，商用时要特别注意。

### 视频

优先顺序：

1. H.264 Baseline / Constrained Baseline
2. VP8

说明：

- 如果 RTSP/RTMP 输入本身就是 H.264，优先尝试不转码
- 但要检查 profile、level、B 帧、时间戳、关键帧间隔
- 有些 IPC 或 NVR 的 H.264 码流并不适合直接喂浏览器

### 音频

优先顺序：

1. Opus
2. AAC 仅作为接入格式，不建议直接作为 WebRTC 输出主路线

原因：

- 浏览器 WebRTC 对 Opus 最稳
- 如果输入是 AAC，商用系统里通常要转成 Opus 再发 WebRTC

## 5. 你最应该先学懂的几个点

### FFmpeg 侧

- `avformat_open_input`
- `avformat_find_stream_info`
- `av_read_frame`
- 时间基 `time_base`
- PTS / DTS / duration
- Annex-B 和 AVCC 的差异
- AAC extradata / AudioSpecificConfig

### WebRTC 侧

- `PeerConnectionFactory`
- `PeerConnection`
- `MediaStreamTrack`
- RTP sender
- SDP offer / answer
- ICE / STUN / TURN
- NACK / PLI / FIR
- 抖动缓冲和码率控制

## 6. 商用品需要额外补的能力

### 必做

- 鉴权
- 流级别权限控制
- STUN/TURN
- 日志和指标
- 超时回收
- 多会话管理
- 异常重连

### 大概率要做

- 转码
- 录制
- 截图
- 水印
- 音视频同步修正
- 码流探测
- 黑屏/静音检测

## 7. 许可证提醒

这里不是法律意见，只是工程上要重点排查的地方。

### FFmpeg

要看：

- 你链接的是 `LGPL` 还是 `GPL` 组件
- 是否启用了 `--enable-gpl`
- 是否用了 `x264`、`x265`、`fdk-aac`

保守建议：

- 优先使用纯 `LGPL` 可接受方案
- 保留完整构建参数
- 上线前做 SBOM 和许可证清单

### libwebrtc

需要核对：

- 上游源码许可证
- 依赖项许可证
- 你自己的二进制分发和修改方式

## 8. 推荐演进路线

### 第一阶段

- 单路输入
- 单浏览器观看
- 内网环境
- 仅 H.264 + Opus

### 第二阶段

- 支持公网
- 加 STUN/TURN
- 加鉴权
- 加会话管理

### 第三阶段

- 支持多路并发
- 按需转码
- 监控报警
- 录制回放

