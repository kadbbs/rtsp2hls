#pragma once

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <atomic>
#include <mutex>
#include <map>
#include <thread>

#include "webrtc_sender.hpp"

namespace rtsp_rtmp2webrtc_hls {

// 前向声明
class RTSPServer;

// 媒体流信息
struct MediaStream {
  std::string stream_path;      // 流路径，如 "live/mystream"
  bool has_video = false;
  bool has_audio = false;
  std::string video_codec;      // "h264", "h265"
  std::string audio_codec;      // "aac", "opus"
  int width = 0;
  int height = 0;
  int fps = 0;
  int64_t bitrate = 0;
};

// 编码帧
struct EncodedFrame {
  std::vector<uint8_t> data;
  int64_t pts = 0;
  int64_t dts = 0;
  bool is_key_frame = false;
  bool is_video = false;
};

// 帧回调
using FrameCallback = std::function<void(const std::string& stream_path, const EncodedFrame& frame)>;


// 媒体服务器（RTSP + RTMP）
class MediaServer {
public:
  using LogCallback = std::function<void(const std::string&)>;

  struct Config {
    int rtsp_port = 8554;
    int rtmp_port = 1935;
    std::string hls_output_dir = "./hls_output";
    bool enable_rtsp = true;
    bool enable_rtmp = true;
    bool enable_hls = true;
  };

  explicit MediaServer(Config config);
  ~MediaServer();

  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  void set_webrtc_sender(std::unique_ptr<IWebRTCSender> sender) {
    webrtc_sender_ = std::move(sender);
  }

  bool start();
  void stop();
  bool is_running() const { return running_.load(); }

  // 获取当前活跃的流列表
  std::vector<MediaStream> get_active_streams() const;

private:
  bool start_rtsp_server();
  bool start_rtmp_server();
  void stop_servers();

  // 当新流到达时调用
  void on_stream_started(const std::string& stream_path);
  void on_stream_stopped(const std::string& stream_path);

  // 当收到帧时调用
  void on_video_frame(const std::string& stream_path, const EncodedFrame& frame);
  void on_audio_frame(const std::string& stream_path, const EncodedFrame& frame);
  
  // RTP 帧回调（新）
  void on_video_frame_rtp(const std::string& stream_path, 
                          const std::vector<uint8_t>& data,
                          int64_t pts, bool is_key_frame);
  
  // SPS/PPS 回调（新）
  void on_sps_pps_received(const std::string& stream_path,
                           const std::vector<uint8_t>& sps,
                           const std::vector<uint8_t>& pps);

  void log(const std::string& msg) const;

  Config config_;
  LogCallback log_cb_;
  std::atomic<bool> running_{false};

  std::unique_ptr<IWebRTCSender> webrtc_sender_;

  // RTSP 服务器实例
  std::unique_ptr<RTSPServer> rtsp_server_;
  
  // HLS 复用器
  std::unique_ptr<class HLSMuxer> hls_muxer_;

  // 活跃的流
  mutable std::mutex streams_mutex_;
  std::map<std::string, MediaStream> active_streams_;

  // 服务器线程
  std::thread rtsp_thread_;
  std::thread rtmp_thread_;
  std::thread hls_thread_;
};

}  // namespace rtsp_rtmp2webrtc_hls
