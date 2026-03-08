#pragma once

#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

namespace rtsp_rtmp2webrtc_hls {

struct HLSOutputConfig {
  std::string stream_path;
  std::string output_dir;
  int segment_duration = 2;
  int playlist_size = 5;
};

class HLSOutput {
public:
  using LogCallback = std::function<void(const std::string&)>;
  
  explicit HLSOutput(HLSOutputConfig config);
  ~HLSOutput();
  
  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  bool start();
  void stop();
  bool is_running() const { return running_.load(); }
  std::string get_playlist_path() const;

private:
  std::string get_stream_filename() const;
  void ffmpeg_thread();
  void log(const std::string& msg) const;
  
  HLSOutputConfig config_;
  LogCallback log_cb_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  int ffmpeg_pid_ = -1;
};

}  // namespace rtsp_rtmp2webrtc_hls
