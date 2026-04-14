#pragma once

#include "common.h"
#include "webrtc_bridge.h"

namespace avtrans {

class SignalingServer {
 public:
  explicit SignalingServer(WebRtcBridge& bridge);
  ~SignalingServer();

  bool Start(const std::string& listen_host, int port);
  void Stop();

 private:
  void Run(std::string listen_host, int port);
  void HandleClient(int client_fd);
  static std::string ReadFile(const std::string& path);
  static std::string BuildHttpResponse(const std::string& status,
                                       const std::string& content_type,
                                       const std::string& body);

  WebRtcBridge& bridge_;
  std::atomic<bool> running_ {false};
  std::thread worker_;
  int listen_fd_ = -1;
};

}  // namespace avtrans

