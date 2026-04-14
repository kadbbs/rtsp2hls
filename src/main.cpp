#include "ffmpeg_ingest.h"
#include "signaling_server.h"
#include "webrtc_bridge.h"

#include <csignal>
#include <iostream>

namespace avtrans {

namespace {
std::atomic<bool> g_running {true};

void OnSignal(int) {
  g_running = false;
}

std::optional<AppConfig> ParseArgs(int argc, char** argv) {
  AppConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--listen" && i + 1 < argc) {
      cfg.listen_host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      cfg.port = std::stoi(argv[++i]);
    } else if (arg == "--input" && i + 1 < argc) {
      cfg.input_url = argv[++i];
    } else if (arg == "--help") {
      std::cout
          << "Usage: avtrans [--listen 0.0.0.0] [--port 8080] [--input rtsp://...]\n";
      return std::nullopt;
    }
  }
  return cfg;
}
}  // namespace

}  // namespace avtrans

int main(int argc, char** argv) {
  using namespace avtrans;

  const auto cfg = ParseArgs(argc, argv);
  if (!cfg.has_value()) {
    return 0;
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  WebRtcBridge bridge;
  if (!bridge.Init()) {
    Log("Failed to init WebRTC bridge.");
    return 1;
  }

  SignalingServer signaling(bridge);
  if (!signaling.Start(cfg->listen_host, cfg->port)) {
    Log("Failed to start signaling server.");
    return 1;
  }

  FFmpegIngest ingest;
  if (!cfg->input_url.empty()) {
    ingest.Start(
        cfg->input_url,
        [&bridge](const PacketInfo& packet) {
          bridge.PushPacket(packet);
        },
        [&bridge](const VideoFrameInfo& frame) {
          bridge.PushVideoFrame(frame);
        });
  } else {
    Log("No input URL provided. Running signaling server only.");
  }

  Log("avtrans is running. Press Ctrl+C to stop.");
  while (g_running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  ingest.Stop();
  signaling.Stop();
  Log("avtrans stopped.");
  return 0;
}
