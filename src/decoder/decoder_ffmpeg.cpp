// decoder_ffmpeg.cpp
#include "decoder_ffmpeg.hpp"
#include <iostream>

bool DecoderFFmpeg::init() {
    const char* names[] = {
#ifdef PLATFORM_WINDOWS
        "h264_cuvid", "h264_qsv",
#elif defined(PLATFORM_MACOS)
        "h264_vda", "h264_videotoolbox",
#elif defined(PLATFORM_LINUX)
        "h264_vaapi", "h264_cuvid",
#endif
        "h264", nullptr
    };
    const AVCodec* codec = nullptr;
    for (int i = 0; names[i]; ++i) {
        codec = avcodec_find_decoder_by_name(names[i]);
        if (codec) { std::cout << "[decoder] using " << names[i] << "\n"; break; }
    }
    if (!codec) return false;

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_count = 1;   // Lowest single-thread latency

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
        std::cerr << "[decoder] avcodec_open2 failed\n"; return false;
    }
    avf_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    return true;
}

void DecoderFFmpeg::decode(EncodedPacketPtr ep, FrameCallback cb) {
    if (!ep) return;
    pkt_->data = const_cast<uint8_t*>(ep->data.data());
    pkt_->size = (int)ep->data.size();
    if (avcodec_send_packet(ctx_, pkt_) < 0) return;

    while (true) {
        if (avcodec_receive_frame(ctx_, avf_) < 0) break;
        int w = avf_->width, h = avf_->height;
        if (!sws_ || w != sw_ || h != sh_) {
            if (sws_) sws_freeContext(sws_);
            sw_ = w; sh_ = h;
            sws_ = sws_getContext(w, h, (AVPixelFormat)avf_->format,
                                  w, h, AV_PIX_FMT_BGRA,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }
        auto f = std::make_shared<RawFrame>();
        f->timestamp_us = ep->timestamp_us;
        f->width    = w; f->height = h;
        f->linesize = w * 4;
        f->format   = PixelFormat::BGRA;
        f->data.resize(w * 4 * h);
        uint8_t* dst[1]  = { f->data.data() };
        int dst_ls[1]    = { w * 4 };
        sws_scale(sws_, avf_->data, avf_->linesize, 0, h, dst, dst_ls);
        av_frame_unref(avf_);
        cb(std::move(f));
    }
}

void DecoderFFmpeg::cleanup() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (avf_) av_frame_free(&avf_);
    if (pkt_) av_packet_free(&pkt_);
    if (sws_) sws_freeContext(sws_);
}
