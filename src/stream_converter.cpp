#include "stream_converter.hpp"
#include "webrtc_sender.hpp"

#include <chrono>
#include <sstream>
#include <thread>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

namespace rtsp_rtmp2webrtc_hls {

StreamConverter::StreamConverter(Config config)
    : config_(std::move(config)) {}

StreamConverter::~StreamConverter() {
  stop();
}

void StreamConverter::log(const std::string& msg) const {
  if (log_cb_) {
    log_cb_(msg);
  }
}

void StreamConverter::cleanup() {
  if (output_ctx_) {
    if (output_ctx_->pb) {
      avio_closep(&output_ctx_->pb);
    }
    avformat_free_context(output_ctx_);
    output_ctx_ = nullptr;
  }
  if (input_ctx_) {
    avformat_close_input(&input_ctx_);
    input_ctx_ = nullptr;
  }
  if (packet_) {
    av_packet_free(&packet_);
    packet_ = nullptr;
  }
}

bool StreamConverter::open_input() {
  AVDictionary* opts = nullptr;

  // RTSP 偏好 TCP 传输，减少丢包
  av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  av_dict_set(&opts, "stimeout", "5000000", 0);  // 5 秒超时
  av_dict_set(&opts, "max_delay", "500000", 0);  // 减少缓冲延迟

  int ret = avformat_open_input(&input_ctx_, config_.input_url.c_str(), nullptr, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    log("打开输入失败：" + std::string(errbuf));
    return false;
  }

  // 获取流信息
  ret = avformat_find_stream_info(input_ctx_, nullptr);
  if (ret < 0) {
    log("获取流信息失败");
    avformat_close_input(&input_ctx_);
    input_ctx_ = nullptr;
    return false;
  }

  std::ostringstream oss;
  oss << "输入流已打开："
      << "nb_streams=" << input_ctx_->nb_streams;
  log(oss.str());

  // 初始化流索引映射
  stream_index_map_.resize(input_ctx_->nb_streams, -1);

  return true;
}

bool StreamConverter::open_hls_output() {
  if (!input_ctx_) {
    log("输入上下文为空");
    return false;
  }

  const std::string playlist_path =
      config_.hls_output_dir + "/" + config_.hls_playlist_name;
  const std::string segment_pattern =
      config_.hls_output_dir + "/segment_%03d.ts";

  int ret = avformat_alloc_output_context2(&output_ctx_, nullptr, "hls", playlist_path.c_str());
  if (ret < 0 || !output_ctx_) {
    log("创建 HLS 输出上下文失败");
    return false;
  }

  // 配置 HLS 参数
  av_opt_set(output_ctx_->priv_data, "hls_time",
             std::to_string(config_.hls_segment_duration_sec).c_str(), 0);
  av_opt_set(output_ctx_->priv_data, "hls_list_size",
             std::to_string(config_.hls_list_size).c_str(), 0);
  av_opt_set(output_ctx_->priv_data, "hls_segment_filename",
             segment_pattern.c_str(), 0);
  av_opt_set(output_ctx_->priv_data, "hls_flags", "delete_segments", 0);
  av_opt_set(output_ctx_->priv_data, "hls_allow_cache", "1", 0);

  // 为每个输入流创建对应的输出流
  for (unsigned i = 0; i < input_ctx_->nb_streams; ++i) {
    AVStream* in_st = input_ctx_->streams[i];

    // 只处理视频和音频
    if (in_st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
        in_st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      continue;
    }

    AVStream* out_st = avformat_new_stream(output_ctx_, nullptr);
    if (!out_st) {
      log("创建输出流失败");
      return false;
    }

    ret = avcodec_parameters_copy(out_st->codecpar, in_st->codecpar);
    if (ret < 0) {
      log("复制编码参数失败");
      return false;
    }

    out_st->time_base = in_st->time_base;
    stream_index_map_[i] = static_cast<int>(out_st->index);
  }

  // 打开输出文件
  if (!(output_ctx_->oformat->flags & AVFMT_NOFILE)) {
    ret = avio_open2(&output_ctx_->pb, playlist_path.c_str(),
                     AVIO_FLAG_WRITE, nullptr, nullptr);
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, sizeof(errbuf));
      log("打开输出文件失败：" + std::string(errbuf));
      return false;
    }
  }

  // 写入文件头
  ret = avformat_write_header(output_ctx_, nullptr);
  if (ret < 0) {
    log("写入文件头失败");
    return false;
  }

  log("HLS 输出已打开：" + playlist_path);
  return true;
}

bool StreamConverter::run_loop() {
  if (!input_ctx_ || !output_ctx_) {
    return false;
  }

  // 分配数据包
  packet_ = av_packet_alloc();
  if (!packet_) {
    log("分配数据包失败");
    return false;
  }

  int64_t frame_count = 0;
  int64_t last_log_time = av_gettime_relative();

  while (running_.load()) {
    int ret = av_read_frame(input_ctx_, packet_);

    if (ret == AVERROR_EOF) {
      log("输入流结束 (EOF)");
      break;
    }

    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, sizeof(errbuf));
      log("读取帧失败：" + std::string(errbuf));
      break;
    }

    // 获取输入流索引
    unsigned input_idx = packet_->stream_index;
    if (input_idx >= stream_index_map_.size()) {
      av_packet_unref(packet_);
      continue;
    }

    // 获取输出流索引
    int output_idx = stream_index_map_[input_idx];
    if (output_idx < 0) {
      av_packet_unref(packet_);
      continue;
    }

    AVStream* in_st = input_ctx_->streams[input_idx];
    AVStream* out_st = output_ctx_->streams[output_idx];

    // 更新数据包流索引
    packet_->stream_index = output_idx;

    // 时间戳转换（从输入时间基到输出时间基）
    packet_->pts = av_rescale_q_rnd(
        packet_->pts, in_st->time_base, out_st->time_base,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    packet_->dts = av_rescale_q_rnd(
        packet_->dts, in_st->time_base, out_st->time_base,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    packet_->duration = av_rescale_q(packet_->duration, in_st->time_base, out_st->time_base);

    // 如果是视频帧且有 WebRTC 发送器，推送帧数据
    if (webrtc_sender_ && in_st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      EncodedVideoFrame frame;
      frame.data = packet_->data;
      frame.size = packet_->size;
      frame.key_frame = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
      frame.pts_ms = packet_->pts * 1000 / out_st->time_base.den;
      frame.codec = (in_st->codecpar->codec_id == AV_CODEC_ID_H264) ? "h264" :
                    (in_st->codecpar->codec_id == AV_CODEC_ID_H265) ? "h265" : "unknown";

      webrtc_sender_->push_video_frame(config_.stream_id, frame);
    }

    // 写入输出
    ret = av_interleaved_write_frame(output_ctx_, packet_);
    av_packet_unref(packet_);

    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, sizeof(errbuf));
      log("写入帧失败：" + std::string(errbuf));
      break;
    }

    ++frame_count;

    // 定期输出统计信息
    int64_t now = av_gettime_relative();
    if (now - last_log_time > 10000000) {  // 每 10 秒
      std::ostringstream oss;
      oss << "已处理帧数：" << frame_count;
      log(oss.str());
      last_log_time = now;
    }
  }

  log("拉流循环结束");
  return true;
}

void StreamConverter::stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);

  // 等待工作线程结束
  if (worker_.joinable()) {
    worker_.join();
  }

  cleanup();
  log("已停止");
}

bool StreamConverter::start() {
  if (running_.load()) {
    log("已经在运行中");
    return true;
  }

  running_.store(true);

  worker_ = std::thread([this]() {
    while (running_.load()) {
      // 打开输入
      if (!open_input()) {
        log("打开输入失败，5 秒后重试...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        cleanup();
        continue;
      }

      // 打开 HLS 输出
      if (!open_hls_output()) {
        log("打开 HLS 输出失败，5 秒后重试...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        cleanup();
        continue;
      }

      // 运行拉流循环
      run_loop();

      // 清理资源
      cleanup();

      // 如果仍在运行，等待后重连
      if (running_.load()) {
        log("3 秒后重连...");
        std::this_thread::sleep_for(std::chrono::seconds(3));
      }
    }
  });

  log("已启动");
  return true;
}

}  // namespace rtsp_rtmp2webrtc_hls
