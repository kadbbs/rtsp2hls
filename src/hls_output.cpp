#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdlib>
#include <sys/wait.h>

namespace rtsp_rtmp2webrtc_hls {

// HLS 输出配置
struct HLSOutputConfig {
  std::string stream_path;          // 流路径
  std::string output_dir;           // 输出目录
  int segment_duration = 2;         // 切片时长（秒）
  int playlist_size = 5;            // 播放列表大小
};

// HLS 输出生成器（使用 FFmpeg 子进程）
class HLSOutput {
public:
  using LogCallback = std::function<void(const std::string&)>;
  
  explicit HLSOutput(HLSOutputConfig config);
  ~HLSOutput();
  
  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  
  // 启动 HLS 输出
  bool start();
  
  // 停止 HLS 输出
  void stop();
  
  // 是否正在运行
  bool is_running() const { return running_.load(); }
  
  // 获取播放列表路径
  std::string get_playlist_path() const {
    return config_.output_dir + "/" + get_stream_filename() + "/playlist.m3u8";
  }

private:
  std::string get_stream_filename() const;
  void ffmpeg_thread();
  void log(const std::string& msg) const;
  
  HLSOutputConfig config_;
  LogCallback log_cb_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  pid_t ffmpeg_pid_ = -1;
};

HLSOutput::HLSOutput(HLSOutputConfig config)
    : config_(std::move(config)) {}

HLSOutput::~HLSOutput() {
  stop();
}

std::string HLSOutput::get_stream_filename() const {
  // live/mystream -> live_mystream
  std::string filename = config_.stream_path;
  for (char& c : filename) {
    if (c == '/') c = '_';
  }
  return filename;
}

void HLSOutput::log(const std::string& msg) const {
  if (log_cb_) {
    log_cb_(msg);
  }
}

bool HLSOutput::start() {
  if (running_.load()) {
    log("HLS 输出已经在运行");
    return false;
  }
  
  // 创建输出目录
  std::string stream_dir = config_.output_dir + "/" + get_stream_filename();
  std::string mkdir_cmd = "mkdir -p " + stream_dir;
  system(mkdir_cmd.c_str());
  
  running_.store(true);
  thread_ = std::thread(&HLSOutput::ffmpeg_thread, this);
  
  log("HLS 输出已启动：" + get_playlist_path());
  return true;
}

void HLSOutput::stop() {
  if (!running_.load()) {
    return;
  }
  
  running_.store(false);
  
  // 停止 FFmpeg 进程
  if (ffmpeg_pid_ > 0) {
    kill(ffmpeg_pid_, SIGTERM);
    waitpid(ffmpeg_pid_, nullptr, 0);
    ffmpeg_pid_ = -1;
  }
  
  if (thread_.joinable()) {
    thread_.join();
  }
  
  log("HLS 输出已停止");
}

void HLSOutput::ffmpeg_thread() {
  std::string stream_url = "rtsp://localhost:" + 
                           std::to_string(8555) + "/" + 
                           config_.stream_path;
  
  std::string stream_dir = config_.output_dir + "/" + get_stream_filename();
  std::string playlist_path = stream_dir + "/playlist.m3u8";
  std::string segment_pattern = stream_dir + "/segment_%03d.ts";
  
  // 构建 FFmpeg 命令
  std::string cmd = "ffmpeg -y "
                    "-rtsp_transport tcp "
                    "-stimeout 5000000 "
                    "-i " + stream_url + " "
                    "-c copy "
                    "-f hls "
                    "-hls_time " + std::to_string(config_.segment_duration) + " "
                    "-hls_list_size " + std::to_string(config_.playlist_size) + " "
                    "-hls_flags delete_segments "
                    "-hls_segment_filename '" + segment_pattern + "' "
                    "'" + playlist_path + "' "
                    "2>&1";
  
  log("启动 FFmpeg: " + cmd);
  
  // 使用 popen 执行 FFmpeg
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    log("启动 FFmpeg 失败");
    running_.store(false);
    return;
  }
  
  // 获取 FFmpeg PID
  ffmpeg_pid_ = fileno(pipe);
  
  // 读取 FFmpeg 输出
  char buffer[256];
  while (running_.load() && fgets(buffer, sizeof(buffer), pipe)) {
    std::string line(buffer);
    // 过滤并输出重要信息
    if (line.find("error") != std::string::npos ||
        line.find("Error") != std::string::npos ||
        line.find("frame=") != std::string::npos ||
        line.find("speed=") != std::string::npos) {
      log("[FFmpeg] " + line);
    }
  }
  
  pclose(pipe);
  ffmpeg_pid_ = -1;
  
  if (running_.load()) {
    log("FFmpeg 进程结束，准备重启...");
    // 重启
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ffmpeg_thread();
  }
}

}  // namespace rtsp_rtmp2webrtc_hls
