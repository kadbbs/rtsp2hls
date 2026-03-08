#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstring>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <sys/stat.h>

namespace rtsp_rtmp2webrtc_hls {

// HTTP 服务器配置
struct HTTPServerConfig {
  int port = 8080;
  std::string bind_address = "0.0.0.0";
  std::string root_dir = "./hls_output";
};

// HTTP 服务器类
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
  void send_directory_listing(int client_socket, const std::string& dir_path);
  std::string get_mime_type(const std::string& filepath);
  void log(const std::string& msg) const;
  
  HTTPServerConfig config_;
  LogCallback log_cb_;
  std::atomic<bool> running_{false};
  std::thread server_thread_;
  int server_socket_ = -1;
};

HTTPServer::HTTPServer(HTTPServerConfig config)
    : config_(std::move(config)) {}

HTTPServer::~HTTPServer() {
  stop();
}

void HTTPServer::log(const std::string& msg) const {
  if (log_cb_) {
    log_cb_(msg);
  }
}

std::string HTTPServer::get_mime_type(const std::string& filepath) {
  // 转为小写进行比较
  std::string lower_path = filepath;
  for (char& c : lower_path) {
    c = tolower(c);
  }
  
  if (lower_path.size() >= 5 && lower_path.substr(lower_path.size() - 5) == ".m3u8") {
    return "application/vnd.apple.mpegurl; charset=utf-8";
  } else if (lower_path.size() >= 3 && lower_path.substr(lower_path.size() - 3) == ".ts") {
    return "video/mp2t";
  } else if (lower_path.size() >= 5 && lower_path.substr(lower_path.size() - 5) == ".html") {
    return "text/html; charset=utf-8";
  } else if (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".css") {
    return "text/css; charset=utf-8";
  } else if (lower_path.size() >= 3 && lower_path.substr(lower_path.size() - 3) == ".js") {
    return "application/javascript; charset=utf-8";
  } else if (lower_path.size() >= 5 && lower_path.substr(lower_path.size() - 5) == ".json") {
    return "application/json; charset=utf-8";
  }
  return "application/octet-stream";
}

void HTTPServer::send_response(int client_socket, int status_code,
                               const std::string& content_type, 
                               const std::string& body) {
  std::map<int, std::string> status_texts = {
    {200, "OK"},
    {404, "Not Found"},
    {403, "Forbidden"},
    {500, "Internal Server Error"}
  };
  
  std::ostringstream response;
  response << "HTTP/1.1 " << status_code << " " << status_texts[status_code] << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
           << "Connection: close\r\n"
           << "\r\n";
  
  std::string resp = response.str();
  send(client_socket, resp.c_str(), resp.size(), 0);
  if (!body.empty()) {
    send(client_socket, body.c_str(), body.size(), 0);
  }
}

void HTTPServer::send_file(int client_socket, const std::string& filepath) {
  std::ifstream file(filepath, std::ios::binary);
  if (!file) {
    send_response(client_socket, 404, "text/plain", "File not found");
    return;
  }
  
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();
  
  send_response(client_socket, 200, get_mime_type(filepath), content);
}

void HTTPServer::send_directory_listing(int client_socket, const std::string& dir_path) {
  std::ostringstream html;
  html << "<!DOCTYPE html>\n"
       << "<html><head><title>目录列表</title>"
       << "<meta charset='utf-8'></head>\n"
       << "<body><h1>📁 目录列表</h1>\n<ul>\n";
  
  // 返回目录链接（带完整路径）
  html << "<li><a href=\"/index.html\">📄 index.html</a></li>\n";
  html << "<li><a href=\"/player.html\">🎮 player.html</a></li>\n";
  html << "</ul></body></html>\n";
  
  send_response(client_socket, 200, "text/html", html.str());
}

void HTTPServer::handle_client(int client_socket, const std::string& client_ip) {
  char buffer[4096];
  int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
  
  if (bytes_read <= 0) {
    close(client_socket);
    return;
  }
  
  buffer[bytes_read] = '\0';
  std::string request(buffer);
  
  // 解析请求行
  std::istringstream iss(request);
  std::string method, path, version;
  iss >> method >> path >> version;
  
  // 处理 OPTIONS 预检请求
  if (method == "OPTIONS") {
    std::ostringstream response;
    response << "HTTP/1.1 204 No Content\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
             << "Access-Control-Allow-Headers: Content-Type\r\n"
             << "Content-Length: 0\r\n"
             << "\r\n";
    send(client_socket, response.str().c_str(), response.str().length(), 0);
    close(client_socket);
    return;
  }
  
  if (method != "GET") {
    send_response(client_socket, 405, "text/plain", "Method Not Allowed");
    close(client_socket);
    return;
  }
  
  log(client_ip + " 请求：" + path);
  
  // 安全检查：防止目录遍历攻击
  if (path.find("..") != std::string::npos) {
    send_response(client_socket, 403, "text/plain", "Forbidden");
    close(client_socket);
    return;
  }
  
  // 移除查询参数
  size_t query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    path = path.substr(0, query_pos);
  }
  
  // 构建文件路径
  std::string filepath;
  if (path == "/" || path.empty()) {
    filepath = config_.root_dir + "/index.html";
  } else {
    // 移除前导斜杠来构建路径
    std::string relative_path = (path[0] == '/') ? path.substr(1) : path;
    filepath = config_.root_dir + "/" + relative_path;
  }
  
  // 如果请求的是目录，尝试返回 index.html
  struct stat path_stat;
  if (stat(filepath.c_str(), &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
    if (filepath.back() != '/') {
      filepath += '/';
    }
    filepath += "index.html";
  }
  
  // 检查文件是否存在
  if (stat(filepath.c_str(), &path_stat) != 0) {
    // 文件不存在，返回 404
    send_response(client_socket, 404, "text/plain", "File not found: " + path);
    close(client_socket);
    return;
  }
  
  // 发送文件
  send_file(client_socket, filepath);
  close(client_socket);
}

void HTTPServer::server_loop() {
  // 创建 socket
  server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket_ < 0) {
    log("创建 socket 失败：" + std::string(strerror(errno)));
    return;
  }
  
  // 设置 socket 选项
  int opt = 1;
  setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  
  // 绑定地址
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(config_.port);
  
  if (config_.bind_address == "0.0.0.0") {
    addr.sin_addr.s_addr = INADDR_ANY;
  } else {
    inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr);
  }
  
  if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    log("绑定端口 " + std::to_string(config_.port) + " 失败：" + std::string(strerror(errno)));
    close(server_socket_);
    server_socket_ = -1;
    return;
  }
  
  // 开始监听
  if (listen(server_socket_, 10) < 0) {
    log("监听失败：" + std::string(strerror(errno)));
    close(server_socket_);
    server_socket_ = -1;
    return;
  }
  
  running_.store(true);
  log("HTTP 服务器已启动：" + config_.bind_address + ":" + std::to_string(config_.port));
  
  while (running_.load()) {
    struct pollfd pfd{};
    pfd.fd = server_socket_;
    pfd.events = POLLIN;
    
    int ret = poll(&pfd, 1, 1000);  // 1 秒超时
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    
    if (ret == 0) continue;
    
    // 接受新连接
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_socket < 0) {
      if (errno == EINTR) continue;
      continue;
    }
    
    std::string client_ip = inet_ntoa(client_addr.sin_addr);
    log("新 HTTP 连接：" + client_ip);
    
    // 在新线程中处理
    std::thread(&HTTPServer::handle_client, this, client_socket, client_ip).detach();
  }
  
  close(server_socket_);
  server_socket_ = -1;
}

bool HTTPServer::start() {
  if (running_.load()) {
    log("HTTP 服务器已经在运行");
    return false;
  }
  
  server_thread_ = std::thread(&HTTPServer::server_loop, this);
  return true;
}

void HTTPServer::stop() {
  if (!running_.load()) {
    return;
  }
  
  running_.store(false);
  
  if (server_socket_ >= 0) {
    shutdown(server_socket_, SHUT_RDWR);
    close(server_socket_);
    server_socket_ = -1;
  }
  
  if (server_thread_.joinable()) {
    server_thread_.join();
  }
  
  log("HTTP 服务器已停止");
}

}  // namespace rtsp_rtmp2webrtc_hls
