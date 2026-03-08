#include "stream_converter.hpp"
#include "webrtc_sender.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void on_signal(int signum) {
  std::cout << "\n收到信号：" << signum << "，准备退出..." << std::endl;
  g_running.store(false);
}

void print_usage(const char* prog) {
  std::cout << "用法：" << prog << " [选项] <输入 URL> <HLS 输出目录>\n"
            << "\n"
            << "选项:\n"
            << "  -h, --help     显示帮助信息\n"
            << "  -s, --segment  HLS 切片时长（秒），默认：2\n"
            << "  -l, --list     HLS 播放列表大小，默认：5\n"
            << "  -i, --id       流 ID（用于 WebRTC），默认：live/stream1\n"
            << "\n"
            << "示例:\n"
            << "  " << prog << " rtsp://192.168.1.100/stream ./hls_output\n"
            << "  " << prog << " rtmp://live.example.com/app/stream ./hls\n"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  // 设置信号处理
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // 默认参数
  std::string input_url;
  std::string hls_output_dir = "./hls_output";
  int hls_segment_duration = 2;
  int hls_list_size = 5;
  std::string stream_id = "live/stream1";

  // 解析命令行参数
  int arg_idx = 1;
  while (arg_idx < argc) {
    std::string arg = argv[arg_idx];

    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "-s" || arg == "--segment") {
      if (arg_idx + 1 >= argc) {
        std::cerr << "错误：-s 需要参数\n";
        return 1;
      }
      hls_segment_duration = std::stoi(argv[++arg_idx]);
    } else if (arg == "-l" || arg == "--list") {
      if (arg_idx + 1 >= argc) {
        std::cerr << "错误：-l 需要参数\n";
        return 1;
      }
      hls_list_size = std::stoi(argv[++arg_idx]);
    } else if (arg == "-i" || arg == "--id") {
      if (arg_idx + 1 >= argc) {
        std::cerr << "错误：-i 需要参数\n";
        return 1;
      }
      stream_id = argv[++arg_idx];
    } else if (arg[0] != '-') {
      // 位置参数
      if (input_url.empty()) {
        input_url = arg;
      } else if (hls_output_dir == "./hls_output") {
        hls_output_dir = arg;
      }
    } else {
      std::cerr << "未知选项：" << arg << "\n";
      print_usage(argv[0]);
      return 1;
    }

    ++arg_idx;
  }

  // 检查必需参数
  if (input_url.empty()) {
    std::cerr << "错误：请提供输入 URL\n";
    print_usage(argv[0]);
    return 1;
  }

  // 创建 HLS 输出目录
  std::cout << "HLS 输出目录：" << hls_output_dir << std::endl;

  // 配置转换器
  rtsp_rtmp2webrtc_hls::StreamConverter::Config config;
  config.input_url = input_url;
  config.hls_output_dir = hls_output_dir;
  config.hls_playlist_name = "playlist.m3u8";
  config.hls_segment_duration_sec = hls_segment_duration;
  config.hls_list_size = hls_list_size;
  config.stream_id = stream_id;

  // 创建转换器
  rtsp_rtmp2webrtc_hls::StreamConverter converter(config);

  // 设置日志回调
  converter.set_log_callback([](const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::cout << "[" << time << "] " << msg << std::endl;
  });

  // 创建并设置 WebRTC 发送器（当前是空实现）
  auto webrtc_sender = rtsp_rtmp2webrtc_hls::create_webrtc_sender();
  if (webrtc_sender) {
    webrtc_sender->add_stream(stream_id);
    converter.set_webrtc_sender(std::move(webrtc_sender));
    std::cout << "WebRTC 发送器已初始化（当前为空实现）\n";
  }

  // 启动转换器
  std::cout << "\n启动拉流...\n"
            << "  输入 URL: " << input_url << "\n"
            << "  HLS 输出：" << hls_output_dir << "/playlist.m3u8\n"
            << "  流 ID: " << stream_id << "\n"
            << "  切片时长：" << hls_segment_duration << "秒\n"
            << "  播放列表大小：" << hls_list_size << "\n"
            << "\n按 Ctrl+C 停止\n"
            << std::endl;

  if (!converter.start()) {
    std::cerr << "启动失败\n";
    return 1;
  }

  // 主循环：等待退出信号
  while (g_running.load() && converter.is_running()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  // 停止转换器
  converter.stop();

  std::cout << "\n已退出\n";
  return 0;
}
