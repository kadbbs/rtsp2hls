#include "ffmpeg_ingest.h"

#include <cstring>
#include <utility>

#if AVTRANS_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif

namespace avtrans {

FFmpegIngest::FFmpegIngest() = default;

FFmpegIngest::~FFmpegIngest() {
  Stop();
}

bool FFmpegIngest::Start(const std::string& input_url, PacketHandler handler) {
  return Start(input_url, std::move(handler), nullptr);
}

bool FFmpegIngest::Start(const std::string& input_url,
                         PacketHandler packet_handler,
                         VideoFrameHandler video_handler) {
  if (running_.exchange(true)) {
    return false;
  }
  worker_ = std::thread(&FFmpegIngest::Run,
                        this,
                        input_url,
                        std::move(packet_handler),
                        std::move(video_handler));
  return true;
}

void FFmpegIngest::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void FFmpegIngest::Run(std::string input_url,
                       PacketHandler packet_handler,
                       VideoFrameHandler video_handler) {
#if !AVTRANS_HAS_FFMPEG
  Log("FFmpeg support is disabled. Rebuild with -DENABLE_FFMPEG=ON.");
  running_ = false;
  return;
#else
  AVFormatContext* fmt = nullptr;
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  av_dict_set(&opts, "stimeout", "5000000", 0);

  Log("Opening input: " + input_url);
  int rc = avformat_open_input(&fmt, input_url.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    Log("avformat_open_input failed: " + std::to_string(rc));
    running_ = false;
    return;
  }

  rc = avformat_find_stream_info(fmt, nullptr);
  if (rc < 0) {
    Log("avformat_find_stream_info failed: " + std::to_string(rc));
    avformat_close_input(&fmt);
    running_ = false;
    return;
  }

  int video_stream = -1;
  int audio_stream = -1;
  for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
    const AVStream* stream = fmt->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) {
      video_stream = static_cast<int>(i);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream < 0) {
      audio_stream = static_cast<int>(i);
    }
  }

  Log("Input opened. video_stream=" + std::to_string(video_stream) +
      " audio_stream=" + std::to_string(audio_stream));

  AVCodecContext* video_dec_ctx = nullptr;
  SwsContext* sws_ctx = nullptr;
  AVFrame* decoded = nullptr;
  AVFrame* yuv420 = nullptr;

  if (video_stream >= 0 && video_handler) {
    const AVCodecParameters* codecpar = fmt->streams[video_stream]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (decoder == nullptr) {
      Log("No video decoder found for codec_id=" + std::to_string(codecpar->codec_id));
    } else {
      video_dec_ctx = avcodec_alloc_context3(decoder);
      if (video_dec_ctx == nullptr) {
        Log("avcodec_alloc_context3 failed for video decoder.");
      } else if (avcodec_parameters_to_context(video_dec_ctx, codecpar) < 0) {
        Log("avcodec_parameters_to_context failed for video decoder.");
        avcodec_free_context(&video_dec_ctx);
      } else if (avcodec_open2(video_dec_ctx, decoder, nullptr) < 0) {
        Log("avcodec_open2 failed for video decoder.");
        avcodec_free_context(&video_dec_ctx);
      } else {
        decoded = av_frame_alloc();
        yuv420 = av_frame_alloc();
        Log("Video decoder initialized for WebRTC video source.");
      }
    }
  }

  AVPacket pkt;
  av_init_packet(&pkt);

  while (running_) {
    rc = av_read_frame(fmt, &pkt);
    if (rc < 0) {
      Log("av_read_frame stopped: " + std::to_string(rc));
      break;
    }

    PacketInfo info;
    info.stream_index = pkt.stream_index;
    info.pts = pkt.pts;
    info.dts = pkt.dts;
    info.duration = pkt.duration;
    info.key_frame = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
    info.payload.assign(pkt.data, pkt.data + pkt.size);

    if (pkt.stream_index == video_stream) {
      info.kind = PacketInfo::Kind::kVideo;
    } else if (pkt.stream_index == audio_stream) {
      info.kind = PacketInfo::Kind::kAudio;
    }

    if (packet_handler) {
      packet_handler(info);
    }

    if (video_handler && video_dec_ctx != nullptr && pkt.stream_index == video_stream) {
      const AVRational time_base = fmt->streams[video_stream]->time_base;
      if (avcodec_send_packet(video_dec_ctx, &pkt) >= 0) {
        while (avcodec_receive_frame(video_dec_ctx, decoded) >= 0) {
          AVFrame* output = decoded;
          if (decoded->format != AV_PIX_FMT_YUV420P) {
            sws_ctx = sws_getCachedContext(sws_ctx,
                                           decoded->width,
                                           decoded->height,
                                           static_cast<AVPixelFormat>(decoded->format),
                                           decoded->width,
                                           decoded->height,
                                           AV_PIX_FMT_YUV420P,
                                           SWS_BILINEAR,
                                           nullptr,
                                           nullptr,
                                           nullptr);
            if (sws_ctx == nullptr) {
              Log("sws_getCachedContext failed.");
              break;
            }

            av_frame_unref(yuv420);
            yuv420->format = AV_PIX_FMT_YUV420P;
            yuv420->width = decoded->width;
            yuv420->height = decoded->height;
            if (av_frame_get_buffer(yuv420, 32) < 0) {
              Log("av_frame_get_buffer failed for yuv420 conversion.");
              break;
            }

            sws_scale(sws_ctx,
                      decoded->data,
                      decoded->linesize,
                      0,
                      decoded->height,
                      yuv420->data,
                      yuv420->linesize);
            output = yuv420;
          }

          VideoFrameInfo frame;
          frame.width = output->width;
          frame.height = output->height;
          frame.stride_y = output->linesize[0];
          frame.stride_u = output->linesize[1];
          frame.stride_v = output->linesize[2];

          const int64_t best_effort_pts = decoded->best_effort_timestamp == AV_NOPTS_VALUE
                                              ? 0
                                              : decoded->best_effort_timestamp;
          frame.timestamp_us = av_rescale_q(best_effort_pts, time_base, AVRational{1, 1000000});

          frame.data_y.resize(static_cast<size_t>(frame.stride_y * frame.height));
          frame.data_u.resize(static_cast<size_t>(frame.stride_u * ((frame.height + 1) / 2)));
          frame.data_v.resize(static_cast<size_t>(frame.stride_v * ((frame.height + 1) / 2)));

          std::memcpy(frame.data_y.data(), output->data[0], frame.data_y.size());
          std::memcpy(frame.data_u.data(), output->data[1], frame.data_u.size());
          std::memcpy(frame.data_v.data(), output->data[2], frame.data_v.size());

          video_handler(frame);
          av_frame_unref(decoded);
        }
      }
    }
    av_packet_unref(&pkt);
  }

  if (video_dec_ctx != nullptr) {
    avcodec_send_packet(video_dec_ctx, nullptr);
    while (decoded != nullptr && avcodec_receive_frame(video_dec_ctx, decoded) >= 0) {
      av_frame_unref(decoded);
    }
  }

  if (yuv420 != nullptr) {
    av_frame_free(&yuv420);
  }
  if (decoded != nullptr) {
    av_frame_free(&decoded);
  }
  if (sws_ctx != nullptr) {
    sws_freeContext(sws_ctx);
  }
  if (video_dec_ctx != nullptr) {
    avcodec_free_context(&video_dec_ctx);
  }
  avformat_close_input(&fmt);
  running_ = false;
#endif
}

}  // namespace avtrans
