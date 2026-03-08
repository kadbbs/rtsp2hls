#pragma once

#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace rtsp_rtmp2webrtc_hls {

// HLS 复用器 - 负责将 RTP 帧写入 HLS
class HLSMuxer {
public:
    explicit HLSMuxer(const std::string& output_dir, int segment_duration = 2, int playlist_size = 5);
    ~HLSMuxer();
    
    // 添加流
    bool add_stream(const std::string& stream_path);
    
    // 写入视频帧
    bool write_video_frame(const std::string& stream_path, 
                          const std::vector<uint8_t>& data,
                          int64_t pts, bool is_key_frame);
    
    // 设置编解码器额外数据（SPS/PPS）
    void set_extradata(const std::string& stream_path,
                       const std::vector<uint8_t>& sps,
                       const std::vector<uint8_t>& pps);
    
    // 移除流
    void remove_stream(const std::string& stream_path);
    
    // 获取播放列表路径
    std::string get_playlist_path(const std::string& stream_path) const;
    
private:
    struct StreamContext {
        AVFormatContext* format_ctx = nullptr;
        AVStream* video_stream = nullptr;
        int64_t last_pts = 0;
        int64_t last_dts = 0;
        bool initialized = false;
        
        // H.264 SPS/PPS 数据
        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        bool sps_extracted = false;
    };
    
    bool init_stream(StreamContext& ctx, const std::string& stream_path);
    bool init_stream_with_extradata(StreamContext& ctx, const std::string& stream_path,
                                    const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);
    void cleanup_stream(StreamContext& ctx);
    std::string get_stream_filename(const std::string& stream_path) const;
    
    std::string output_dir_;
    int segment_duration_;
    int playlist_size_;
    
    std::map<std::string, std::unique_ptr<StreamContext>> streams_;
    mutable std::mutex mutex_;
};

}  // namespace rtsp_rtmp2webrtc_hls
