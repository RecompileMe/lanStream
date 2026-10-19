#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>      // NEW: used for sleep_for below
#include <csignal>

#include "capture/capture_factory.hpp"
#include "encoder/encoder_ffmpeg.hpp"
#include "transport/udp_sender.hpp"

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run = false; }

void start_streaming(std::vector<std::string>& args)
{
    auto it = args.begin();
    std::string ip  = *it;
    it++;
    auto port = (uint16_t)std::stoi(*it);
    it++;
    int fps = std::stoi(*it);
    it++;
    int bitrate = std::stoi(*it);
    EncoderFFmpeg encoder;
    UdpSender udp;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    auto capture = create_capture();
    if (!capture->init(0))
    {
        std::cerr << "capture init failed" << std::endl;
        return;
    }

    int width = capture->width(), height = capture->height();
    std::cout << "Sender: " << width << "x" << height
              << " -> " << ip << ":" << port
              << " fps=" << fps << " bitrate=" << bitrate << "kbps" << std::endl;

    if (!encoder.init(width, height, fps, bitrate))
    {
        std::cerr << "encoder init failed" << std::endl;
        return;
    }

    if (!udp.init(ip, port))
    {
        std::cerr << "udp sender init failed" << std::endl;
        return;
    }

    capture->start([&](RawFramePtr frame)
                   {
                       if (!g_run)
                           return;

                       encoder.encode(std::move(frame), [&](EncodedPacketPtr pkt)
                       {
                           // Print the latency every 60 frames
                           // pkt->timestamp_us is now the moment the frame entered encode(), so
                           // enc_ms = real encode time (sws_scale + encoder), not ~0 anymore.
                           if (pkt->frame_id % 60 == 0)
                           {
                               double ms = (now_us() - pkt->timestamp_us) / 1000.0;
                               std::cout << "frame=" << pkt->frame_id
                                         << " size=" << pkt->data.size() << "B"
                                         << " enc_ms=" << ms
                                         << (pkt->is_keyframe ? " [I]" : "") << std::endl;
                           }
                           udp.send(pkt);
                       });
                   });

    while (g_run)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    capture->stop();
    encoder.flush([&](EncodedPacketPtr p)
                  {
                      udp.send(p);
                  });
    std::cout << "sender stopped." << std::endl;
}

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::cout << "Usage: ./sender <client_IP> <port> <fps> <bitrate_kbps>" << std::endl;
        return -1;
    }
    else
    {
        std::vector<std::string> args{};
        while(*++argv)
            args.push_back(*argv);
        start_streaming(args);
        return 0;
    }
}