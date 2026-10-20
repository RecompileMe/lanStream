#include "encoder_ffmpeg.hpp"
#include "gpu_detect.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

void log_av(const char* what, int err)
{
        char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(err, buf, sizeof(buf));
        std::cerr << "[encoder] " << what << " failed: " << buf << " (" << err << ")" << std::endl;
}


bool EncoderFFmpeg::init(int width, int height, int fps, int bitrate_kbps) {
    width_ = width; height_ = height;

    // ---- 1. What hardware is there? -------------------------------------
    const GpuInfo gpu = detect_gpus();
#ifdef PLATFORM_MACOS
    // CHANGED: on macOS there is no per-vendor encoder to pick. VideoToolbox is
    // the single API in front of Apple Silicon, Intel Quick Sync and AMD GPUs,
    // and macOS decides which hardware encoder it uses.
    std::cout << "[gpu] macOS: VideoToolbox picks the hardware encoder "
                 "(Apple Silicon / AMD / Intel)" << std::endl;
#else
    std::cout << "[gpu] NVIDIA: ";

    if (gpu.nvidia)
        std::cout << "yes" << std::endl;
    else
        std::cout <<  "no" << std::endl;

    std::cout << ", Intel: ";

    if (gpu.intel)
        std::cout << "yes" << std::endl;
    else
        std::cout << "no" << std::endl;

    if (gpu.intel_render_node.empty())
        std::cout << "The gpu render node is empty." << std::endl;
    else
        std::cout << "[" << gpu.intel_render_node << "]" << std::endl;

    if (gpu.known)
        std::cout << "GPU detected, proceeding next." << std::endl;
    else
        std::cout << " [detection unavailable, trying everything]" << std::endl;
#endif
    vaapi_node_ = gpu.intel_render_node;

    // ---- 2. Build the try-order: NVIDIA first, then Intel, then CPU -------
    std::vector<std::string> codec_list;

    // Debug override, e.g. SENDER_ENCODER=h264_vaapi ./build/sender
    // Goes first; the normal order stays behind it as fallback.
    if (const char* forced = std::getenv("SENDER_ENCODER"))
        codec_list.push_back(forced);

#ifdef PLATFORM_MACOS
    codec_list.push_back("h264_videotoolbox");
#else
    if (gpu.nvidia)
        codec_list.push_back("h264_nvenc");
#ifdef PLATFORM_LINUX
    // VAAPI is the non-NVIDIA route on Linux (Intel via i965, AMD via Mesa).
    // CHANGED: tried on every system without NVIDIA, not only when an Intel GPU
    // was detected, and on hybrid Intel+NVIDIA machines as fallback behind NVENC.
    if (gpu.intel || !gpu.nvidia)
        codec_list.push_back("h264_vaapi");
#endif
    if (gpu.intel)
        codec_list.push_back("h264_qsv");
#ifdef PLATFORM_WINDOWS
    codec_list.push_back("h264_amf");                    // AMD, Windows only
#endif
#endif
    codec_list.push_back("libx264");                     // CPU, always last

    std::cout << "[encoder] order:";
    for (const auto& codec : codec_list)
        std::cout << " " << codec;
    std::cout << std::endl;

    // ---- 3. Use the first encoder that really OPENS ------------------------
    // Detection is only a pre-filter. If NVIDIA is detected but NVENC fails
    // (driver without libnvidia-encode, GPU busy, ...), this falls through to
    // Intel and finally to the CPU. On a Mac, VideoToolbox failing falls
    // through to libx264.
    for (const auto& codec : codec_list)
    {
        const AVCodec* av_codec = avcodec_find_encoder_by_name(codec.c_str());

        if (!av_codec)
        {
            std::cerr << "[encoder] " << codec << " is not in this FFmpeg build, skipping" << std::endl;
            continue;
        }

        if (open_codec(av_codec, fps, bitrate_kbps))
        {
            std::cout << "[encoder] using " << codec << std::endl;
            break;
        }

        // NEW: some Intel chips (e.g. HD 520) only offer CQP rate control through
        // VAAPI. Retry once with CQP before giving up on this encoder.
        if (codec == "h264_vaapi" && !force_cqp_)
        {
            std::cerr << "[encoder] h264_vaapi: retrying with constant-QP rate control" << std::endl;
            force_cqp_ = true;

            if (open_codec(av_codec, fps, bitrate_kbps))
            {
                std::cout << "[encoder] using h264_vaapi (CQP)" << std::endl;
                break;
            }
            force_cqp_ = false;
        }
        std::cerr << "[encoder] has" << codec << " failed, trying next" << std::endl;
    }

    if (!av_codec_context_)
    {
        std::cerr << "[encoder] has no usable codec" << std::endl;
        return false;
    }

    avf_ = av_frame_alloc();

    if (!avf_)
        return false;

    avf_->format = AV_PIX_FMT_NV12;
    avf_->width  = width;
    avf_->height = height;

    if (av_frame_get_buffer(avf_, 32) < 0)
        return false;

    if (av_codec_context_->hw_frames_ctx)
        hw_frm_ = av_frame_alloc();  // only for VAAPI

    pkt_ = av_packet_alloc();

    if (!pkt_)
        return false;

    sws_ = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                          width, height, AV_PIX_FMT_NV12,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    return sws_ != nullptr;
}

// The context setup for a single encoder.
// On failure it frees everything it allocated, so init() can try the next one.
bool EncoderFFmpeg::open_codec(const AVCodec* codec, int fps, int bitrate_kbps)
{
    std::string_view codec_str{};
    auto fail = [this]
            {
        avcodec_free_context(&av_codec_context_);
        av_buffer_unref(&hw_dev_);
        return false;
    };

    // Set a private codec option and warn (instead of silently ignoring) when
    // this FFmpeg / driver combination doesn't know it.
    auto opt = [&](const char* key, const char* val)
            {
        int r = av_opt_set(av_codec_context_->priv_data, key, val, 0);
        if (r < 0)
            std::cerr << "[encoder] " << codec->name << ": option " << key << "=" << val
                      << " not accepted (ignored)" << std::endl;
        return r >= 0;
    };

    av_codec_context_ = avcodec_alloc_context3(codec);

    if (!av_codec_context_)
        return false;

    av_codec_context_->width        = width_;
    av_codec_context_->height       = height_;
    av_codec_context_->time_base    = {1, fps};
    av_codec_context_->framerate    = {fps, 1};
    av_codec_context_->bit_rate     = bitrate_kbps * 1000LL;

    // One IDR per second, no B-frames. (gop_size = 1 / all-intra made NVENC refuse
    // to start and starved libx264 at 8 Mbps.)
    av_codec_context_->gop_size     = std::max(fps, 2);
    av_codec_context_->max_b_frames = 0;

    // Small VBV buffer (~4 frames) keeps bitrate peaks, and so latency, in check.
    av_codec_context_->rc_max_rate    = av_codec_context_->bit_rate;
    av_codec_context_->rc_buffer_size = static_cast<int>(av_codec_context_->bit_rate / std::max(fps, 1) * 4);
    codec_str = codec->name;

    if (codec_str == "h264_vaapi")
    {
#ifdef PLATFORM_LINUX
        // NEW: legacy Intel driver (i965, Intel Gen4-Gen9) instead of the newer iHD.
        // Only when we are really targeting an Intel node (an AMD GPU has no i965),
        // and the last argument 0 = never override a LIBVA_DRIVER_NAME the user set.
        // libva reads this variable during vaInitialize, i.e. inside the call below.
        if (!vaapi_node_.empty())
            setenv("LIBVA_DRIVER_NAME", "i965", 0);
#endif

        // 1) VAAPI device: the Intel render node found by detect_gpus();
        // nullptr lets libva auto-detect.
        const char* node = vaapi_node_.empty() ? nullptr : vaapi_node_.c_str();
        // NEW: manual override, same variable name as in your own version
        if (const char* o = std::getenv("LANSTREAM_VAAPI_DEVICE"))
        {
            if (*o)
                node = o;
        }
        if (!force_cqp_)
        {
            const char* drv = std::getenv("LIBVA_DRIVER_NAME");
            std::cout << "[encoder] VAAPI device: " << (node ? node : "auto")
                      << ", driver: " << (drv ? drv : "auto") << "\n";
        }
        int r = av_hwdevice_ctx_create(&hw_dev_, AV_HWDEVICE_TYPE_VAAPI, node, nullptr, 0);
        if (r < 0)
        {
            log_av("av_hwdevice_ctx_create(vaapi)", r);
            return fail();
        }

        // 2) Pool of GPU surfaces = the "hardware frames reference" the encoder asks for
        AVBufferRef* fref = av_hwframe_ctx_alloc(hw_dev_);
        if (!fref)
            return fail();
        auto* fc = reinterpret_cast<AVHWFramesContext*>(fref->data);
        fc->format            = AV_PIX_FMT_VAAPI;   // what the encoder is fed
        fc->sw_format         = AV_PIX_FMT_NV12;    // what is stored inside the surface
        fc->width             = width_;
        fc->height            = height_;
        fc->initial_pool_size = 8;
        if (av_hwframe_ctx_init(fref) < 0) { av_buffer_unref(&fref); return fail(); }
        av_codec_context_->hw_frames_ctx = av_buffer_ref(fref);
        av_buffer_unref(&fref);

        av_codec_context_->pix_fmt = AV_PIX_FMT_VAAPI;
        // x264 calls it "baseline", h264_vaapi calls it "constrained_baseline"
        // (drop this line if your driver doesn't list VAProfileH264ConstrainedBaseline)
        opt("profile", "constrained_baseline");

        // NEW: power saving off. Low-power encoding (fixed-function "LP" entrypoint)
        // often only offers CQP; the normal entrypoint also does CBR/VBR.
        opt("low_power", "0");
        opt("async_depth", "1");               // no frame queueing -> lowest latency

        if (force_cqp_)
        {
            // Constant-QP: bit_rate and the VBV settings must be 0 in this mode.
            opt("rc_mode", "CQP");
            av_opt_set_int(av_codec_context_->priv_data, "qp", 24, 0);   // lower = better quality, bigger packets
            av_codec_context_->bit_rate       = 0;
            av_codec_context_->rc_max_rate    = 0;
            av_codec_context_->rc_buffer_size = 0;
        }

    }
    else if (std::strcmp(codec->name, "h264_nvenc") == 0)
    {
        // Low-latency NVENC settings
        av_codec_context_->pix_fmt = AV_PIX_FMT_NV12;
        if (!opt("preset", "p1"))              // FFmpeg >= 4.4 (new SDK presets)
            opt("preset", "llhq");             // older FFmpeg fallback
        opt("tune", "ull");                    // ultra low latency
        opt("rc", "cbr");
        opt("zerolatency", "1");
        opt("profile", "baseline");

    }
    else if (std::strcmp(codec->name, "h264_qsv") == 0)
    {
        // Intel Quick Sync, fed with system-memory NV12 frames
        av_codec_context_->pix_fmt = AV_PIX_FMT_NV12;
        opt("preset", "veryfast");
        opt("async_depth", "1");               // no frame queueing -> lowest latency

    }
    else if (std::strcmp(codec->name, "h264_videotoolbox") == 0)
    {
        // NEW: macOS VideoToolbox (Apple Silicon, AMD and Intel Macs).
        // Needs an FFmpeg built with --enable-videotoolbox.
        av_codec_context_->pix_fmt = AV_PIX_FMT_NV12;
        opt("realtime",   "1");                // low-latency mode instead of throughput mode
        opt("prio_speed", "1");                // speed over quality (newer FFmpeg only)
        opt("allow_sw",   "0");                // no silent software fallback; open() fails
        // and init() moves on to libx264 instead
        opt("profile",    "baseline");

    }
    else
    {
        av_codec_context_->pix_fmt = AV_PIX_FMT_NV12;
        if (std::strcmp(codec->name, "libx264") == 0)
        {   // x264-only options
            opt("preset",  "ultrafast");
            opt("tune",    "zerolatency");
            opt("profile", "baseline");
        }
    }

    int r = avcodec_open2(av_codec_context_, codec, nullptr);
    if (r < 0)
    {
        log_av(codec->name, r);
        return fail();
    }
    return true;
}

void EncoderFFmpeg::do_encode(AVFrame* frame, PacketCallback& cb)
{
    if (!av_codec_context_)
        return;
    int r = avcodec_send_frame(av_codec_context_, frame);
    if (r < 0 && r != AVERROR_EOF)
    {
        log_av("avcodec_send_frame", r);
        return;
    }

    while (true)
    {
        int ret = avcodec_receive_packet(av_codec_context_, pkt_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { log_av("avcodec_receive_packet", ret); break; }

        auto ep = std::make_shared<EncodedPacket>();
        ep->frame_id     = fid_++;
        // timestamp = moment the frame ENTERED encode(), looked up by pts.
        const int64_t idx = pkt_->pts >= 0 ? pkt_->pts : 0;
        ep->timestamp_us = in_us_[static_cast<size_t>(idx % kRing)];
        ep->is_keyframe  = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
        ep->data.assign(pkt_->data, pkt_->data + pkt_->size);
        av_packet_unref(pkt_);
        cb(std::move(ep));
    }
}

void EncoderFFmpeg::encode(RawFramePtr raw, PacketCallback cb) {
    if (!raw || raw->data.empty() || !av_codec_context_) return;

    // If RawFrame has its own capture timestamp, use that here instead to
    // measure the full capture -> encode-done latency.
    const int64_t t_in = now_us();

    const uint8_t* src[1]  = { raw->data.data() };
    int  src_ls[1]          = { raw->linesize };
    av_frame_make_writable(avf_);
    sws_scale(sws_, src, src_ls, 0, height_,
              avf_->data, avf_->linesize);

    // Own input counter instead of fid_ (which only counts output packets
    // and drifts as soon as an encoder buffers).
    avf_->pts = pts_;
    in_us_[static_cast<size_t>(pts_ % kRing)] = t_in;
    ++pts_;

    if (hw_frm_)
    {                                                   // VAAPI path
        av_frame_unref(hw_frm_);

        if (av_hwframe_get_buffer(av_codec_context_->hw_frames_ctx, hw_frm_, 0) < 0)
            return;

        if (av_hwframe_transfer_data(hw_frm_, avf_, 0) < 0)
            return;  // CPU NV12 -> GPU surface

        hw_frm_->pts = avf_->pts;
        do_encode(hw_frm_, cb);
    }
    else
        do_encode(avf_, cb);
}

void EncoderFFmpeg::flush(PacketCallback cb) { do_encode(nullptr, cb); }

void EncoderFFmpeg::cleanup()
{
    if (hw_frm_)
        av_frame_free(&hw_frm_);                    // before ctx_
    if (av_codec_context_)
        avcodec_free_context(&av_codec_context_);
    if (hw_dev_)
        av_buffer_unref(&hw_dev_);                  // after ctx_
    if (avf_)
        av_frame_free(&avf_);
    if (pkt_)
        av_packet_free(&pkt_);
    if (sws_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }    // null it, so a second cleanup is safe
}