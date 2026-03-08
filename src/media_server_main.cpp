#include "media_server.hpp"
#include "webrtc_sender.hpp"
#include "hls_output.hpp"
#include "http_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>
#include <map>

namespace {

std::atomic<bool> g_running{true};

void on_signal(int signum) {
  std::cout << "\n收到信号：" << signum << "，准备退出..." << std::endl;
  g_running.store(false);
}

void print_usage(const char* prog) {
  std::cout << "用法：" << prog << " [选项]\n"
            << "\n"
            << "选项:\n"
            << "  -h, --help          显示帮助信息\n"
            << "  --rtsp-port <port>  RTSP 端口（默认：8554）\n"
            << "  --rtmp-port <port>  RTMP 端口（默认：1935）\n"
            << "  --hls-dir <dir>     HLS 输出目录（默认：./hls_output）\n"
            << "  --no-rtsp           禁用 RTSP 服务器\n"
            << "  --no-rtmp           禁用 RTMP 服务器\n"
            << "  --no-hls            禁用 HLS 输出\n"
            << "\n"
            << "示例:\n"
            << "  " << prog << "                          # 使用默认配置启动\n"
            << "  " << prog << " --rtsp-port 8555         # 自定义 RTSP 端口\n"
            << "  " << prog << " --hls-dir /var/www/hls   # 自定义 HLS 目录\n"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  // 设置信号处理
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // 默认配置
  rtsp_rtmp2webrtc_hls::MediaServer::Config config;
  config.rtsp_port = 8554;
  config.rtmp_port = 1935;
  config.hls_output_dir = "./hls_output";
  config.enable_rtsp = true;
  config.enable_rtmp = true;
  config.enable_hls = true;
  
  // HTTP 服务器配置
  int http_port = 8080;
  bool enable_http = true;

  // 解析命令行参数
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "--rtsp-port" && i + 1 < argc) {
      config.rtsp_port = std::stoi(argv[++i]);
    } else if (arg == "--rtmp-port" && i + 1 < argc) {
      config.rtmp_port = std::stoi(argv[++i]);
    } else if (arg == "--hls-dir" && i + 1 < argc) {
      config.hls_output_dir = argv[++i];
    } else if (arg == "--http-port" && i + 1 < argc) {
      http_port = std::stoi(argv[++i]);
    } else if (arg == "--no-http") {
      enable_http = false;
    } else if (arg == "--no-rtsp") {
      config.enable_rtsp = false;
    } else if (arg == "--no-rtmp") {
      config.enable_rtmp = false;
    } else if (arg == "--no-hls") {
      config.enable_hls = false;
    } else {
      std::cerr << "未知选项：" << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }
  }

  std::cout << "========================================\n";
  std::cout << "RTSP/RTMP 转 WebRTC/HLS 媒体服务器\n";
  std::cout << "========================================\n\n";

  // 创建媒体服务器
  auto server = std::make_unique<rtsp_rtmp2webrtc_hls::MediaServer>(config);

  // 设置日志回调
  server->set_log_callback([](const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::cout << "[" << time << "] " << msg << std::endl;
  });

  // 创建并设置 WebRTC 发送器（当前是空实现）
  auto webrtc_sender = rtsp_rtmp2webrtc_hls::create_webrtc_sender();
  if (webrtc_sender) {
    server->set_webrtc_sender(std::move(webrtc_sender));
    std::cout << "WebRTC 发送器已初始化（当前为空实现）\n";
  }

  // 启动服务器
  std::cout << "\n启动媒体服务器...\n";
  if (!server->start()) {
    std::cerr << "启动失败\n";
    return 1;
  }

  std::cout << "\n服务器配置:\n";
  std::cout << "  RTSP 端口：" << (config.enable_rtsp ? std::to_string(config.rtsp_port) : "禁用") << "\n";
  std::cout << "  RTMP 端口：" << (config.enable_rtmp ? std::to_string(config.rtmp_port) : "禁用") << "\n";
  std::cout << "  HLS 目录：" << (config.enable_hls ? config.hls_output_dir : "禁用") << "\n";
  std::cout << "\n推流地址示例:\n";
  if (config.enable_rtsp) {
    std::cout << "  FFmpeg 推流：ffmpeg -re -i input.mp4 -c copy -f rtsp rtsp://localhost:"
              << config.rtsp_port << "/live/mystream\n";
  }
  if (config.enable_rtmp) {
    std::cout << "  FFmpeg 推流：ffmpeg -re -i input.mp4 -c copy -f flv rtmp://localhost:"
              << config.rtmp_port << "/live/mystream\n";
  }
  std::cout << "\n按 Ctrl+C 停止\n" << std::endl;

  // 主循环：等待退出信号并显示状态
  while (g_running.load() && server->is_running()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // 每 10 秒显示一次状态
    static int counter = 0;
    if (++counter % 10 == 0) {
      auto streams = server->get_active_streams();
      if (!streams.empty()) {
        std::cout << "[状态] 活跃推流：" << streams.size() << std::endl;
        for (const auto& stream : streams) {
          std::cout << "  - " << stream.stream_path;
          if (stream.has_video) std::cout << " [视频]";
          if (stream.has_audio) std::cout << " [音频]";
          std::cout << std::endl;
        }
      }
    }
  }

  // 停止服务器
  server->stop();

  std::cout << "\n已退出\n";
  return 0;
}
