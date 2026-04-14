#include "webrtc_shim.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "media/base/adapted_video_track_source.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

namespace {

class FfmpegVideoSource : public webrtc::AdaptedVideoTrackSource {
 public:
  FfmpegVideoSource() : webrtc::AdaptedVideoTrackSource(/*required_alignment=*/2) {}

  SourceState state() const override { return SourceState::kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return false; }
  std::optional<bool> needs_denoising() const override { return std::nullopt; }

  void PushFrame(const avtrans_webrtc_i420_frame& frame) {
    auto buffer = webrtc::I420Buffer::Copy(frame.width,
                                           frame.height,
                                           frame.data_y,
                                           frame.stride_y,
                                           frame.data_u,
                                           frame.stride_u,
                                           frame.data_v,
                                           frame.stride_v);
    webrtc::VideoFrame video_frame =
        webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(buffer)
            .set_rotation(webrtc::kVideoRotation_0)
            .set_timestamp_us(frame.timestamp_us)
            .build();
    OnFrame(video_frame);
  }
};

class CreateDescObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::lock_guard<std::mutex> lock(mu_);
    desc_.reset(desc);
    done_ = true;
    cv_.notify_all();
  }

  void OnFailure(webrtc::RTCError error) override {
    std::lock_guard<std::mutex> lock(mu_);
    error_ = std::string(error.message());
    done_ = true;
    cv_.notify_all();
  }

  bool Wait(std::unique_ptr<webrtc::SessionDescriptionInterface>* desc_out,
            std::string* error_out) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return done_; });
    if (!error_.empty()) {
      if (error_out != nullptr) {
        *error_out = error_;
      }
      return false;
    }
    if (desc_out != nullptr) {
      *desc_out = std::move(desc_);
    }
    return true;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::string error_;
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc_;
};

class SetLocalDescObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    std::lock_guard<std::mutex> lock(mu_);
    ok_ = error.ok();
    error_ = std::string(error.message());
    done_ = true;
    cv_.notify_all();
  }

  bool Wait(std::string* error_out) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return done_; });
    if (!ok_ && error_out != nullptr) {
      *error_out = error_;
    }
    return ok_;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  bool ok_ = false;
  std::string error_;
};

class SetRemoteDescObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    std::lock_guard<std::mutex> lock(mu_);
    ok_ = error.ok();
    error_ = std::string(error.message());
    done_ = true;
    cv_.notify_all();
  }

  bool Wait(std::string* error_out) {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return done_; });
    if (!ok_ && error_out != nullptr) {
      *error_out = error_;
    }
    return ok_;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  bool ok_ = false;
  std::string error_;
};

class PeerSession : public webrtc::PeerConnectionObserver {
 public:
  PeerSession(webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
              webrtc::scoped_refptr<FfmpegVideoSource> source)
      : factory_(std::move(factory)), source_(std::move(source)) {}

  bool CreateAnswer(const std::string& remote_offer_sdp, std::string* out_answer, std::string* out_error) {
    if (!EnsurePeerConnection(out_error)) {
      return false;
    }

    ResetNegotiationState();

    webrtc::SdpParseError parse_error;
    auto remote_offer =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, remote_offer_sdp, &parse_error);
    if (!remote_offer) {
      if (out_error != nullptr) {
        *out_error = "remote SDP parse failed: " + parse_error.description;
      }
      return false;
    }

    auto remote_observer = webrtc::make_ref_counted<SetRemoteDescObserver>();
    peer_connection_->SetRemoteDescription(std::move(remote_offer), remote_observer);

    std::string error;
    if (!remote_observer->Wait(&error)) {
      if (out_error != nullptr) {
        *out_error = "SetRemoteDescription failed: " + error;
      }
      return false;
    }

    auto create_observer = webrtc::make_ref_counted<CreateDescObserver>();
    peer_connection_->CreateAnswer(
        create_observer.get(),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());

    std::unique_ptr<webrtc::SessionDescriptionInterface> answer;
    if (!create_observer->Wait(&answer, &error)) {
      if (out_error != nullptr) {
        *out_error = "CreateAnswer failed: " + error;
      }
      return false;
    }

    auto local_observer = webrtc::make_ref_counted<SetLocalDescObserver>();
    peer_connection_->SetLocalDescription(std::move(answer), local_observer);
    if (!local_observer->Wait(&error)) {
      if (out_error != nullptr) {
        *out_error = "SetLocalDescription failed: " + error;
      }
      return false;
    }

    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::seconds(8), [this] {
      return ice_complete_ && !latest_local_sdp_.empty();
    });

    if (latest_local_sdp_.empty()) {
      if (out_error != nullptr) {
        *out_error = "local SDP was not produced";
      }
      return false;
    }

    if (out_answer != nullptr) {
      *out_answer = latest_local_sdp_;
    }
    return true;
  }

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ice_complete_ = new_state == webrtc::PeerConnectionInterface::kIceGatheringComplete;
      CaptureLocalDescription();
    }
    cv_.notify_all();
  }
  void OnIceCandidate(const webrtc::IceCandidate*) override {
    std::lock_guard<std::mutex> lock(mu_);
    CaptureLocalDescription();
    cv_.notify_all();
  }

 private:
  void ResetNegotiationState() {
    std::lock_guard<std::mutex> lock(mu_);
    latest_local_sdp_.clear();
    ice_complete_ = false;
  }

  void CaptureLocalDescription() {
    const webrtc::SessionDescriptionInterface* desc = peer_connection_->local_description();
    if (desc == nullptr) {
      return;
    }
    std::string sdp;
    if (desc->ToString(&sdp)) {
      latest_local_sdp_ = std::move(sdp);
    }
  }

  bool EnsurePeerConnection(std::string* out_error) {
    if (peer_connection_ != nullptr) {
      return true;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    webrtc::PeerConnectionDependencies dependencies(this);
    auto result = factory_->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!result.ok()) {
      if (out_error != nullptr) {
        *out_error = std::string("CreatePeerConnectionOrError failed: ") + result.error().message();
      }
      return false;
    }
    peer_connection_ = result.MoveValue();

    auto track = factory_->CreateVideoTrack(source_, "avtrans-video");
    auto add_result = peer_connection_->AddTrack(track, {"avtrans-stream"});
    if (!add_result.ok()) {
      if (out_error != nullptr) {
        *out_error = std::string("AddTrack failed: ") + add_result.error().message();
      }
      return false;
    }
    return true;
  }

  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<FfmpegVideoSource> source_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool ice_complete_ = false;
  std::string latest_local_sdp_;
};

struct WebRtcShimState {
  std::unique_ptr<webrtc::Thread> network_thread;
  std::unique_ptr<webrtc::Thread> worker_thread;
  std::unique_ptr<webrtc::Thread> signaling_thread;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  webrtc::scoped_refptr<FfmpegVideoSource> video_source;
  std::mutex mu;
  std::deque<std::shared_ptr<PeerSession>> sessions;
  bool ssl_initialized = false;
};

}  // namespace

extern "C" {

struct avtrans_webrtc_handle {
  WebRtcShimState* state;
};

avtrans_webrtc_handle* avtrans_webrtc_create(void) {
  auto* handle = new avtrans_webrtc_handle();
  handle->state = new WebRtcShimState();
  return handle;
}

void avtrans_webrtc_destroy(avtrans_webrtc_handle* handle) {
  if (handle == nullptr) {
    return;
  }
  if (handle->state != nullptr) {
    handle->state->sessions.clear();
    if (handle->state->signaling_thread) {
      handle->state->signaling_thread->Stop();
    }
    if (handle->state->worker_thread) {
      handle->state->worker_thread->Stop();
    }
    if (handle->state->network_thread) {
      handle->state->network_thread->Stop();
    }
    if (handle->state->ssl_initialized) {
      webrtc::CleanupSSL();
    }
    delete handle->state;
  }
  delete handle;
}

int avtrans_webrtc_init(avtrans_webrtc_handle* handle) {
  if (handle == nullptr || handle->state == nullptr) {
    return 0;
  }
  if (!webrtc::InitializeSSL()) {
    return 0;
  }
  handle->state->ssl_initialized = true;

  handle->state->network_thread = webrtc::Thread::CreateWithSocketServer();
  handle->state->worker_thread = webrtc::Thread::Create();
  handle->state->signaling_thread = webrtc::Thread::Create();

  handle->state->network_thread->SetName("avtrans-network", nullptr);
  handle->state->worker_thread->SetName("avtrans-worker", nullptr);
  handle->state->signaling_thread->SetName("avtrans-signaling", nullptr);

  if (!handle->state->network_thread->Start() ||
      !handle->state->worker_thread->Start() ||
      !handle->state->signaling_thread->Start()) {
    return 0;
  }

  handle->state->video_source = webrtc::make_ref_counted<FfmpegVideoSource>();
  auto audio_processing =
      webrtc::BuiltinAudioProcessingBuilder().Build(webrtc::CreateEnvironment());
  handle->state->factory = webrtc::CreatePeerConnectionFactory(
      handle->state->network_thread.get(),
      handle->state->worker_thread.get(),
      handle->state->signaling_thread.get(),
      nullptr,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(),
      nullptr,
      audio_processing);

  return handle->state->factory != nullptr ? 1 : 0;
}

int avtrans_webrtc_push_i420(avtrans_webrtc_handle* handle,
                             const avtrans_webrtc_i420_frame* frame) {
  if (handle == nullptr || handle->state == nullptr || handle->state->video_source == nullptr ||
      frame == nullptr) {
    return 0;
  }
  handle->state->video_source->PushFrame(*frame);
  return 1;
}

char* avtrans_webrtc_create_answer(avtrans_webrtc_handle* handle,
                                   const char* remote_offer_sdp,
                                   int* ok,
                                   const char** content_type) {
  if (ok != nullptr) {
    *ok = 0;
  }
  if (content_type != nullptr) {
    *content_type = "application/json";
  }
  if (handle == nullptr || handle->state == nullptr || handle->state->factory == nullptr ||
      handle->state->video_source == nullptr ||
      remote_offer_sdp == nullptr) {
    const char* error = "{\n  \"error\": \"WebRTC shim is not initialized\"\n}\n";
    char* out = static_cast<char*>(std::malloc(std::strlen(error) + 1));
    std::memcpy(out, error, std::strlen(error) + 1);
    return out;
  }

  auto session =
      std::make_shared<PeerSession>(handle->state->factory, handle->state->video_source);
  std::string answer;
  std::string error;
  const bool success = session->CreateAnswer(remote_offer_sdp, &answer, &error);
  if (success) {
    std::lock_guard<std::mutex> lock(handle->state->mu);
    handle->state->sessions.push_back(session);
    while (handle->state->sessions.size() > 32) {
      handle->state->sessions.pop_front();
    }
    if (ok != nullptr) {
      *ok = 1;
    }
    if (content_type != nullptr) {
      *content_type = "application/sdp";
    }
    char* out = static_cast<char*>(std::malloc(answer.size() + 1));
    std::memcpy(out, answer.c_str(), answer.size() + 1);
    return out;
  }

  std::string body = "{\n  \"error\": \"" + error + "\"\n}\n";
  char* out = static_cast<char*>(std::malloc(body.size() + 1));
  std::memcpy(out, body.c_str(), body.size() + 1);
  return out;
}

void avtrans_webrtc_free_string(char* s) {
  std::free(s);
}

}  // extern "C"
