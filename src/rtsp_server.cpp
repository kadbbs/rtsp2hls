#include "rtsp_server.hpp"
#include "rtsp_session.hpp"

#include <cstring>
#include <sstream>
#include <iostream>
#include <regex>
#include <random>
#include <algorithm>

// Base64 解码表
static const int base64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

// Base64 解码
static std::vector<uint8_t> base64_decode(const std::string& src) {
    std::vector<uint8_t> result;
    int val = 0, valb = -8;
    for (char c : src) {
        if (c == '=') break;
        if (base64_table[(unsigned char)c] == -1) continue;
        val = (val << 6) + base64_table[(unsigned char)c];
        valb += 6;
        if (valb >= 0) {
            result.push_back((val >> valb) & 0xFF);
            valb -= 8;
        }
    }
    return result;
}

// 从 SDP 中提取 SPS/PPS
static bool extract_sps_pps_from_sdp(const std::string& sdp, 
                                     std::vector<uint8_t>& sps, 
                                     std::vector<uint8_t>& pps) {
    // 查找 sprop-parameter-sets
    auto pos = sdp.find("sprop-parameter-sets=");
    if (pos == std::string::npos) return false;
    
    pos += 21;  // 跳过 "sprop-parameter-sets="
    
    // 查找逗号分隔 SPS 和 PPS
    auto comma_pos = sdp.find(',', pos);
    if (comma_pos == std::string::npos) return false;
    
    std::string sps_b64 = sdp.substr(pos, comma_pos - pos);
    
    // 查找 PPS（可能有多个，取第一个）
    auto end_pos = sdp.find_first_of(";\r\n", comma_pos);
    std::string pps_b64;
    if (end_pos == std::string::npos) {
        pps_b64 = sdp.substr(comma_pos + 1);
    } else {
        pps_b64 = sdp.substr(comma_pos + 1, end_pos - comma_pos - 1);
    }
    
    // Base64 解码
    sps = base64_decode(sps_b64);
    pps = base64_decode(pps_b64);
    
    return !sps.empty() && !pps.empty();
}

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace rtsp_rtmp2webrtc_hls {

namespace {

// 生成随机 SSRC
uint32_t generate_ssrc() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dis;
  return dis(gen);
}

// 解析 RTSP 请求行
bool parse_request_line(const std::string& line, std::string& method, 
                       std::string& uri, std::string& version) {
  std::istringstream iss(line);
  return (iss >> method >> uri >> version) && !method.empty() && !uri.empty();
}

// 解析 RTSP 头
std::map<std::string, std::string> parse_headers(const std::string& data) {
  std::map<std::string, std::string> headers;
  std::istringstream iss(data);
  std::string line;
  
  while (std::getline(iss, line)) {
    // 移除 \r\n
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
      line.pop_back();
    }
    
    if (line.empty()) break;
    
    auto pos = line.find(':');
    if (pos != std::string::npos) {
      std::string key = line.substr(0, pos);
      std::string value = line.substr(pos + 1);
      
      // 去除前导空格
      while (!value.empty() && value[0] == ' ') {
        value.erase(0, 1);
      }
      
      headers[key] = value;
    }
  }
  
  return headers;
}

// 从 URL 提取流路径
std::string extract_stream_path(const std::string& uri) {
  // rtsp://192.168.1.5:8554/live/mystream -> live/mystream
  auto pos = uri.find("://");
  if (pos != std::string::npos) {
    pos = uri.find('/', pos + 3);
    if (pos != std::string::npos) {
      return uri.substr(pos + 1);
    }
  }
  return uri;
}

// 解析 transport 头获取客户端端口
int parse_client_port(const std::string& transport) {
  // client_port=3456-3457
  auto pos = transport.find("client_port=");
  if (pos != std::string::npos) {
    auto start = pos + 12;
    auto end = transport.find('-', start);
    if (end == std::string::npos) {
      end = transport.find(';', start);
    }
    if (end == std::string::npos) {
      end = transport.length();
    }
    try {
      return std::stoi(transport.substr(start, end - start));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

}  // namespace

RTSPServer::RTSPServer(RTSPServerConfig config)
    : config_(std::move(config)) {}

RTSPServer::~RTSPServer() {
  stop();
}

void RTSPServer::log(const std::string& msg) const {
  if (log_cb_) {
    log_cb_(msg);
  }
}

bool RTSPServer::start() {
  if (running_.load()) {
    log("RTSP 服务器已经在运行");
    return true;
  }

  // 创建 socket
  server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket_ < 0) {
    log("创建 socket 失败：" + std::string(strerror(errno)));
    return false;
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
    return false;
  }

  // 开始监听
  if (listen(server_socket_, 10) < 0) {
    log("监听失败：" + std::string(strerror(errno)));
    close(server_socket_);
    server_socket_ = -1;
    return false;
  }

  running_.store(true);
  server_thread_ = std::thread(&RTSPServer::server_loop, this);
  
  log("RTSP 服务器已启动：" + config_.bind_address + ":" + std::to_string(config_.port));
  return true;
}

void RTSPServer::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);
  
  // 关闭服务器 socket
  if (server_socket_ >= 0) {
    shutdown(server_socket_, SHUT_RDWR);
    close(server_socket_);
    server_socket_ = -1;
  }

  // 等待服务器线程结束
  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  // 关闭所有客户端连接
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [fd, session] : sessions_) {
      if (session->client_socket >= 0) {
        close(session->client_socket);
      }
    }
    sessions_.clear();
  }

  log("RTSP 服务器已停止");
}

std::vector<std::string> RTSPServer::get_active_streams() const {
  std::vector<std::string> streams;
  std::lock_guard<std::mutex> lock(streams_mutex_);
  for (const auto& [path, info] : active_streams_) {
    streams.push_back(path);
  }
  return streams;
}

MediaInfo RTSPServer::get_media_info(const std::string& stream_path) const {
  std::lock_guard<std::mutex> lock(streams_mutex_);
  auto it = active_streams_.find(stream_path);
  if (it != active_streams_.end()) {
    return it->second;
  }
  return MediaInfo{};
}

void RTSPServer::server_loop() {
  while (running_.load()) {
    struct pollfd pfd{};
    pfd.fd = server_socket_;
    pfd.events = POLLIN;
    
    int ret = poll(&pfd, 1, 1000);  // 1 秒超时
    if (ret < 0) {
      if (errno == EINTR) continue;
      log("poll 失败：" + std::string(strerror(errno)));
      break;
    }
    
    if (ret == 0) continue;  // 超时
    
    // 接受新连接
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_socket < 0) {
      if (errno == EINTR) continue;
      log("accept 失败：" + std::string(strerror(errno)));
      continue;
    }

    std::string client_ip = inet_ntoa(client_addr.sin_addr);
    log("新客户端连接：" + client_ip);

    // 在新线程中处理客户端
    std::thread(&RTSPServer::handle_client, this, client_socket, client_ip).detach();
  }
}

void RTSPServer::handle_client(int client_socket, const std::string& client_ip) {
  char buffer[4096];
  std::string stream_path;
  MediaInfo media_info;
  bool streaming = false;
  
  // 设置超时
  struct timeval timeout{config_.timeout_sec, 0};
  setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  while (running_.load()) {
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
      if (bytes_read < 0 && errno == EAGAIN) continue;
      break;
    }

    buffer[bytes_read] = '\0';
    std::string request(buffer);

    // 检查是否是 RTSP 请求
    if (request.find("RTSP/1.0") != std::string::npos) {
      std::string response;
      if (handle_rtsp_request(client_socket, request, stream_path, media_info, client_ip)) {
        if (!streaming && !stream_path.empty()) {
          // 新流开始
          std::lock_guard<std::mutex> lock(streams_mutex_);
          if (active_streams_.find(stream_path) == active_streams_.end()) {
            active_streams_[stream_path] = media_info;
            log("新流：" + stream_path);
            if (on_new_stream_) {
              on_new_stream_(stream_path);
            }
          }
          streaming = true;
        }
      } else {
        break;
      }
    } else {
      // 可能是 RTP 数据或其他，忽略
      break;
    }
  }

  // 客户端断开
  close(client_socket);
  log("客户端断开：" + client_ip);

  // 如果没有其他客户端了，移除流
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    bool has_other_clients = false;
    for (const auto& [fd, session] : sessions_) {
      if (session->stream_path == stream_path) {
        has_other_clients = true;
        break;
      }
    }
    
    if (!has_other_clients && !stream_path.empty()) {
      std::lock_guard<std::mutex> lock2(streams_mutex_);
      active_streams_.erase(stream_path);
      log("流结束：" + stream_path);
      if (on_stream_closed_) {
        on_stream_closed_(stream_path);
      }
    }
  }
}

bool RTSPServer::handle_rtsp_request(int client_socket, const std::string& request,
                                    std::string& stream_path, MediaInfo& media_info,
                                    const std::string& client_ip) {
  // 解析请求行
  auto first_line_end = request.find('\n');
  if (first_line_end == std::string::npos) return false;
  
  std::string request_line = request.substr(0, first_line_end);
  std::string method, uri, version;
  
  if (!parse_request_line(request_line, method, uri, version)) {
    return false;
  }

  // 解析头
  auto headers = parse_headers(request.substr(first_line_end + 1));
  
  // 获取 CSeq
  std::string cseq = headers.count("CSeq") ? headers["CSeq"] : "1";

  // 提取流路径
  stream_path = extract_stream_path(uri);

  std::ostringstream response;

  if (method == "OPTIONS") {
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, ANNOUNCE, RECORD\r\n"
             << "\r\n";
    send_rtsp_response(client_socket, response.str());
    return true;
  }
  
  if (method == "ANNOUNCE") {
    // 接收 SDP 信息（推流端发送的媒体描述）
    // 获取 Content-Length
    std::string content_length = headers.count("Content-Length") ? headers["Content-Length"] : "0";
    int length = std::stoi(content_length);
    
    // 读取 SDP 数据
    std::string sdp_data;
    if (length > 0) {
      char sdp_buffer[4096];
      int remaining = length;
      while (remaining > 0) {
        int received = recv(client_socket, sdp_buffer, std::min(remaining, (int)sizeof(sdp_buffer)), 0);
        if (received <= 0) break;
        sdp_data.append(sdp_buffer, received);
        remaining -= received;
      }
    }
    
    log("收到 ANNOUNCE: " + stream_path);
    log("SDP: " + sdp_data.substr(0, 200));
    
    // 解析 SDP 获取媒体信息
    media_info.stream_path = stream_path;
    media_info.sdp = sdp_data;
    media_info.has_video = (sdp_data.find("video") != std::string::npos);
    media_info.has_audio = (sdp_data.find("audio") != std::string::npos);
    
    if (sdp_data.find("H264") != std::string::npos) {
      media_info.video_codec = "h264";
    } else if (sdp_data.find("H265") != std::string::npos) {
      media_info.video_codec = "h265";
    }
    
    if (sdp_data.find("mpeg4-generic") != std::string::npos || 
        sdp_data.find("MP4A-LATM") != std::string::npos) {
      media_info.audio_codec = "aac";
    }
    
    // 从 SDP 中提取 SPS/PPS
    std::vector<uint8_t> sps, pps;
    if (extract_sps_pps_from_sdp(sdp_data, sps, pps)) {
      media_info.sps = sps;
      media_info.pps = pps;
      log("从 SDP 中提取 SPS/PPS 成功：" + std::to_string(sps.size()) + "/" + 
          std::to_string(pps.size()) + " 字节");
      
      // 调用 SPS/PPS 回调
      if (on_sps_pps_) {
        on_sps_pps_(stream_path, sps, pps);
      }
    } else {
      log("从 SDP 中提取 SPS/PPS 失败");
    }
    
    // 返回 200 OK
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "\r\n";
    send_rtsp_response(client_socket, response.str());
    return true;
  }
  
  if (method == "RECORD") {
    // 开始录制（推流）
    log("开始推流：" + stream_path);
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "Range: npt=0.000-\r\n"
             << "Session: 12345678\r\n"
             << "\r\n";
    send_rtsp_response(client_socket, response.str());
    return true;
  }
  
  if (method == "DESCRIBE") {
    // 返回 SDP
    // 注意：这里需要实际的 SDP，暂时返回一个简单的
    std::string sdp = 
      "v=0\r\n"
      "o=- 0 0 IN IP4 127.0.0.1\r\n"
      "s=No Name\r\n"
      "c=IN IP4 0.0.0.0\r\n"
      "t=0 0\r\n"
      "m=video 0 RTP/AVP 96\r\n"
      "a=rtpmap:96 H264/90000\r\n"
      "a=fmtp:96 packetization-mode=1\r\n"
      "a=control:track0\r\n";
    
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "Content-Type: application/sdp\r\n"
             << "Content-Length: " << sdp.length() << "\r\n"
             << "\r\n"
             << sdp;
    send_rtsp_response(client_socket, response.str());
    return true;
  }
  
  if (method == "SETUP") {
    // 解析 transport
    std::string transport = headers.count("Transport") ? headers["Transport"] : "";
    int client_port = parse_client_port(transport);
    
    if (client_port == 0) {
      log("无法解析客户端 RTP 端口");
      response << "RTSP/1.0 461 Unsupported Transport\r\n"
               << "CSeq: " << cseq << "\r\n"
               << "\r\n";
      send_rtsp_response(client_socket, response.str());
      return false;
    }
    
    // 创建 RTP 会话
    auto rtsp_session = std::make_shared<RTSPSession>(stream_path);
    
    // 设置会话
    uint32_t ssrc = generate_ssrc();
    if (!rtsp_session->setup(client_port, client_port + 1, ssrc)) {
      log("RTP 会话 setup 失败");
      response << "RTSP/1.0 500 Internal Server Error\r\n"
               << "CSeq: " << cseq << "\r\n"
               << "\r\n";
      send_rtsp_response(client_socket, response.str());
      return false;
    }
    
    // 设置客户端地址
    rtsp_session->set_client_address(client_ip);
    
    // 设置帧回调
    rtsp_session->set_frame_callback([this, stream_path](const std::string& path,
                                                         const std::vector<uint8_t>& data,
                                                         int64_t pts, bool is_key_frame) {
      if (on_video_frame_) {
        on_video_frame_(path, data, pts, is_key_frame);
      }
    });
    
    // 启动 RTP 接收
    if (!rtsp_session->start()) {
      log("RTP 会话启动失败");
      response << "RTSP/1.0 500 Internal Server Error\r\n"
               << "CSeq: " << cseq << "\r\n"
               << "\r\n";
      send_rtsp_response(client_socket, response.str());
      return false;
    }
    
    int server_port = rtsp_session->get_server_rtp_port();
    
    log("RTP 会话已创建：" + stream_path + " (client:" + std::to_string(client_port) + 
        " server:" + std::to_string(server_port) + ")");
    
    // 返回响应
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "Transport: RTP/AVP;unicast;client_port=" << client_port << "-" << (client_port+1)
             << ";server_port=" << server_port << "-" << (server_port+1) << "\r\n"
             << "Session: 12345678\r\n"
             << "\r\n";
    send_rtsp_response(client_socket, response.str());
    
    // 保存会话信息
    auto session = std::make_shared<SessionState>();
    session->stream_path = stream_path;
    session->client_socket = client_socket;
    session->rtp_port = client_port;
    session->ssrc = ssrc;
    
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      sessions_[client_socket] = session;
      rtsp_sessions_[stream_path] = rtsp_session;  // 保存 RTP 会话
    }
    
    return true;
  }
  
  if (method == "PLAY") {
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "Range: npt=0.000-\r\n"
             << "Session: 12345678\r\n"
             << "\r\n";
    send_rtsp_response(client_socket, response.str());
    return true;
  }
  
  if (method == "TEARDOWN") {
    response << "RTSP/1.0 200 OK\r\n"
             << "CSeq: " << cseq << "\r\n"
             << "\r\n";
    send_rtsp_response(client_socket, response.str());
    return false;  // 结束
  }

  // 不支持的方法
  response << "RTSP/1.0 501 Not Implemented\r\n"
           << "CSeq: " << cseq << "\r\n"
           << "\r\n";
  send_rtsp_response(client_socket, response.str());
  return true;
}

void RTSPServer::send_rtsp_response(int client_socket, const std::string& response) {
  send(client_socket, response.c_str(), response.length(), 0);
}

}  // namespace rtsp_rtmp2webrtc_hls
