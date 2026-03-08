#include "media_server.hpp"
#include "rtsp_server.hpp"
#include "hls_muxer.hpp"

#include <iostream>
#include <fstream>
#include <sys/stat.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace rtsp_rtmp2webrtc_hls {

MediaServer::MediaServer(Config config)
    : config_(std::move(config)) {}

MediaServer::~MediaServer() {
  stop();
}

void MediaServer::log(const std::string& msg) const {
  if (log_cb_) {
    log_cb_(msg);
  }
}

bool MediaServer::start() {
  if (running_.load()) {
    log("媒体服务器已经在运行");
    return false;
  }

  log("启动媒体服务器...");

  // 创建 HLS 输出目录
  if (config_.enable_hls) {
    mkdir(config_.hls_output_dir.c_str(), 0755);
    
    // 创建 HLS 复用器
    hls_muxer_ = std::make_unique<HLSMuxer>(config_.hls_output_dir, 2, 5);
    log("HLS 复用器已创建：" + config_.hls_output_dir);
  }

  running_.store(true);

  // 创建并启动 RTSP 服务器
  if (config_.enable_rtsp) {
    log("创建 RTSP 服务器...");
    RTSPServerConfig rtsp_config;
    rtsp_config.port = config_.rtsp_port;
    rtsp_config.bind_address = "0.0.0.0";
    
    rtsp_server_ = std::make_unique<RTSPServer>(rtsp_config);
    
    // 设置回调
    rtsp_server_->set_new_stream_callback([this](const std::string& path) {
      this->on_stream_started(path);
    });
    rtsp_server_->set_stream_closed_callback([this](const std::string& path) {
      this->on_stream_stopped(path);
    });
    rtsp_server_->set_video_frame_callback([this](const std::string& path, 
                                                   const std::vector<uint8_t>& data,
                                                   int64_t pts, bool is_key_frame) {
      this->on_video_frame_rtp(path, data, pts, is_key_frame);
    });
    rtsp_server_->set_sps_pps_callback([this](const std::string& path,
                                               const std::vector<uint8_t>& sps,
                                               const std::vector<uint8_t>& pps) {
      this->on_sps_pps_received(path, sps, pps);
    });
    rtsp_server_->set_log_callback([this](const std::string& msg) {
      this->log("[RTSP] " + msg);
    });
    
    if (!rtsp_server_->start()) {
      log("RTSP 服务器启动失败");
      rtsp_server_.reset();
      return false;
    }
    
    log("RTSP 服务器已启动");
  }

  // 启动 RTMP 服务器（暂未实现）
  if (config_.enable_rtmp) {
    log("启动 RTMP 服务器...");
  }

  // 启动 HLS 输出线程
  if (config_.enable_hls) {
    log("启动 HLS 输出...");
  }

  log("媒体服务器已启动");
  log("  RTSP: " + std::to_string(config_.rtsp_port));
  log("  RTMP: " + std::to_string(config_.rtmp_port));
  log("  HLS:  " + config_.hls_output_dir);

  return true;
}

void MediaServer::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);
  log("停止媒体服务器...");

  // 停止 RTSP 服务器
  if (rtsp_server_) {
    rtsp_server_->stop();
    rtsp_server_.reset();
  }

  // 等待所有线程结束
  if (rtsp_thread_.joinable()) rtsp_thread_.join();
  if (rtmp_thread_.joinable()) rtmp_thread_.join();
  if (hls_thread_.joinable()) hls_thread_.join();

  log("媒体服务器已停止");
}

std::vector<MediaStream> MediaServer::get_active_streams() const {
  std::vector<MediaStream> streams;
  std::lock_guard<std::mutex> lock(streams_mutex_);
  
  for (const auto& [path, info] : active_streams_) {
    MediaStream stream;
    stream.stream_path = path;
    stream.has_video = info.has_video;
    stream.has_audio = info.has_audio;
    stream.video_codec = info.video_codec;
    stream.audio_codec = info.audio_codec;
    stream.width = info.width;
    stream.height = info.height;
    stream.fps = info.fps;
    streams.push_back(stream);
  }
  
  return streams;
}

void MediaServer::on_stream_started(const std::string& stream_path) {
  log("新推流：" + stream_path);
  
  {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    MediaStream stream;
    stream.stream_path = stream_path;
    active_streams_[stream_path] = stream;
  }
  
  // 通知 WebRTC 发送器
  if (webrtc_sender_) {
    webrtc_sender_->add_stream(stream_path);
  }
}

void MediaServer::on_stream_stopped(const std::string& stream_path) {
  log("推流结束：" + stream_path);
  
  {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    active_streams_.erase(stream_path);
  }
  
  // 通知 WebRTC 发送器
  if (webrtc_sender_) {
    webrtc_sender_->remove_stream(stream_path);
  }
}

void MediaServer::on_video_frame(const std::string& stream_path, const EncodedFrame& frame) {
  // 推送到 WebRTC
  if (webrtc_sender_) {
    EncodedVideoFrame webrtc_frame;
    webrtc_frame.data = frame.data.data();
    webrtc_frame.size = frame.data.size();
    webrtc_frame.key_frame = frame.is_key_frame;
    webrtc_frame.pts_ms = frame.pts / 90;  // 从 90kHz 转换为 ms
    webrtc_frame.codec = "h264";
    
    webrtc_sender_->push_video_frame(stream_path, webrtc_frame);
  }
  
  // TODO: 写入 HLS（需要使用 FFmpeg 的 HLS muxer）
}

void MediaServer::on_video_frame_rtp(const std::string& stream_path, 
                                     const std::vector<uint8_t>& data,
                                     int64_t pts, bool is_key_frame) {
  // log("[RTP] 收到视频帧：" + std::to_string(data.size()) + " 字节，关键帧=" + std::to_string(is_key_frame));
  
  // 推送到 WebRTC
  if (webrtc_sender_) {
    EncodedVideoFrame webrtc_frame;
    webrtc_frame.data = data.data();
    webrtc_frame.size = data.size();
    webrtc_frame.key_frame = is_key_frame;
    webrtc_frame.pts_ms = pts;
    webrtc_frame.codec = "h264";
    
    webrtc_sender_->push_video_frame(stream_path, webrtc_frame);
  }
  
  // 写入 HLS
  if (hls_muxer_) {
    // 确保流已添加
    static std::mutex add_mutex;
    {
      std::lock_guard<std::mutex> lock(add_mutex);
      hls_muxer_->add_stream(stream_path);
    }
    
    // 写入帧
    if (!hls_muxer_->write_video_frame(stream_path, data, pts, is_key_frame)) {
      // log("[HLS] 写入失败：" + stream_path);
    }
  }
}

void MediaServer::on_audio_frame(const std::string& stream_path, const EncodedFrame& frame) {
  // TODO: 推送到 WebRTC 和 HLS
}

void MediaServer::on_sps_pps_received(const std::string& stream_path,
                                       const std::vector<uint8_t>& sps,
                                       const std::vector<uint8_t>& pps) {
  log("[RTSP] 收到 SPS/PPS: " + std::to_string(sps.size()) + "/" + 
      std::to_string(pps.size()) + " 字节");
  
  // 传递给 HLS muxer
  if (hls_muxer_) {
    hls_muxer_->set_extradata(stream_path, sps, pps);
  }
}

}  // namespace rtsp_rtmp2webrtc_hls
