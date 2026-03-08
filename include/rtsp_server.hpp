#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <map>
#include <mutex>

namespace rtsp_rtmp2webrtc_hls {

// RTSP 服务器配置
struct RTSPServerConfig {
  int port = 8554;
  std::string bind_address = "0.0.0.0";
  int timeout_sec = 30;
};

// 媒体信息
struct MediaInfo {
  std::string stream_path;          // 流路径，如 "live/mystream"
  std::string sdp;                  // SDP 描述
  
  // 视频信息
  bool has_video = false;
  std::string video_codec;          // "h264", "h265"
  int width = 0;
  int height = 0;
  int fps = 0;
  std::vector<uint8_t> sps;         // H.264 SPS
  std::vector<uint8_t> pps;         // H.264 PPS
  std::vector<uint8_t> vps;         // H.265 VPS
  
  // 音频信息
  bool has_audio = false;
  std::string audio_codec;          // "aac", "opus"
  int sample_rate = 0;
  int channels = 0;
  std::vector<uint8_t> aac_config;  // AAC config
};

// 帧数据
struct FrameData {
  std::vector<uint8_t> data;
  int64_t pts = 0;
  int64_t dts = 0;
  bool is_key_frame = false;
  bool is_video = false;
};

// 回调类型
using NewStreamCallback = std::function<void(const std::string& stream_path)>;
using StreamClosedCallback = std::function<void(const std::string& stream_path)>;
using VideoFrameCallback = std::function<void(const std::string& stream_path, 
                                              const std::vector<uint8_t>& data,
                                              int64_t pts, bool is_key_frame)>;
using SpsPpsCallback = std::function<void(const std::string& stream_path,
                                          const std::vector<uint8_t>& sps,
                                          const std::vector<uint8_t>& pps)>;
using LogCallback = std::function<void(const std::string&)>;

// RTSP 服务器类
class RTSPServer {
public:
  explicit RTSPServer(RTSPServerConfig config = RTSPServerConfig());
  ~RTSPServer();

  // 设置回调
  void set_new_stream_callback(NewStreamCallback cb) { on_new_stream_ = std::move(cb); }
  void set_stream_closed_callback(StreamClosedCallback cb) { on_stream_closed_ = std::move(cb); }
  void set_video_frame_callback(VideoFrameCallback cb) { on_video_frame_ = std::move(cb); }
  void set_sps_pps_callback(SpsPpsCallback cb) { on_sps_pps_ = std::move(cb); }
  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }

  // 启动/停止
  bool start();
  void stop();
  bool is_running() const { return running_.load(); }

  // 获取活跃的流
  std::vector<std::string> get_active_streams() const;

  // 获取媒体信息
  MediaInfo get_media_info(const std::string& stream_path) const;

private:
  void server_loop();
  void handle_client(int client_socket, const std::string& client_ip);
  bool handle_rtsp_request(int client_socket, const std::string& request, 
                          std::string& stream_path, MediaInfo& media_info,
                          const std::string& client_ip);
  void send_rtsp_response(int client_socket, const std::string& response);
  std::string generate_sdp(const MediaInfo& media_info, const std::string& client_ip, int client_port);
  
  void log(const std::string& msg) const;

  RTSPServerConfig config_;
  std::atomic<bool> running_{false};
  std::thread server_thread_;
  int server_socket_ = -1;

  // 回调
  NewStreamCallback on_new_stream_;
  StreamClosedCallback on_stream_closed_;
  VideoFrameCallback on_video_frame_;
  SpsPpsCallback on_sps_pps_;
  LogCallback log_cb_;

  // 活跃的流
  mutable std::mutex streams_mutex_;
  std::map<std::string, MediaInfo> active_streams_;
  
  // 会话状态
  struct SessionState {
    std::string stream_path;
    int client_socket = -1;
    std::string client_ip;
    int rtp_port = 0;
    int rtcp_port = 0;
    uint32_t ssrc = 0;
    uint16_t seq = 0;
  };
  std::map<int, std::shared_ptr<SessionState>> sessions_;
  mutable std::mutex sessions_mutex_;
  
  // RTP 会话管理
  std::map<std::string, std::shared_ptr<class RTSPSession>> rtsp_sessions_;
  mutable std::mutex rtsp_sessions_mutex_;
};

}  // namespace rtsp_rtmp2webrtc_hls
