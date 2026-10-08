#include "decoder_ffmpeg.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

std::string av_err(int errnum)
{
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(errnum, buf, sizeof(buf));
        return buf;
}

// FFmpeg calls this once it has parsed the stream header and has to pick the pixel
// format to decode into. Returning AV_PIX_FMT_VAAPI is what switches GPU decoding on.
// If the driver cannot decode this stream (unsupported profile, hwaccel setup
// failed, ...) VAAPI is missing from the list, and we fall back to normal software
// decoding instead of failing.
AVPixelFormat get_vaapi_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts)
{
        for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        {
            if (*p == AV_PIX_FMT_VAAPI)
                return AV_PIX_FMT_VAAPI;
        }

        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::cerr << "[decoder] VAAPI cannot decode this stream, falling back to software" << std::endl;
        }
        return avcodec_default_get_format(ctx, pix_fmts);
}

bool DecoderFFmpeg::init()
{
    // Prioritize hardware decoding based on platform, falling back to standard software h264
    const std::vector<std::string> names = {
#ifdef PLATFORM_WINDOWS
            "h264_cuvid", "h264_qsv",
#elif defined(PLATFORM_MACOS)
            "h264_vda", "h264_videotoolbox",
#elif defined(PLATFORM_LINUX)
            "h264", "h264_cuvid"
#endif
            "h264"
    };

    for (const auto& name : names)
    {
        const AVCodec* codec = avcodec_find_decoder_by_name(name.c_str());
        if (!codec)
            continue;

        if (open_codec(codec))
        {
            std::cout << "[decoder] using " << name;
            if (use_hw_)
                std::cout << " (GPU)" << std::endl;
            else
                std::cout << " (CPU)" << std::endl;
            return true;
        }
        std::cerr << "[decoder] " << name << " not usable, trying next" << std::endl;
        cleanup();
    }
    std::cerr << "[decoder] no usable decoder found" << std::endl;
    return false;
}

bool DecoderFFmpeg::setup_vaapi()
{
    // Override the render node with e.g. LANSTREAM_VAAPI_DEVICE=/dev/dri/renderD129
    const char* env = std::getenv("LANSTREAM_VAAPI_DEVICE");
    const std::string device = [&]() {
        if (env && *env)
            return env;
        else
            return "/dev/dri/renderD128";
    }();

    const int ret = av_hwdevice_ctx_create(&hw_dev_, AV_HWDEVICE_TYPE_VAAPI,
                                           device.c_str(), nullptr, 0);
    if (ret < 0)
    {
        std::cerr << "[decoder] cannot open VAAPI device " << device
                  << ": " << av_err(ret) << std::endl;
        return false;
    }

    ctx_->hw_device_ctx = av_buffer_ref(hw_dev_);
    ctx_->get_format    = get_vaapi_format;
    if (ctx_->hw_device_ctx != nullptr)
        return true;
    else
        return false;
}

bool DecoderFFmpeg::open_codec(const AVCodec* codec)
{
    std::string codec_name = codec->name;
    if (codec_name.contains("h264"))
        use_hw_ = true;
    else
        use_hw_ = false;

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_)
        return false;

    ctx_->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_count = 1; // Lowest single-thread latency

    if (!setup_vaapi())
        return false;

    const int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0)
    {
        std::cerr << "[decoder] avcodec_open2 failed: " << av_err(ret) << std::endl;
        return false;
    }

    avf_ = av_frame_alloc();
    if (use_hw_)
        sw_frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();

    if (avf_ && pkt_ && (!use_hw_ || sw_frame_))
        return true;
    else
        return false;
}

void DecoderFFmpeg::decode(EncodedPacketPtr ep, FrameCallback cb)
{
    if (!ep)
        return;

    pkt_->data = const_cast<uint8_t*>(ep->data.data());
    pkt_->size = static_cast<int>(ep->data.size());

    if (avcodec_send_packet(ctx_, pkt_) < 0)
        return;

    while (true)
    {
        const int ret = avcodec_receive_frame(ctx_, avf_);
        if (ret < 0)    // EAGAIN (needs more input), EOF or a real error
            break;

        // VAAPI is only requested at init; whether the GPU really took the stream is
        // only known now, so report what actually happened (once).
        if (!path_logged_)
        {
            path_logged_ = true;
            std::cout << "[decoder] first frame decoded on ";
            if (avf_->format == AV_PIX_FMT_VAAPI)
                std::cout << "GPU (VAAPI)";
            else
                std::cout << "CPU";

            std::cout << std::endl;
        }

        AVFrame* frame = avf_;

        // With VAAPI the decoded frame lives in GPU memory: copy it down into a normal
        // CPU NV12 frame. sw_frame_ is deliberately left empty (no buffer), so FFmpeg
        // allocates it with the surface's full coded size; we only request the format.
        if (use_hw_ && avf_->format == AV_PIX_FMT_VAAPI)
        {
            av_frame_unref(sw_frame_);
            sw_frame_->format = AV_PIX_FMT_NV12;
            if (av_hwframe_transfer_data(sw_frame_, avf_, 0) < 0)
            {
                av_frame_unref(avf_);
                continue;
            }
            frame = sw_frame_;
        }

        const int w = frame->width, h = frame->height;
        if (!sws_ || w != sw_ || h != sh_)
        {
            if (sws_)
                sws_freeContext(sws_);

            sw_ = w; sh_ = h;
            sws_ = sws_getContext(w, h, static_cast<AVPixelFormat>(frame->format),
                                  w, h, AV_PIX_FMT_BGRA,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }

        auto f = std::make_shared<RawFrame>();
        f->timestamp_us = ep->timestamp_us;
        f->width    = w;
        f->height   = h;
        f->linesize = w * 4;
        f->format   = PixelFormat::BGRA;
        f->data.resize(w * 4 * h);

        uint8_t* dst[1] = { f->data.data() };
        int dst_ls[1]   = { w * 4 };

        sws_scale(sws_, frame->data, frame->linesize, 0, h, dst, dst_ls);

        av_frame_unref(avf_);   // give the GPU surface back to the decoder's pool
        cb(std::move(f));
    }
}

DecoderFFmpeg::~DecoderFFmpeg()
{
    cleanup();
}

void DecoderFFmpeg::cleanup()
{
    if (sws_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (pkt_)
        av_packet_free(&pkt_);
    if (sw_frame_)
        av_frame_free(&sw_frame_);
    if (avf_)
        av_frame_free(&avf_);
    if (ctx_)
        avcodec_free_context(&ctx_);
    if (hw_dev_)
        av_buffer_unref(&hw_dev_);
}