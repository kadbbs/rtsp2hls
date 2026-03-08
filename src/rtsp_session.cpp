#include "rtsp_session.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>

namespace rtsp_rtmp2webrtc_hls {

RTSPSession::RTSPSession(const std::string& stream_path)
    : stream_path_(stream_path) {}

RTSPSession::~RTSPSession() {
    stop();
}

bool RTSPSession::setup(int client_rtp_port, int client_rtcp_port, uint32_t ssrc) {
    if (running_.load()) {
        return false;
    }
    
    client_rtp_port_ = client_rtp_port;
    client_rtcp_port_ = client_rtcp_port;
    ssrc_ = ssrc;
    
    // 创建 RTP socket（服务器端监听端口）
    rtp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rtp_socket_ < 0) {
        log("创建 RTP socket 失败：" + std::string(strerror(errno)));
        return false;
    }
    
    // 设置 socket 选项
    int opt = 1;
    setsockopt(rtp_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 绑定到随机端口（让系统分配）
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;  // 系统分配
    
    if (bind(rtp_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log("绑定 RTP socket 失败：" + std::string(strerror(errno)));
        close(rtp_socket_);
        rtp_socket_ = -1;
        return false;
    }
    
    // 获取分配的端口
    struct sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (getsockname(rtp_socket_, (struct sockaddr*)&bound_addr, &addr_len) == 0) {
        server_rtp_port_ = ntohs(bound_addr.sin_port);
        log("RTP socket 绑定在端口：" + std::to_string(server_rtp_port_));
    }
    
    // 设置客户端地址（用于发送 RTCP）
    memset(&client_addr_, 0, sizeof(client_addr_));
    client_addr_.sin_family = AF_INET;
    client_addr_.sin_port = htons(client_rtp_port);
    
    return true;
}

void RTSPSession::set_client_address(const std::string& ip) {
    client_ip_ = ip;
    inet_pton(AF_INET, ip.c_str(), &client_addr_.sin_addr);
}

bool RTSPSession::start() {
    if (running_.load()) {
        return false;
    }
    
    running_.store(true);
    receive_thread_ = std::thread(&RTSPSession::receive_loop, this);
    log("RTP 接收线程已启动");
    return true;
}

void RTSPSession::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    if (rtp_socket_ >= 0) {
        shutdown(rtp_socket_, SHUT_RDWR);
        close(rtp_socket_);
        rtp_socket_ = -1;
    }
    
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    
    log("RTP 会话已停止");
}

void RTSPSession::receive_loop() {
    char buffer[65536];  // 最大 UDP 包大小
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    while (running_.load()) {
        // 使用 poll 避免阻塞
        struct pollfd pfd{};
        pfd.fd = rtp_socket_;
        pfd.events = POLLIN;
        
        int ret = poll(&pfd, 1, 1000);  // 1 秒超时
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        if (ret == 0) continue;  // 超时
        
        // 接收数据
        int bytes_read = recvfrom(rtp_socket_, buffer, sizeof(buffer), 0,
                                 (struct sockaddr*)&from_addr, &from_len);
        
        if (bytes_read <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            log("recvfrom 错误：" + std::string(strerror(errno)));
            continue;
        }
        
        // 解析 RTP 包
        parse_rtp_packet((const uint8_t*)buffer, bytes_read);
    }
}

void RTSPSession::parse_rtp_packet(const uint8_t* data, size_t len) {
    if (len < 12) {
        log("RTP 包太短：" + std::to_string(len));
        return;
    }
    
    // 解析 RTP 头部
    uint8_t version = (data[0] >> 6) & 0x03;
    if (version != 2) {
        log("无效的 RTP 版本：" + std::to_string(version));
        return;
    }
    
    uint8_t padding = (data[0] >> 5) & 0x01;
    uint8_t extension = (data[0] >> 4) & 0x01;
    uint8_t csrc_count = data[0] & 0x0F;
    uint8_t marker = (data[1] >> 7) & 0x01;
    uint8_t payload_type = data[1] & 0x7F;
    
    uint16_t seq_num = ((uint16_t)data[2] << 8) | data[3];
    uint32_t timestamp = ((uint32_t)data[4] << 24) | 
                        ((uint32_t)data[5] << 16) | 
                        ((uint32_t)data[6] << 8) | 
                        data[7];
    uint32_t ssrc = ((uint32_t)data[8] << 24) | 
                   ((uint32_t)data[9] << 16) | 
                   ((uint32_t)data[10] << 8) | 
                   data[11];
    
    // SSRC 验证（可选，暂时跳过以兼容 FFmpeg）
    // if (ssrc != ssrc_) {
    //     log("SSRC 不匹配：" + std::to_string(ssrc) + " != " + std::to_string(ssrc_));
    //     return;
    // }
    
    // 计算负载起始位置
    size_t header_size = 12 + (csrc_count * 4);
    if (extension) {
        if (header_size + 4 > len) return;
        uint16_t ext_len = ((uint16_t)data[header_size + 2] << 8) | data[header_size + 3];
        header_size += 4 + (ext_len * 4);
    }
    
    size_t payload_offset = header_size;
    if (padding) {
        uint8_t pad_count = data[len - 1];
        if (pad_count > 0 && pad_count <= len) {
            len -= pad_count;
        }
    }
    
    const uint8_t* payload = data + payload_offset;
    size_t payload_len = len - payload_offset;
    
    // 处理 H.264 负载
    if (payload_type == 96 || payload_type == 97) {  // H.264
        handle_h264_payload(payload, payload_len, timestamp, marker, seq_num);
    }
}

void RTSPSession::handle_h264_payload(const uint8_t* data, size_t len, 
                                      uint32_t timestamp, bool marker, uint16_t seq) {
    if (len < 1) return;
    
    uint8_t nal_header = data[0];
    uint8_t nal_type = nal_header & 0x1F;
    
    // NALU 类型
    // 1-23: NAL 单元
    // 24: STAP-A (单时间聚合包)
    // 25-27: 保留
    // 28: FU-A (分片单元)
    
    if (nal_type >= 1 && nal_type <= 23) {
        // 完整 NALU
        handle_single_nalu(data, len, timestamp, nal_type);
    } else if (nal_type == 28 || nal_type == 29) {
        // FU-A 分片
        handle_fu_a(data, len, timestamp, marker);
    } else if (nal_type == 24) {
        // STAP-A
        handle_stap_a(data, len, timestamp);
    }
}

void RTSPSession::handle_single_nalu(const uint8_t* data, size_t len, 
                                     uint32_t timestamp, uint8_t nal_type) {
    // 添加 4 字节长度前缀（Annex B 格式）
    std::vector<uint8_t> nalu(len + 4);
    nalu[0] = 0x00;
    nalu[1] = 0x00;
    nalu[2] = 0x00;
    nalu[3] = 0x01;
    memcpy(nalu.data() + 4, data, len);
    
    // 检查是否关键帧（IDR 帧，nal_type = 5）
    bool is_key_frame = (nal_type == 5);
    
    // 调用回调
    if (frame_cb_) {
        frame_cb_(stream_path_, nalu, (int64_t)timestamp * 1000 / 90, is_key_frame);
    }
    
    nalu_count_++;
}

void RTSPSession::handle_fu_a(const uint8_t* data, size_t len, uint32_t timestamp, bool marker) {
    if (len < 2) return;
    
    uint8_t fu_indicator = data[0];
    uint8_t fu_header = data[1];
    
    bool start_bit = (fu_header >> 7) & 1;
    bool end_bit = (fu_header >> 6) & 1;
    uint8_t nal_type = fu_header & 0x1F;
    
    if (start_bit) {
        // 开始新片段
        fu_buffer_.clear();
        
        // 重建原始 NAL 头
        uint8_t original_nal = (fu_indicator & 0xE0) | nal_type;
        fu_buffer_.push_back(0x00);
        fu_buffer_.push_back(0x00);
        fu_buffer_.push_back(0x00);
        fu_buffer_.push_back(0x01);
        fu_buffer_.push_back(original_nal);
        
        fu_timestamp_ = timestamp;
    }
    
    if (!fu_buffer_.empty()) {
        // 添加负载数据
        fu_buffer_.insert(fu_buffer_.end(), data + 2, data + len);
    }
    
    if (end_bit && marker) {
        // 片段结束，发送完整 NALU
        if (frame_cb_) {
            bool is_key_frame = (nal_type == 5);
            frame_cb_(stream_path_, fu_buffer_, (int64_t)fu_timestamp_ * 1000 / 90, is_key_frame);
        }
        fu_buffer_.clear();
        fu_count_++;
    }
}

void RTSPSession::handle_stap_a(const uint8_t* data, size_t len, uint32_t timestamp) {
    if (len < 3) return;
    
    size_t offset = 1;  // 跳过 nal_header
    int nalu_count = 0;
    
    while (offset + 2 < len) {
        // 读取 NALU 长度
        uint16_t nalu_len = ((uint16_t)data[offset] << 8) | data[offset + 1];
        offset += 2;
        
        if (offset + nalu_len > len) break;
        
        // 添加长度前缀
        std::vector<uint8_t> nalu(nalu_len + 4);
        nalu[0] = 0x00;
        nalu[1] = 0x00;
        nalu[2] = 0x00;
        nalu[3] = 0x01;
        memcpy(nalu.data() + 4, data + offset, nalu_len);
        
        uint8_t nal_type = data[offset] & 0x1F;
        bool is_key_frame = (nal_type == 5);
        
        if (frame_cb_) {
            frame_cb_(stream_path_, nalu, (int64_t)timestamp * 1000 / 90, is_key_frame);
        }
        
        offset += nalu_len;
        nalu_count++;
    }
    
    stap_count_ += nalu_count;
}

void RTSPSession::log(const std::string& msg) const {
    std::cerr << "[RTSP Session] " << stream_path_ << " (port:" << server_rtp_port_ 
              << ") " << msg << std::endl;
}

uint64_t RTSPSession::get_stats() const {
    return nalu_count_ + fu_count_ + stap_count_;
}

}  // namespace rtsp_rtmp2webrtc_hls
