#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "decoder_ffmpeg.hpp"
#include "encoder/gpu_detect.hpp"   // shared with the encoder (src/ is on the include path)

namespace {

    std::string av_err(int errnum)
    {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(errnum, buf, sizeof(buf));
        return buf;
    }

// One entry of the try-order: which FFmpeg decoder to open and how it hands back frames.
    struct Candidate
    {
        std::string label;   // what the user sees / RECEIVER_DECODER accepts
        std::string codec;   // FFmpeg decoder name
        DecKind     kind;
    };

    bool candidate_from_label(const std::string& label, Candidate& out)
    {
        if      (label == "h264_cuvid")        out = {label, "h264_cuvid", DecKind::Native};
        else if (label == "h264_qsv")          out = {label, "h264_qsv",   DecKind::Native};
        else if (label == "h264_vaapi")        out = {label, "h264",       DecKind::Vaapi};
        else if (label == "h264_videotoolbox") out = {label, "h264",       DecKind::VideoToolbox};
        else if (label == "h264")              out = {label, "h264",       DecKind::Software};
        else return false;
        return true;
    }

// FFmpeg calls this once it has parsed the stream header and has to pick the pixel
// format to decode into. Returning the hwaccel's surface format is what switches GPU
// decoding on (the wanted format was stored in ctx->opaque by setup_hwaccel).
// If the driver cannot decode this stream (unsupported profile, hwaccel setup
// failed, ...) that format is missing from the list, and we fall back to normal
// software decoding instead of failing.
    AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts)
    {
        const auto want = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
        for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        {
            if (*p == want)
                return want;
        }

        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::cerr << "[decoder] GPU cannot decode this stream, falling back to software" << std::endl;
        }
        return avcodec_default_get_format(ctx, pix_fmts);
    }

} // namespace

bool DecoderFFmpeg::init()
{
    // ---- 1. What hardware is there? (same detection as the encoder) ---------
    const GpuInfo gpu = detect_gpus();
#ifdef PLATFORM_MACOS
    std::cout << "[gpu] macOS: VideoToolbox picks the hardware decoder "
                 "(Apple Silicon / AMD / Intel)" << std::endl;
#else
    std::cout << "[gpu] NVIDIA: " << (gpu.nvidia ? "yes" : "no")
              << ", Intel: " << (gpu.intel ? "yes" : "no");
    if (!gpu.intel_render_node.empty())
        std::cout << " (" << gpu.intel_render_node << ")";
    if (!gpu.known)
        std::cout << " [detection unavailable, trying everything]";
    std::cout << std::endl;
#endif
    vaapi_node_ = gpu.intel_render_node;

    // ---- 2. Build the try-order: NVIDIA first, then Intel, then CPU ---------
    std::vector<std::string> labels;
    auto add = [&](const std::string& l) {
        if (std::find(labels.begin(), labels.end(), l) == labels.end())
            labels.push_back(l);
    };

    // Debug override, e.g. RECEIVER_DECODER=h264_vaapi ./build/receiver 5000
    // (or RECEIVER_DECODER=h264 to force software). Goes first; the normal
    // order stays behind it as fallback.
    if (const char* forced = std::getenv("RECEIVER_DECODER"))
        add(forced);

#ifdef PLATFORM_MACOS
    add("h264_videotoolbox");
#else
    if (gpu.nvidia)
        add("h264_cuvid");                   // NVDEC
#ifdef PLATFORM_LINUX
    // VAAPI is the non-NVIDIA route on Linux (Intel via i965, AMD via Mesa).
    if (gpu.intel || !gpu.nvidia)
        add("h264_vaapi");
#endif
#ifdef PLATFORM_WINDOWS
    if (gpu.intel)
        add("h264_qsv");
#endif
#endif
    add("h264");                             // CPU, always last

    std::cout << "[decoder] order:";
    for (const auto& l : labels)
        std::cout << " " << l;
    std::cout << std::endl;

    // ---- 3. Use the first decoder that really OPENS -------------------------
    // avcodec_find_decoder_by_name() only says the decoder was *compiled into*
    // FFmpeg, not that it works on this machine. So open each candidate for real
    // and fall through to the next one if that fails.
    for (const auto& label : labels)
    {
        Candidate cand;
        if (!candidate_from_label(label, cand))
        {
            std::cerr << "[decoder] unknown decoder '" << label << "', skipping" << std::endl;
            continue;
        }

        const AVCodec* codec = avcodec_find_decoder_by_name(cand.codec.c_str());
        if (!codec)
        {
            std::cerr << "[decoder] " << label << " not in this FFmpeg build, skipping" << std::endl;
            continue;
        }

        if (open_codec(codec, cand.kind))
        {
            label_ = label;
            std::cout << "[decoder] using " << label
                      << (cand.kind == DecKind::Software ? " (CPU)" : " (GPU)") << std::endl;
            return true;
        }
        std::cerr << "[decoder] " << label << " failed, trying next" << std::endl;
        cleanup();
    }
    std::cerr << "[decoder] no usable decoder found" << std::endl;
    return false;
}

bool DecoderFFmpeg::setup_hwaccel(AVHWDeviceType type, const char* device, AVPixelFormat pix_fmt)
{
    const int ret = av_hwdevice_ctx_create(&hw_dev_, type, device, nullptr, 0);
    if (ret < 0)
    {
        std::cerr << "[decoder] cannot open " << av_hwdevice_get_type_name(type) << " device"
                  << (device ? std::string(" ") + device : std::string())
                  << ": " << av_err(ret) << std::endl;
        return false;
    }

    ctx_->hw_device_ctx = av_buffer_ref(hw_dev_);
    if (!ctx_->hw_device_ctx)
        return false;

    hw_pix_fmt_      = pix_fmt;
    ctx_->opaque     = reinterpret_cast<void*>(static_cast<intptr_t>(pix_fmt));
    ctx_->get_format = get_hw_format;
    return true;
}

bool DecoderFFmpeg::open_codec(const AVCodec* codec, DecKind kind)
{
    kind_       = kind;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_)
        return false;

    ctx_->flags  |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx_->thread_count = 1; // Lowest single-thread latency

    switch (kind)
    {
        case DecKind::Vaapi:
        {
            // Override the render node with e.g. LANSTREAM_VAAPI_DEVICE=/dev/dri/renderD129.
            // Otherwise use the Intel node found by detect_gpus(); nullptr lets libva choose.
            const char* node = vaapi_node_.empty() ? nullptr : vaapi_node_.c_str();
            if (const char* env = std::getenv("LANSTREAM_VAAPI_DEVICE"))
            {
                if (*env)
                    node = env;
            }
#ifdef PLATFORM_LINUX
            // Legacy Intel driver (i965) like in the encoder: only for an Intel node, and
            // never override a LIBVA_DRIVER_NAME the user set themselves.
            if (!vaapi_node_.empty())
                setenv("LIBVA_DRIVER_NAME", "i965", 0);
#endif
            const char* drv = std::getenv("LIBVA_DRIVER_NAME");
            std::cout << "[decoder] VAAPI device: " << (node ? node : "auto")
                      << ", driver: " << (drv ? drv : "auto") << std::endl;

            if (!setup_hwaccel(AV_HWDEVICE_TYPE_VAAPI, node, AV_PIX_FMT_VAAPI))
                return false;
            break;
        }
        case DecKind::VideoToolbox:
            if (!setup_hwaccel(AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, AV_PIX_FMT_VIDEOTOOLBOX))
                return false;
            break;
        case DecKind::Native:      // the decoder talks to the GPU by itself
        case DecKind::Software:
            break;
    }

    const int ret = avcodec_open2(ctx_, codec, nullptr);
    if (ret < 0)
    {
        std::cerr << "[decoder] avcodec_open2 failed: " << av_err(ret) << std::endl;
        return false;
    }

    avf_ = av_frame_alloc();
    if (hw_pix_fmt_ != AV_PIX_FMT_NONE)
        sw_frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();

    return avf_ && pkt_ && (hw_pix_fmt_ == AV_PIX_FMT_NONE || sw_frame_);
}

void DecoderFFmpeg::decode(EncodedPacketPtr ep, FrameCallback cb)
{
    // NEW: init() may have failed (or not run); never touch the null members then.
    if (!ep || !ctx_ || !pkt_ || !avf_)
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

        const bool on_gpu_surface = hw_pix_fmt_ != AV_PIX_FMT_NONE && avf_->format == hw_pix_fmt_;

        // The hwaccel is only requested at init; whether the GPU really took the stream is
        // only known now, so report what actually happened (once).
        if (!path_logged_)
        {
            path_logged_ = true;
            std::cout << "[decoder] first frame decoded on ";
            if (on_gpu_surface || kind_ == DecKind::Native)
                std::cout << "GPU (" << label_ << ")";
            else
                std::cout << "CPU";

            std::cout << std::endl;
        }

        AVFrame* frame = avf_;

        // With a hwaccel the decoded frame lives in GPU memory: copy it down into a normal
        // CPU NV12 frame. sw_frame_ is deliberately left empty (no buffer), so FFmpeg
        // allocates it with the surface's full coded size; we only request the format.
        if (on_gpu_surface)
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
        av_frame_free(&avf_);       // before ctx_: GPU surfaces belong to the decoder's pool
    if (ctx_)
        avcodec_free_context(&ctx_);
    if (hw_dev_)
        av_buffer_unref(&hw_dev_);

    // NEW: a failed candidate must not leak state into the next one
    hw_pix_fmt_  = AV_PIX_FMT_NONE;
    path_logged_ = false;
    sw_ = sh_ = 0;
}