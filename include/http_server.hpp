#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace rtsp_rtmp2webrtc_hls {

struct HTTPServerConfig {
  int port = 8080;
  std::string bind_address = "0.0.0.0";
  std::string root_dir = "./hls_output";
};

class HTTPServer {
public:
  using LogCallback = std::function<void(const std::string&)>;
  
  explicit HTTPServer(HTTPServerConfig config);
  ~HTTPServer();
  
  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  bool start();
  void stop();
  bool is_running() const { return running_.load(); }

private:
  void server_loop();
  void handle_client(int client_socket, const std::string& client_ip);
  void send_response(int client_socket, int status_code, 
                    const std::string& content_type, const std::string& body);
  void send_file(int client_socket, const std::string& filepath);
  std::string get_mime_type(const std::string& filepath);
  void log(const std::string& msg) const;
  
  HTTPServerConfig config_;
  LogCallback log_cb_;
  std::atomic<bool> running_{false};
  std::thread server_thread_;
  int server_socket_ = -1;
};

}  // namespace rtsp_rtmp2webrtc_hls
