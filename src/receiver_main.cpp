#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

#include "common/blocking_queue.hpp"
#include "transport/udp_receiver.hpp"
#include "decoder/decoder_ffmpeg.hpp"
#include "render/renderer_sdl.hpp"

static std::atomic<bool> g_run{true};
static void on_signal(int) { g_run = false; }

int main(int argc, char* argv[]) {
    // 用法: receiver [port=5000]
    uint16_t port = argc > 1 ? (uint16_t)std::stoi(argv[1]) : 5000;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    std::cout << "Receiver: port=" << port << "\n";

    UdpReceiver udp;
    if (!udp.init(port)) { std::cerr << "udp init failed\n"; return 1; }

    DecoderFFmpeg decoder;
    if (!decoder.init()) { std::cerr << "decoder init failed\n"; return 1; }

    // 解码线程 → 主渲染线程的帧队列
    // 容量减小到 2，降低队列尾部排队带来的延迟
    BlockingQueue<RawFramePtr> fq(2);

    udp.start([&](EncodedPacketPtr pkt) {
        decoder.decode(std::move(pkt), [&](RawFramePtr f) {
            // 解码完成的原始帧推入队列
            fq.push(f);
        });
    });

    // 渲染必须在主线程（SDL2 要求）
    RendererSDL renderer;
    bool inited = false;
    uint32_t cnt = 0;

    while (g_run) {
        if (!renderer.poll_events()) break;

        // 尝试在 10ms 内取到一帧
        auto opt = fq.pop(1);
        if (!opt) {
            // 当前没有新帧，继续处理事件
            continue;
        }

        // 先拿到一帧
        RawFramePtr latest = *opt;

        // 为了降低延迟：如果队列中还有更多帧，则把旧帧“吃掉”，只保留最新的那一帧
        // 这里受限于 BlockingQueue 只有阻塞 pop(timeout) 接口，
        // 无法完全非阻塞，但可以用一个很小的超时多尝试几次。
        while (true) {
            auto opt_more = fq.pop(0);  // 如果你的 BlockingQueue 不允许 0，可改成 1~2ms
            if (!opt_more) break;
            latest = *opt_more;
        }

        auto& f = latest;

        if (!inited) {
            renderer.init(f->width, f->height, "LanStream Receiver");
            inited = true;
        }

        if (cnt++ % 60 == 0) {
            double e2e = (now_us() - f->timestamp_us) / 1000.0;
            std::cout << "frame=" << cnt
                      << " e2e_ms=" << e2e << "\n";
        }
        renderer.render(f);
    }

    udp.stop();
    fq.close();
    std::cout << "receiver stopped.\n";
    return 0;
}
