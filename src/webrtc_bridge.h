#pragma once

#include "common.h"

struct avtrans_webrtc_handle;

namespace avtrans {

class WebRtcBridge {
 public:
  WebRtcBridge();
  ~WebRtcBridge();

  bool Init();
  bool PushPacket(const PacketInfo& packet);
  bool PushVideoFrame(const VideoFrameInfo& frame);
  SdpAnswerResult CreateAnswer(const std::string& remote_offer_sdp);

 private:
  std::atomic<uint64_t> audio_packets_ {0};
  std::atomic<uint64_t> video_packets_ {0};
#if AVTRANS_HAS_WEBRTC
  avtrans_webrtc_handle* handle_ = nullptr;
#endif
};

}  // namespace avtrans
