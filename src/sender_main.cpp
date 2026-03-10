#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

#include "capture/capture_factory.hpp"
#include "encoder/encoder_ffmpeg.hpp"
#include "transport/udp_sender.hpp"

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run = false; }

int main(int argc, char* argv[]) {
    // 用法: sender <副机IP> [port=5000] [fps=60] [bitrate_kbps=8000]
    std::string ip  = argc > 1 ? argv[1] : "127.0.0.1";
    uint16_t port   = argc > 2 ? (uint16_t)std::stoi(argv[2]) : 5000;
    int fps         = argc > 3 ? std::stoi(argv[3]) : 120;
    int bitrate     = argc > 4 ? std::stoi(argv[4]) : 8000;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    auto capture = create_capture();
    if (!capture->init(0)) { std::cerr << "capture init failed\n"; return 1; }

    int w = capture->width(), h = capture->height();
    std::cout << "Sender: " << w << "x" << h
              << " -> " << ip << ":" << port
              << " fps=" << fps << " bitrate=" << bitrate << "kbps\n";

    EncoderFFmpeg encoder;
    if (!encoder.init(w, h, fps, bitrate)) {
        std::cerr << "encoder init failed\n"; return 1;
    }

    UdpSender udp;
    if (!udp.init(ip, port)) {
        std::cerr << "udp sender init failed\n"; return 1;
    }

    capture->start([&](RawFramePtr frame) {
        if (!g_run) return;
        encoder.encode(std::move(frame), [&](EncodedPacketPtr pkt) {
            // 每 60 帧打印一次延迟（采集→编码完成）
            if (pkt->frame_id % 60 == 0) {
                double ms = (now_us() - pkt->timestamp_us) / 1000.0;
                std::cout << "frame=" << pkt->frame_id
                          << " size=" << pkt->data.size() << "B"
                          << " enc_ms=" << ms
                          << (pkt->is_keyframe ? " [I]" : "") << "\n";
            }
            udp.send(pkt);
        });
    });

    while (g_run) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    capture->stop();
    encoder.flush([&](EncodedPacketPtr p) { udp.send(p); });
    std::cout << "sender stopped.\n";
    return 0;
}
