#include "signaling_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>

namespace avtrans {

namespace {
constexpr const char* kIndexPath = "web/index.html";
}

SignalingServer::SignalingServer(WebRtcBridge& bridge) : bridge_(bridge) {}

SignalingServer::~SignalingServer() {
  Stop();
}

bool SignalingServer::Start(const std::string& listen_host, int port) {
  if (running_.exchange(true)) {
    return false;
  }
  worker_ = std::thread(&SignalingServer::Run, this, listen_host, port);
  return true;
}

void SignalingServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (listen_fd_ >= 0) {
    shutdown(listen_fd_, SHUT_RDWR);
    close(listen_fd_);
    listen_fd_ = -1;
  }

  if (worker_.joinable()) {
    worker_.join();
  }
}

void SignalingServer::Run(std::string listen_host, int port) {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    Log("socket failed: " + std::string(std::strerror(errno)));
    running_ = false;
    return;
  }

  int opt = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, listen_host.c_str(), &addr.sin_addr) != 1) {
    Log("invalid listen host: " + listen_host);
    close(listen_fd_);
    listen_fd_ = -1;
    running_ = false;
    return;
  }

  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    Log("bind failed: " + std::string(std::strerror(errno)));
    close(listen_fd_);
    listen_fd_ = -1;
    running_ = false;
    return;
  }

  if (listen(listen_fd_, 16) < 0) {
    Log("listen failed: " + std::string(std::strerror(errno)));
    close(listen_fd_);
    listen_fd_ = -1;
    running_ = false;
    return;
  }

  Log("HTTP signaling server listening on " + listen_host + ":" + std::to_string(port));

  while (running_) {
    int client_fd = accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (running_) {
        Log("accept failed: " + std::string(std::strerror(errno)));
      }
      continue;
    }
    HandleClient(client_fd);
    close(client_fd);
  }
}

void SignalingServer::HandleClient(int client_fd) {
  char buf[8192];
  const ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) {
    return;
  }
  buf[n] = '\0';
  std::string req(buf, static_cast<size_t>(n));

  const bool is_root = req.rfind("GET / ", 0) == 0 || req.rfind("GET /HTTP", 0) == 0;
  const bool is_health = req.rfind("GET /healthz ", 0) == 0;
  const bool is_offer = req.rfind("POST /offer ", 0) == 0;

  std::string resp;
  if (is_root) {
    resp = BuildHttpResponse("200 OK", "text/html; charset=utf-8", ReadFile(kIndexPath));
  } else if (is_health) {
    resp = BuildHttpResponse("200 OK", "application/json", "{\"ok\":true}\n");
  } else if (is_offer) {
    const auto body_pos = req.find("\r\n\r\n");
    const std::string offer = body_pos == std::string::npos ? "" : req.substr(body_pos + 4);
    const SdpAnswerResult answer = bridge_.CreateAnswer(offer);
    resp = BuildHttpResponse(answer.status, answer.content_type, answer.body);
  } else {
    resp = BuildHttpResponse("404 Not Found", "text/plain; charset=utf-8", "not found\n");
  }

  send(client_fd, resp.data(), resp.size(), 0);
}

std::string SignalingServer::ReadFile(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs) {
    return "<html><body><h1>avtrans</h1><p>missing web/index.html</p></body></html>";
  }
  return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

std::string SignalingServer::BuildHttpResponse(const std::string& status,
                                               const std::string& content_type,
                                               const std::string& body) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << body;
  return oss.str();
}

}  // namespace avtrans
