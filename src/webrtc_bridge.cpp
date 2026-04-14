#include "webrtc_bridge.h"

#include <cstdlib>
#include <utility>

#if AVTRANS_HAS_WEBRTC
#include "webrtc_shim.h"
#endif

namespace avtrans {

WebRtcBridge::WebRtcBridge() = default;

WebRtcBridge::~WebRtcBridge() {
#if AVTRANS_HAS_WEBRTC
  avtrans_webrtc_destroy(handle_);
  handle_ = nullptr;
#endif
}

bool WebRtcBridge::Init() {
#if AVTRANS_HAS_WEBRTC
  handle_ = avtrans_webrtc_create();
  if (handle_ == nullptr) {
    Log("Failed to create WebRTC shim handle.");
    return false;
  }
  if (!avtrans_webrtc_init(handle_)) {
    Log("Failed to initialize WebRTC shim.");
    avtrans_webrtc_destroy(handle_);
    handle_ = nullptr;
    return false;
  }
  Log("WebRTC bridge initialized via shim.");
  return true;
#else
  Log("WebRTC bridge is disabled. Rebuild with -DENABLE_WEBRTC=ON and provide libwebrtc.");
  return true;
#endif
}

bool WebRtcBridge::PushPacket(const PacketInfo& packet) {
  switch (packet.kind) {
    case PacketInfo::Kind::kAudio:
      ++audio_packets_;
      break;
    case PacketInfo::Kind::kVideo:
      ++video_packets_;
      break;
    case PacketInfo::Kind::kUnknown:
      break;
  }

#if AVTRANS_HAS_WEBRTC
  // In a real implementation, this is where H264/AAC access units would be
  // normalized, timestamp-mapped, and forwarded into libwebrtc senders/tracks.
  (void)packet;
  return true;
#else
  return true;
#endif
}

bool WebRtcBridge::PushVideoFrame(const VideoFrameInfo& frame) {
#if AVTRANS_HAS_WEBRTC
  if (handle_ == nullptr) {
    return false;
  }
  avtrans_webrtc_i420_frame raw {};
  raw.width = frame.width;
  raw.height = frame.height;
  raw.timestamp_us = frame.timestamp_us;
  raw.data_y = frame.data_y.data();
  raw.data_u = frame.data_u.data();
  raw.data_v = frame.data_v.data();
  raw.stride_y = frame.stride_y;
  raw.stride_u = frame.stride_u;
  raw.stride_v = frame.stride_v;
  return avtrans_webrtc_push_i420(handle_, &raw) != 0;
#else
  (void)frame;
  return false;
#endif
}

SdpAnswerResult WebRtcBridge::CreateAnswer(const std::string& remote_offer_sdp) {
#if AVTRANS_HAS_WEBRTC
  if (handle_ == nullptr) {
    SdpAnswerResult result;
    result.ok = false;
    result.status = "500 Internal Server Error";
    result.content_type = "application/json";
    result.body = "{\n  \"error\": \"WebRTC bridge is not initialized\"\n}\n";
    return result;
  }
  int ok = 0;
  const char* content_type = "application/json";
  char* body =
      avtrans_webrtc_create_answer(handle_, remote_offer_sdp.c_str(), &ok, &content_type);
  SdpAnswerResult result;
  result.ok = ok != 0;
  result.status = result.ok ? "200 OK" : "500 Internal Server Error";
  result.content_type = content_type;
  result.body = body != nullptr ? body : "";
  avtrans_webrtc_free_string(body);
  return result;
#else
  SdpAnswerResult result;
  result.ok = false;
  result.status = "501 Not Implemented";
  result.content_type = "application/json";
  std::ostringstream oss;
  oss << "{\n"
      << "  \"error\": \"WebRTC is not compiled in\",\n"
      << "  \"hint\": \"Build with -DENABLE_WEBRTC=ON and provide WEBRTC_ROOT/WEBRTC_LIBRARY.\",\n"
      << "  \"stats\": {\n"
      << "    \"audio_packets\": " << audio_packets_.load() << ",\n"
      << "    \"video_packets\": " << video_packets_.load() << "\n"
      << "  },\n"
      << "  \"offer_size\": " << remote_offer_sdp.size() << "\n"
      << "}\n";
  result.body = oss.str();
  return result;
#endif
}

}  // namespace avtrans
