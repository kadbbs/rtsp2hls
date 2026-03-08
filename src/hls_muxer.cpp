#include "hls_muxer.hpp"
#include <sys/stat.h>
#include <iostream>
#include <cstring>

namespace rtsp_rtmp2webrtc_hls {

// 从 H.264 数据中提取 SPS 和 PPS
static bool extract_sps_pps(const uint8_t* data, size_t size, 
                           std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    size_t pos = 0;
    while (pos + 4 <= size) {
        // 查找 NAL 起始码
        bool found_start = false;
        size_t start_offset = 0;
        
        if (pos + 4 <= size && data[pos] == 0 && data[pos + 1] == 0 && 
            data[pos + 2] == 0 && data[pos + 3] == 1) {
            found_start = true;
            start_offset = 4;
        } else if (pos + 3 <= size && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
            found_start = true;
            start_offset = 3;
        }
        
        if (!found_start) {
            pos++;
            continue;
        }
        
        size_t nal_start = pos + start_offset;
        if (nal_start >= size) break;
        
        uint8_t nal_type = data[nal_start] & 0x1F;
        
        // 查找下一个 NAL 起始码
        size_t next_nal = nal_start + 1;
        while (next_nal + 3 <= size) {
            if (data[next_nal] == 0 && data[next_nal + 1] == 0 && 
                ((data[next_nal + 2] == 1) || 
                 (data[next_nal + 2] == 0 && next_nal + 3 < size && data[next_nal + 3] == 1))) {
                break;
            }
            next_nal++;
        }
        
        size_t nal_size = next_nal - nal_start;
        
        // SPS (类型 7)
        if (nal_type == 7 && sps.empty()) {
            sps.resize(nal_size);
            std::memcpy(sps.data(), data + nal_start, nal_size);
            std::cout << "[HLS Muxer] 找到 SPS，大小：" << nal_size << std::endl;
        }
        // PPS (类型 8)
        else if (nal_type == 8 && pps.empty()) {
            pps.resize(nal_size);
            std::memcpy(pps.data(), data + nal_start, nal_size);
            std::cout << "[HLS Muxer] 找到 PPS，大小：" << nal_size << std::endl;
        }
        
        if (!sps.empty() && !pps.empty()) return true;
        pos = next_nal;
    }
    return !sps.empty() && !pps.empty();
}

// 创建 H.264 编解码器 extradata (AVCC 格式)
static std::vector<uint8_t> create_h264_extradata(const std::vector<uint8_t>& sps, 
                                                   const std::vector<uint8_t>& pps) {
    if (sps.size() < 4 || pps.size() < 4) return {};
    
    std::vector<uint8_t> extradata;
    extradata.reserve(16 + sps.size() + pps.size());
    
    // version
    extradata.push_back(0x01);
    // profile
    extradata.push_back(sps[0]);
    // compatibility
    extradata.push_back(0x00);
    // level
    extradata.push_back(sps[2]);
    // reserved + lengthSizeMinusOne
    extradata.push_back(0xff);
    extradata.push_back(0xe1);
    // sps size
    extradata.push_back((sps.size() >> 8) & 0xff);
    extradata.push_back(sps.size() & 0xff);
    // sps data
    extradata.insert(extradata.end(), sps.begin(), sps.end());
    // pps count
    extradata.push_back(0x01);
    // pps size
    extradata.push_back((pps.size() >> 8) & 0xff);
    extradata.push_back(pps.size() & 0xff);
    // pps data
    extradata.insert(extradata.end(), pps.begin(), pps.end());
    
    return extradata;
}

HLSMuxer::HLSMuxer(const std::string& output_dir, int segment_duration, int playlist_size)
    : output_dir_(output_dir), segment_duration_(segment_duration), playlist_size_(playlist_size) {
    mkdir(output_dir_.c_str(), 0755);
}

HLSMuxer::~HLSMuxer() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [path, ctx] : streams_) {
        cleanup_stream(*ctx);
    }
}

bool HLSMuxer::add_stream(const std::string& stream_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string clean_path = stream_path;
    size_t query_pos = clean_path.find('?');
    if (query_pos != std::string::npos) {
        clean_path = clean_path.substr(0, query_pos);
    }
    size_t streamid_pos = clean_path.find("/streamid=");
    if (streamid_pos != std::string::npos) {
        clean_path = clean_path.substr(0, streamid_pos);
    }
    
    if (streams_.find(clean_path) != streams_.end()) {
        return true;
    }
    
    auto ctx = std::make_unique<StreamContext>();
    std::string stream_dir = output_dir_ + "/" + get_stream_filename(clean_path);
    mkdir(stream_dir.c_str(), 0755);
    
    streams_[clean_path] = std::move(ctx);
    return true;
}

bool HLSMuxer::write_video_frame(const std::string& stream_path, 
                                 const std::vector<uint8_t>& data,
                                 int64_t pts, bool is_key_frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string clean_path = stream_path;
    size_t query_pos = clean_path.find('?');
    if (query_pos != std::string::npos) {
        clean_path = clean_path.substr(0, query_pos);
    }
    size_t streamid_pos = clean_path.find("/streamid=");
    if (streamid_pos != std::string::npos) {
        clean_path = clean_path.substr(0, streamid_pos);
    }
    
    auto it = streams_.find(clean_path);
    if (it == streams_.end()) {
        auto ctx = std::make_unique<StreamContext>();
        std::string stream_dir = output_dir_ + "/" + get_stream_filename(clean_path);
        mkdir(stream_dir.c_str(), 0755);
        streams_[clean_path] = std::move(ctx);
        it = streams_.find(clean_path);
        if (it == streams_.end()) {
            std::cerr << "[HLS Muxer] 流不存在：" << clean_path << std::endl;
            return false;
        }
    }
    
    StreamContext& ctx = *it->second;
    
    // 如果是关键帧，尝试提取 SPS/PPS
    if (is_key_frame && !ctx.sps_extracted) {
        std::vector<uint8_t> sps, pps;
        if (extract_sps_pps(data.data(), data.size(), sps, pps)) {
            ctx.sps = sps;
            ctx.pps = pps;
            ctx.sps_extracted = true;
            std::cout << "[HLS Muxer] 已提取 SPS/PPS: " << sps.size() << "/" << pps.size() << " 字节" << std::endl;
        }
    }
    
    // 初始化流
    if (!ctx.initialized) {
        // 如果有 SPS/PPS，先设置 extradata 再初始化
        if (ctx.sps_extracted && !ctx.sps.empty() && !ctx.pps.empty()) {
            if (init_stream_with_extradata(ctx, clean_path, ctx.sps, ctx.pps)) {
                ctx.initialized = true;
                std::cout << "[HLS Muxer] 流已初始化（带 SPS/PPS）：" << clean_path << std::endl;
            } else {
                return false;
            }
        } else {
            // 没有 SPS/PPS，先初始化（会在之后设置）
            if (init_stream(ctx, clean_path)) {
                ctx.initialized = true;
                std::cout << "[HLS Muxer] 流已初始化（等待 SPS/PPS）：" << clean_path << std::endl;
            } else {
                return false;
            }
        }
    }
    
    if (!ctx.format_ctx || !ctx.video_stream) {
        return false;
    }
    
    // 对于关键帧，在数据前面添加 SPS+PPS
    std::vector<uint8_t> frame_data;
    if (is_key_frame && !ctx.sps.empty() && !ctx.pps.empty()) {
        // 添加 SPS (带 00 00 00 01 起始码)
        frame_data.insert(frame_data.end(), {0x00, 0x00, 0x00, 0x01});
        frame_data.insert(frame_data.end(), ctx.sps.begin(), ctx.sps.end());
        
        // 添加 PPS (带 00 00 00 01 起始码)
        frame_data.insert(frame_data.end(), {0x00, 0x00, 0x00, 0x01});
        frame_data.insert(frame_data.end(), ctx.pps.begin(), ctx.pps.end());
        
        // 添加原始数据
        frame_data.insert(frame_data.end(), data.begin(), data.end());
    }
    
    const uint8_t* write_data = frame_data.empty() ? data.data() : frame_data.data();
    size_t write_size = frame_data.empty() ? data.size() : frame_data.size();
    
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;
    
    // 分配新内存，因为 FFmpeg 会接管
    if (av_new_packet(pkt, write_size) < 0) {
        av_packet_free(&pkt);
        return false;
    }
    
    std::memcpy(pkt->data, write_data, write_size);
    pkt->size = write_size;
    pkt->stream_index = ctx.video_stream->index;
    
    if (is_key_frame) {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }
    
    const int64_t frame_duration = 3000;
    int64_t new_pts = pts * 90 / 1000;
    if (new_pts > ctx.last_pts) {
        ctx.last_pts = new_pts;
    } else {
        ctx.last_pts += frame_duration;
    }
    
    pkt->pts = ctx.last_pts;
    pkt->dts = ctx.last_pts;
    
    int ret = av_interleaved_write_frame(ctx.format_ctx, pkt);
    av_packet_free(&pkt);
    
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[HLS Muxer] 写入失败：" << errbuf << std::endl;
        return false;
    }
    
    return true;
}

void HLSMuxer::set_extradata(const std::string& stream_path,
                              const std::vector<uint8_t>& sps,
                              const std::vector<uint8_t>& pps) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string clean_path = stream_path;
    size_t query_pos = clean_path.find('?');
    if (query_pos != std::string::npos) {
        clean_path = clean_path.substr(0, query_pos);
    }
    size_t streamid_pos = clean_path.find("/streamid=");
    if (streamid_pos != std::string::npos) {
        clean_path = clean_path.substr(0, streamid_pos);
    }
    
    auto it = streams_.find(clean_path);
    if (it == streams_.end()) {
        // 流还不存在，先创建上下文并存储 SPS/PPS
        auto ctx = std::make_unique<StreamContext>();
        ctx->sps = sps;
        ctx->pps = pps;
        ctx->sps_extracted = true;
        std::string stream_dir = output_dir_ + "/" + get_stream_filename(clean_path);
        mkdir(stream_dir.c_str(), 0755);
        streams_[clean_path] = std::move(ctx);
        std::cout << "[HLS Muxer] 已存储 SPS/PPS（等待视频帧）: " << sps.size() << "/" << pps.size() << " 字节" << std::endl;
    } else {
        // 流已存在，更新 SPS/PPS
        it->second->sps = sps;
        it->second->pps = pps;
        it->second->sps_extracted = true;
        std::cout << "[HLS Muxer] 已更新 SPS/PPS: " << sps.size() << "/" << pps.size() << " 字节" << std::endl;
        
        // 如果流已初始化但没有 extradata，需要重新初始化
        if (it->second->initialized && it->second->video_stream && 
            !it->second->video_stream->codecpar->extradata) {
            // 关闭当前输出
            if (it->second->format_ctx->pb) {
                avio_flush(it->second->format_ctx->pb);
            }
            av_write_trailer(it->second->format_ctx);
            
            // 设置 extradata
            auto extradata = create_h264_extradata(sps, pps);
            if (!extradata.empty()) {
                it->second->video_stream->codecpar->extradata = (uint8_t*)av_malloc(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
                if (it->second->video_stream->codecpar->extradata) {
                    std::memcpy(it->second->video_stream->codecpar->extradata, extradata.data(), extradata.size());
                    std::memset(it->second->video_stream->codecpar->extradata + extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
                    it->second->video_stream->codecpar->extradata_size = extradata.size();
                    std::cout << "[HLS Muxer] 已设置 extradata: " << extradata.size() << " 字节" << std::endl;
                }
            }
            
            // 重新写入文件头
            AVDictionary* opts = nullptr;
            avformat_write_header(it->second->format_ctx, &opts);
        }
    }
}

void HLSMuxer::remove_stream(const std::string& stream_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_path);
    if (it != streams_.end()) {
        cleanup_stream(*it->second);
        streams_.erase(it);
    }
}

std::string HLSMuxer::get_playlist_path(const std::string& stream_path) const {
    return output_dir_ + "/" + get_stream_filename(stream_path) + "/playlist.m3u8";
}

// 使用 SPS/PPS 初始化流
bool HLSMuxer::init_stream_with_extradata(StreamContext& ctx, const std::string& stream_path,
                                          const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    std::string stream_dir = output_dir_ + "/" + get_stream_filename(stream_path);
    std::string playlist_path = stream_dir + "/playlist.m3u8";
    std::string segment_pattern = stream_dir + "/segment_%03d.ts";
    
    int ret = avformat_alloc_output_context2(&ctx.format_ctx, nullptr, "hls", playlist_path.c_str());
    if (ret < 0 || !ctx.format_ctx) {
        std::cerr << "[HLS Muxer] 创建输出上下文失败" << std::endl;
        return false;
    }
    
    av_opt_set(ctx.format_ctx->priv_data, "hls_time", std::to_string(segment_duration_).c_str(), 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_list_size", std::to_string(playlist_size_).c_str(), 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_segment_filename", segment_pattern.c_str(), 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_flags", "delete_segments", 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_allow_cache", "1", 0);
    
    ctx.video_stream = avformat_new_stream(ctx.format_ctx, nullptr);
    if (!ctx.video_stream) {
        std::cerr << "[HLS Muxer] 创建视频流失败" << std::endl;
        return false;
    }
    
    ctx.video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx.video_stream->codecpar->codec_id = AV_CODEC_ID_H264;
    ctx.video_stream->codecpar->format = AV_PIX_FMT_YUV420P;
    ctx.video_stream->codecpar->width = 640;
    ctx.video_stream->codecpar->height = 480;
    ctx.video_stream->time_base = (AVRational){1, 90000};
    
    // 设置 extradata
    auto extradata = create_h264_extradata(sps, pps);
    if (!extradata.empty()) {
        ctx.video_stream->codecpar->extradata = (uint8_t*)av_malloc(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ctx.video_stream->codecpar->extradata) {
            std::memcpy(ctx.video_stream->codecpar->extradata, extradata.data(), extradata.size());
            std::memset(ctx.video_stream->codecpar->extradata + extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
            ctx.video_stream->codecpar->extradata_size = extradata.size();
            std::cout << "[HLS Muxer] 已设置 extradata: " << extradata.size() << " 字节" << std::endl;
        }
    }
    
    if (!(ctx.format_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&ctx.format_ctx->pb, playlist_path.c_str(),
                        AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            std::cerr << "[HLS Muxer] 打开文件失败：" << errbuf << std::endl;
            return false;
        }
    }
    
    ret = avformat_write_header(ctx.format_ctx, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[HLS Muxer] 写入文件头失败：" << errbuf << std::endl;
        return false;
    }
    
    return true;
}

bool HLSMuxer::init_stream(StreamContext& ctx, const std::string& stream_path) {
    std::string stream_dir = output_dir_ + "/" + get_stream_filename(stream_path);
    std::string playlist_path = stream_dir + "/playlist.m3u8";
    std::string segment_pattern = stream_dir + "/segment_%03d.ts";
    
    int ret = avformat_alloc_output_context2(&ctx.format_ctx, nullptr, "hls", playlist_path.c_str());
    if (ret < 0 || !ctx.format_ctx) {
        std::cerr << "[HLS Muxer] 创建输出上下文失败" << std::endl;
        return false;
    }
    
    av_opt_set(ctx.format_ctx->priv_data, "hls_time", std::to_string(segment_duration_).c_str(), 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_list_size", std::to_string(playlist_size_).c_str(), 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_segment_filename", segment_pattern.c_str(), 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_flags", "delete_segments", 0);
    av_opt_set(ctx.format_ctx->priv_data, "hls_allow_cache", "1", 0);
    
    ctx.video_stream = avformat_new_stream(ctx.format_ctx, nullptr);
    if (!ctx.video_stream) {
        std::cerr << "[HLS Muxer] 创建视频流失败" << std::endl;
        return false;
    }
    
    ctx.video_stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx.video_stream->codecpar->codec_id = AV_CODEC_ID_H264;
    ctx.video_stream->codecpar->format = AV_PIX_FMT_YUV420P;
    ctx.video_stream->codecpar->width = 640;
    ctx.video_stream->codecpar->height = 480;
    ctx.video_stream->time_base = (AVRational){1, 90000};
    
    if (!(ctx.format_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&ctx.format_ctx->pb, playlist_path.c_str(),
                        AVIO_FLAG_WRITE, nullptr, nullptr);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            std::cerr << "[HLS Muxer] 打开文件失败：" << errbuf << std::endl;
            return false;
        }
    }
    
    ret = avformat_write_header(ctx.format_ctx, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[HLS Muxer] 写入文件头失败：" << errbuf << std::endl;
        return false;
    }
    
    return true;
}

void HLSMuxer::cleanup_stream(StreamContext& ctx) {
    if (ctx.format_ctx) {
        av_write_trailer(ctx.format_ctx);
        if (ctx.format_ctx->pb) {
            avio_closep(&ctx.format_ctx->pb);
        }
        avformat_free_context(ctx.format_ctx);
        ctx.format_ctx = nullptr;
        ctx.video_stream = nullptr;
    }
}

std::string HLSMuxer::get_stream_filename(const std::string& stream_path) const {
    std::string filename = stream_path;
    size_t query_pos = filename.find('?');
    if (query_pos != std::string::npos) {
        filename = filename.substr(0, query_pos);
    }
    for (char& c : filename) {
        if (c == '/') c = '_';
    }
    return filename;
}

}  // namespace rtsp_rtmp2webrtc_hls