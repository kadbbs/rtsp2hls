#include "webrtc_sender.hpp"

#include <iostream>

namespace rtsp_rtmp2webrtc_hls {

/// @brief 空实现的 WebRTC 发送器
/// 用于项目编译通过，实际使用时请替换为真正的实现
class WebRTCSenderStub : public IWebRTCSender {
public:
  bool add_stream(const std::string& stream_id) override {
    std::cout << "[WebRTC Stub] 添加流：" << stream_id << std::endl;
    return true;
  }

  void remove_stream(const std::string& stream_id) override {
    std::cout << "[WebRTC Stub] 移除流：" << stream_id << std::endl;
  }

  bool push_video_frame(const std::string& stream_id,
                        const EncodedVideoFrame& frame) override {
    // 空实现：不发送任何数据
    // 实际实现时，这里应该将帧数据通过 WebRTC 发送出去
    (void)stream_id;
    (void)frame;
    return true;
  }
};

std::unique_ptr<IWebRTCSender> create_webrtc_sender() {
  return std::make_unique<WebRTCSenderStub>();
}

}  // namespace rtsp_rtmp2webrtc_hls
