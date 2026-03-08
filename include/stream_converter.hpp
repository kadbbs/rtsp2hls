#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include <thread>

// FFmpeg 前向声明
struct AVFormatContext;
struct AVPacket;

namespace rtsp_rtmp2webrtc_hls {

// WebRTC 发送器前向声明
class IWebRTCSender;

// 拉流（RTSP/RTMP）并输出 HLS 的转换器
class StreamConverter {
public:
  using LogCallback = std::function<void(const std::string&)>;

  struct Config {
    std::string input_url;           // rtsp://... 或 rtmp://...
    std::string hls_output_dir;      // HLS 输出目录（生成 .m3u8 和 .ts）
    std::string hls_playlist_name{"playlist.m3u8"};
    int hls_segment_duration_sec{2}; // 每个 TS 切片时长（秒）
    int hls_list_size{5};            // 播放列表保留切片数
    std::string stream_id{"live/stream1"}; // 用于 WebRTC 的流 ID
  };

  explicit StreamConverter(Config config);
  ~StreamConverter();

  // 设置日志回调
  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }

  // 设置 WebRTC 发送器（可选）
  void set_webrtc_sender(std::unique_ptr<IWebRTCSender> sender) {
    webrtc_sender_ = std::move(sender);
  }

  // 启动拉流
  bool start();

  // 停止拉流
  void stop();

  // 是否正在运行
  bool is_running() const { return running_.load(); }

private:
  bool open_input();
  bool open_hls_output();
  bool run_loop();
  void log(const std::string& msg) const;
  void cleanup();

  Config config_;
  LogCallback log_cb_;
  std::atomic<bool> running_{false};
  std::unique_ptr<IWebRTCSender> webrtc_sender_;

  // FFmpeg 上下文（使用原始指针，手动管理生命周期）
  AVFormatContext* input_ctx_ = nullptr;
  AVFormatContext* output_ctx_ = nullptr;
  AVPacket* packet_ = nullptr;


  // 输入流索引到输出流索引的映射
  std::vector<int> stream_index_map_;

  // 工作线程
  std::thread worker_;
};

}  // namespace rtsp_rtmp2webrtc_hls
