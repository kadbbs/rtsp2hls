#pragma once

#include "common.h"

namespace avtrans {

class FFmpegIngest {
 public:
  using PacketHandler = std::function<void(const PacketInfo&)>;
  using VideoFrameHandler = std::function<void(const VideoFrameInfo&)>;

  FFmpegIngest();
  ~FFmpegIngest();

  bool Start(const std::string& input_url, PacketHandler packet_handler);
  bool Start(const std::string& input_url,
             PacketHandler packet_handler,
             VideoFrameHandler video_handler);
  void Stop();

 private:
  void Run(std::string input_url,
           PacketHandler packet_handler,
           VideoFrameHandler video_handler);

  std::atomic<bool> running_ {false};
  std::thread worker_;
};

}  // namespace avtrans
