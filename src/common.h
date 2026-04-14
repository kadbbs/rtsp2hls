#pragma once

#include <atomic>
#include <ctime>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace avtrans {

struct PacketInfo {
  enum class Kind {
    kUnknown,
    kAudio,
    kVideo
  };

  Kind kind = Kind::kUnknown;
  int stream_index = -1;
  int64_t pts = 0;
  int64_t dts = 0;
  int64_t duration = 0;
  bool key_frame = false;
  std::vector<uint8_t> payload;
};

struct VideoFrameInfo {
  int width = 0;
  int height = 0;
  int64_t timestamp_us = 0;
  std::vector<uint8_t> data_y;
  std::vector<uint8_t> data_u;
  std::vector<uint8_t> data_v;
  int stride_y = 0;
  int stride_u = 0;
  int stride_v = 0;
};

struct SdpAnswerResult {
  bool ok = false;
  std::string status = "500 Internal Server Error";
  std::string content_type = "application/json";
  std::string body;
};

struct AppConfig {
  std::string listen_host = "0.0.0.0";
  int port = 8080;
  std::string input_url;
};

inline std::string NowString() {
  const std::time_t now = std::time(nullptr);
  std::tm tm {};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

inline void Log(const std::string& line) {
  std::fprintf(stderr, "[%s] %s\n", NowString().c_str(), line.c_str());
}

}  // namespace avtrans
