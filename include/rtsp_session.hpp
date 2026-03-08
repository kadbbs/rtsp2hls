#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace rtsp_rtmp2webrtc_hls {

// RTP 会话 - 负责接收实际的音视频流数据
class RTSPSession {
public:
    using FrameCallback = std::function<void(const std::string& stream_path, 
                                              const std::vector<uint8_t>& data,
                                              int64_t pts, bool is_key_frame)>;
    
    explicit RTSPSession(const std::string& stream_path);
    ~RTSPSession();
    
    // 初始化会话（SETUP 阶段）
    bool setup(int client_rtp_port, int client_rtcp_port, uint32_t ssrc);
    
    // 设置客户端地址
    void set_client_address(const std::string& ip);
    
    // 设置帧回调
    void set_frame_callback(FrameCallback cb) { frame_cb_ = std::move(cb); }
    
    // 启动接收
    bool start();
    
    // 停止会话
    void stop();
    
    // 获取信息
    std::string get_stream_path() const { return stream_path_; }
    int get_server_rtp_port() const { return server_rtp_port_; }
    uint64_t get_stats() const;
    
    // 检查是否正在运行
    bool is_running() const { return running_.load(); }
    
private:
    // RTP 接收循环
    void receive_loop();
    
    // 解析 RTP 包
    void parse_rtp_packet(const uint8_t* data, size_t len);
    
    // 处理 H.264 负载
    void handle_h264_payload(const uint8_t* data, size_t len, 
                           uint32_t timestamp, bool marker, uint16_t seq);
    
    // 处理单个 NALU
    void handle_single_nalu(const uint8_t* data, size_t len, 
                           uint32_t timestamp, uint8_t nal_type);
    
    // 处理 FU-A 分片
    void handle_fu_a(const uint8_t* data, size_t len, uint32_t timestamp, bool marker);
    
    // 处理 STAP-A 聚合包
    void handle_stap_a(const uint8_t* data, size_t len, uint32_t timestamp);
    
    // 日志
    void log(const std::string& msg) const;
    
    std::string stream_path_;
    std::atomic<bool> running_{false};
    
    // Socket
    int rtp_socket_ = -1;
    struct sockaddr_in client_addr_;
    std::string client_ip_;
    
    // 端口信息
    int client_rtp_port_ = 0;
    int client_rtcp_port_ = 0;
    int server_rtp_port_ = 0;  // 服务器实际监听的端口
    
    // SSRC
    uint32_t ssrc_ = 0;
    
    // 统计
    uint64_t nalu_count_ = 0;
    uint64_t fu_count_ = 0;
    uint64_t stap_count_ = 0;
    
    // 回调
    FrameCallback frame_cb_;
    std::thread receive_thread_;
    
    // FU-A 重组缓冲
    std::vector<uint8_t> fu_buffer_;
    uint32_t fu_timestamp_ = 0;
    
    mutable std::mutex mutex_;
};

}  // namespace rtsp_rtmp2webrtc_hls
