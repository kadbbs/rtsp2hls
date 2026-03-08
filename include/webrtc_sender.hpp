#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtsp_rtmp2webrtc_hls {

/// @brief 编码视频帧结构
/// 用于从拉流线程传递视频数据到 WebRTC 发送器
struct EncodedVideoFrame {
  const uint8_t* data = nullptr;   ///< 帧数据指针
  size_t size = 0;                  ///< 数据大小（字节）
  bool key_frame = false;           ///< 是否关键帧（I 帧）
  int64_t pts_ms = 0;               ///< 显示时间戳（毫秒）
  const char* codec = "h264";       ///< 编码格式："h264", "h265"
};

/// @brief WebRTC 发送器接口
/// 你可以在项目中实现此接口来集成 libwebrtc 或其他 WebRTC 库
class IWebRTCSender {
public:
  virtual ~IWebRTCSender() = default;

  /// @brief 添加一路推流
  /// @param stream_id 流 ID，例如 "live/cam1"
  /// @return 成功返回 true
  virtual bool add_stream(const std::string& stream_id) = 0;

  /// @brief 移除一路推流
  /// @param stream_id 流 ID
  virtual void remove_stream(const std::string& stream_id) = 0;

  /// @brief 推送一帧视频到指定流
  /// @param stream_id 流 ID
  /// @param frame 编码视频帧
  /// @return 成功返回 true
  virtual bool push_video_frame(const std::string& stream_id,
                                const EncodedVideoFrame& frame) = 0;

  /// @brief 初始化发送器（可选）
  /// 可以在这里初始化底层库（如 libwebrtc）
  virtual bool initialize() { return true; }

  /// @brief 关闭发送器（可选）
  virtual void shutdown() {}
};

/// @brief 创建 WebRTC 发送器
/// @return WebRTC 发送器智能指针
///
/// 使用说明：
/// 1. 当前返回空实现（stub），用于编译通过
/// 2. 你需要在 src/webrtc_sender_impl.cpp 中实现真正的 WebRTC 发送逻辑
/// 3. 实现后替换 CMakeLists.txt 中的 webrtc_sender_stub.cpp 为你的实现文件
///
/// 实现建议：
/// - 使用 libwebrtc：创建 PeerConnection，通过 VideoTrackSource 注入编码帧
/// - 或使用其他 C++ WebRTC 库（如 libdatachannel）
/// - 信令（SDP/ICE）可通过 HTTP/WebSocket 服务处理
std::unique_ptr<IWebRTCSender> create_webrtc_sender();

}  // namespace rtsp_rtmp2webrtc_hls
