#include "encoder_ffmpeg.hpp"
#include <iostream>
#include <vector>
#include <cstring>

bool EncoderFFmpeg::init(int width, int height, int fps, int bitrate_kbps)
{
    width_ = width; height_ = height;

    // Prioritize hardware encoding based on the platform, with a final fallback to libx264.
    std::vector<std::string> names = {
#ifdef PLATFORM_WINDOWS
            "h264_nvenc", "h264_qsv", "h264_amf",
#elif defined(PLATFORM_MACOS)
            "h264_videotoolbox",
#elif defined(PLATFORM_LINUX)
            "h264_vaapi", "h264_nvenc",
#endif
            "libx264", ""
    };
/*
    const char* names[] = {
#ifdef PLATFORM_WINDOWS
        "h264_nvenc", "h264_qsv", "h264_amf",
#elif defined(PLATFORM_MACOS)
        "h264_videotoolbox",
#elif defined(PLATFORM_LINUX)
        "h264_vaapi", "h264_nvenc",
#endif
        "libx264", nullptr
    };
*/
    const AVCodec* codec = nullptr;
    for (auto &name: names)
    {
        codec = avcodec_find_encoder_by_name(name.c_str());
        if (codec)
        {
            std::cout << "[encoder] using " << name << std::endl;
            break;
        }
    }

    if (!codec)
    {
        std::cerr << "[encoder] no codec" << std::endl;
        return false;
    }

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width       = width;
    ctx_->height      = height;
    ctx_->time_base   = {1, fps};
    ctx_->framerate   = {fps, 1};
    ctx_->bit_rate    = bitrate_kbps * 1000LL;
    ctx_->gop_size    = 1;           // All I-frames, lowest latency
    ctx_->max_b_frames = 0;
    ctx_->pix_fmt     = AV_PIX_FMT_YUV420P;

    av_opt_set(ctx_->priv_data, "preset",  "ultrafast",   0);
    av_opt_set(ctx_->priv_data, "tune",    "zerolatency", 0);
    av_opt_set(ctx_->priv_data, "profile", "baseline",    0);

    if (avcodec_open2(ctx_, codec, nullptr) < 0)
    {
        std::cerr << "[encoder] avcodec_open2 failed\n";
        return false;
    }

    avf_ = av_frame_alloc();
    avf_->format = AV_PIX_FMT_YUV420P;
    avf_->width  = width;
    avf_->height = height;
    av_frame_get_buffer(avf_, 32);

    pkt_ = av_packet_alloc();

    sws_ = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                          width, height, AV_PIX_FMT_YUV420P,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    return sws_ != nullptr;
}

void EncoderFFmpeg::do_encode(AVFrame* frame, PacketCallback& cb)
{
    if (avcodec_send_frame(ctx_, frame) < 0)
        return;

    while (true)
    {
        int ret = avcodec_receive_packet(ctx_, pkt_);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;

        if (ret < 0)
            break;

        auto ep = std::make_shared<EncodedPacket>();
        ep->frame_id     = fid_++;
        ep->timestamp_us = now_us();
        ep->is_keyframe  = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        ep->data.assign(pkt_->data, pkt_->data + pkt_->size);
        av_packet_unref(pkt_);
        cb(std::move(ep));
    }
}

void EncoderFFmpeg::encode(RawFramePtr raw, PacketCallback cb)
{
    if (!raw || raw->data.empty())
        return;

    const uint8_t* src[1]  = { raw->data.data() };
    int  src_ls[1]          = { raw->linesize };
    av_frame_make_writable(avf_);
    sws_scale(sws_, src, src_ls, 0, height_,
              avf_->data, avf_->linesize);
    avf_->pts = fid_;
    do_encode(avf_, cb);
}

void EncoderFFmpeg::flush(PacketCallback cb)
{
    do_encode(nullptr, cb);
}

EncoderFFmpeg::~EncoderFFmpeg()
{
    cleanup();
}

void EncoderFFmpeg::cleanup()
{
    if (ctx_)
        avcodec_free_context(&ctx_);

    if (avf_)
        av_frame_free(&avf_);

    if (pkt_)
        av_packet_free(&pkt_);

    if (sws_)
        sws_freeContext(sws_);

}

