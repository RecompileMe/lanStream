#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "decoder_ffmpeg.hpp"
#include "encoder/gpu_detect.hpp"   // shared with the encoder (src/ is on the include path)

static constexpr unsigned int hash(std::string_view str)
{
    unsigned int hash = 2166136261u;
    for (char c : str)
    {
        hash ^= static_cast<unsigned int>(c);
        hash *= 16777619u;
    }
    return hash;
}

std::string av_err(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return buf;
}

bool codec_from_name(const std::string &name, CodecType &output)
{
    switch (hash(name))
    {
        case hash("h264_cuvid"):
            output = {name, "h264_cuvid", DecoderType::Native};
            break;
        case hash("h264_qsv"):
            output = {name, "h264_qsv", DecoderType::Native};
            break;
        case hash("h264_vaapi"):
            output = {name, "h264", DecoderType::Vaapi};
            break;
        case hash("h264_videotoolbox"):
            output = {name, "h264", DecoderType::VideoToolbox};
            break;
        case hash("h264"):
            output = {name, "h264", DecoderType::Software};
            break;
        default:
            return false;
    }
    /*
    if (label == "h264_cuvid")
        output = {label, "h264_cuvid", DecoderType::Native};
    else if (label == "h264_qsv")
        output = {label, "h264_qsv", DecoderType::Native};
    else if (label == "h264_vaapi")
        output = {label, "h264", DecoderType::Vaapi};
    else if (label == "h264_videotoolbox")
        output = {label, "h264", DecoderType::VideoToolbox};
    else if (label == "h264")
        output = {label, "h264", DecoderType::Software};
    else
        return false;
     */
    return true;
}

// FFmpeg calls this once it has parsed the stream header and has to pick the pixel
// format to decode into. Returning the hwaccel's surface format is what switches GPU
// decoding on (the wanted format was stored in ctx->opaque by setup_hwaccel).
// If the driver cannot decode this stream (unsupported profile, hwaccel setup
// failed, ...) that format is missing from the list, and we fall back to normal
// software decoding instead of failing.
AVPixelFormat get_hw_format(AVCodecContext *codec_context, const AVPixelFormat *pixel_format)
{
    const auto want = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(codec_context->opaque));

    for (const AVPixelFormat *ptr = pixel_format; *ptr != AV_PIX_FMT_NONE; ++ptr)
    {
        if (*ptr == want)
            return want;
    }

    static bool warned = false;

    if (!warned)
    {
        warned = true;
        std::cerr << "[decoder] GPU cannot decode this stream, falling back to software" << std::endl;
    }
    return avcodec_default_get_format(codec_context, pixel_format);
}

bool DecoderFFmpeg::init()
{
    // ---- 1. What hardware is there? (same detection as the encoder) ---------
    const GpuInfo gpu = detect_gpus();
#ifdef PLATFORM_MACOS
    std::cout << "[gpu] macOS: VideoToolbox picks the hardware decoder "
                 "(Apple Silicon / AMD / Intel)" << std::endl;
#else
    std::cout << "[gpu] NVIDIA: ";
    if (gpu.nvidia)
        std:: cout << "yes" << std::endl;
    else
        std::cout << "no" << std::endl;
    std::cout << "[gpu] Intel: ";

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

    // ---- 2. Build the try-order: NVIDIA first, then Intel, then CPU ---------
    std::vector<std::string> codecs_list;

    // Debug override, e.g. RECEIVER_DECODER=h264_vaapi ./build/receiver 5000
    // (or RECEIVER_DECODER=h264 to force software). Goes first; the normal
    // order stays behind it as fallback.
    if (const char *forced = std::getenv("RECEIVER_DECODER"))
        codecs_list.push_back(forced);

#ifdef PLATFORM_MACOS
    codecs_list.push_back("h264_videotoolbox");
#else
    if (gpu.nvidia)
        codecs_list.push_back("h264_cuvid");                   // NVDEC
#ifdef PLATFORM_LINUX
    // VAAPI is the non-NVIDIA route on Linux (Intel via i965, AMD via Mesa).
    if (gpu.intel || !gpu.nvidia)
        codecs_list.push_back("h264_vaapi");
#endif
#ifdef PLATFORM_WINDOWS
    if (gpu.intel)
        add("h264_qsv");
#endif
#endif
    codecs_list.push_back("h264");                             // CPU, always last

    std::cout << "[decoder] order:";
    for (const auto &codec: codecs_list)
        std::cout << " " << codec;
    std::cout << std::endl;

    // ---- 3. Use the first decoder that really OPENS -------------------------
    // avcodec_find_decoder_by_name() only says the decoder was *compiled into*
    // FFmpeg, not that it works on this machine. So open each candidate for real
    // and fall through to the next one if that fails.
    for (const auto &codec: codecs_list)
    {
        CodecType codec_type;
        if (!codec_from_name(codec, codec_type))
        {
            std::cerr << "[decoder] unknown decoder '" << codec << "', skipping" << std::endl;
            continue;
        }

        const AVCodec *av_codec = avcodec_find_decoder_by_name(codec_type.codec.c_str());
        if (!av_codec)
        {
            std::cerr << "[decoder] " << codec << " not in this FFmpeg build, skipping" << std::endl;
            continue;
        }

        if (open_codec(av_codec, codec_type.type))
        {
            codec_ = codec;
            std::cout << "[decoder] using " << codec;
            if (codec_type.type == DecoderType::Software)
                std::cout << " (CPU)" << std::endl;
            else
                std::cout << " (GPU)" << std::endl;
            return true;
        }
        std::cerr << "[decoder] " << codec << " failed, trying next" << std::endl;
        cleanup();
    }
    std::cerr << "[decoder] no usable decoder found" << std::endl;
    return false;
}

bool DecoderFFmpeg::setup_hwaccel(AVHWDeviceType type, const char *device, AVPixelFormat pixel_format)
{
    const int ret = av_hwdevice_ctx_create(&hardware_device_, type, device, nullptr, 0);
    if (ret < 0)
    {
        std::cerr << "[decoder] cannot open " << av_hwdevice_get_type_name(type) << " device";
        if (device)
            std::cerr << std::string(" ") + device << std::endl;
        else
            std::cerr << std::string() << ": " << av_err(ret) << std::endl;
        return false;
    }

    codec_context_->hw_device_ctx = av_buffer_ref(hardware_device_);
    if (!codec_context_->hw_device_ctx)
        return false;

    hardware_pixel_format_ = pixel_format;
    codec_context_->opaque = reinterpret_cast<void *>(static_cast<intptr_t>(pixel_format));
    codec_context_->get_format = get_hw_format;
    return true;
}

bool DecoderFFmpeg::open_codec(const AVCodec *codec, DecoderType decoder_type)
{
    type_ = decoder_type;
    hardware_pixel_format_ = AV_PIX_FMT_NONE;

    codec_context_ = avcodec_alloc_context3(codec);
    if (!codec_context_)
        return false;

    codec_context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_context_->flags2 |= AV_CODEC_FLAG2_FAST;
    codec_context_->thread_count = 1; // Lowest single-thread latency

    switch (decoder_type)
    {
        case DecoderType::Vaapi:
        {
            // Override the render node with e.g. LANSTREAM_VAAPI_DEVICE=/dev/dri/renderD129.
            // Otherwise, use the Intel node found by detect_gpus(); nullptr lets libva choose.
            std::string node{};
            if (vaapi_node_.empty())
                node = "";
            else
                node = vaapi_node_;

            const char* env = std::getenv("LANSTREAM_VAAPI_DEVICE");
                if (!env)
                    env = nullptr;
                else
                    node = *env;
#ifdef PLATFORM_LINUX
            // Legacy Intel driver (i965) like in the encoder: only for an Intel node, and
            // never override a LIBVA_DRIVER_NAME the user set themselves.
            if (!vaapi_node_.empty())
                setenv("LIBVA_DRIVER_NAME", "i965", 0);
#endif
            const char* drv = std::getenv("LIBVA_DRIVER_NAME");
            std::cout << "[decoder] VAAPI device: ";

            if (node.empty())
                std::cout << "auto";
            else
                std::cout << node;

            std::cout << ", driver: ";
            if (!drv)
                std::cout << "auto" << std::endl;
            else
                std::cout << drv << std::endl;

            if (!setup_hwaccel(AV_HWDEVICE_TYPE_VAAPI, node.c_str(), AV_PIX_FMT_VAAPI))
                return false;
            break;
        }
        case DecoderType::VideoToolbox:
            if (!setup_hwaccel(AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, AV_PIX_FMT_VIDEOTOOLBOX))
                return false;
            break;
        case DecoderType::Native:      // the decoder talks to the GPU by itself
        case DecoderType::Software:
            break;
    }

    const int ret = avcodec_open2(codec_context_, codec, nullptr);

    if (ret < 0)
    {
        std::cerr << "[decoder] avcodec_open2 failed: " << av_err(ret) << std::endl;
        return false;
    }

    av_frame_ = av_frame_alloc();
    if (hardware_pixel_format_ != AV_PIX_FMT_NONE)
        sw_frame_ = av_frame_alloc();
    av_packet_ = av_packet_alloc();

    bool has_core_resources = av_frame_ && av_packet_;
    bool has_valid_format = (hardware_pixel_format_ == AV_PIX_FMT_NONE || sw_frame_);

    if (has_core_resources && has_valid_format)
        return true;
    else
        return false;
}

void DecoderFFmpeg::decode(EncodedPacketPtr packet_ptr, FrameCallback callback)
{
    // NEW: init() may have failed (or not run); never touch the null members then.
    if (!packet_ptr || !codec_context_ || !av_packet_ || !av_frame_)
        return;

    av_packet_->data = const_cast<uint8_t*>(packet_ptr->data.data());
    av_packet_->size = static_cast<int>(packet_ptr->data.size());

    if (avcodec_send_packet(codec_context_, av_packet_) < 0)
        return;

    while (true)
    {
        const int ret = avcodec_receive_frame(codec_context_, av_frame_);
        if (ret < 0)    // EAGAIN (needs more input), EOF or a real error
            break;

        bool on_gpu_surface = false;
        if (hardware_pixel_format_ != AV_PIX_FMT_NONE && av_frame_->format == hardware_pixel_format_)
            on_gpu_surface = true;
        else
            on_gpu_surface = false;

        // The hwaccel is only requested at init; whether the GPU really took the stream is
        // only known now, so report what actually happened (once).
        if (!path_logged_)
        {
            path_logged_ = true;
            std::cout << "[decoder] first frame decoded on ";
            if (on_gpu_surface || type_ == DecoderType::Native)
                std::cout << "GPU (" << codec_ << ")";
            else
                std::cout << "CPU";

            std::cout << std::endl;
        }

        AVFrame *frame_ptr = av_frame_;

        // With a hwaccel the decoded frame lives in GPU memory: copy it down into a normal
        // CPU NV12 frame. sw_frame_ is deliberately left empty (no buffer), so FFmpeg
        // allocates it with the surface's full coded size; we only request the format.
        if (on_gpu_surface)
        {
            av_frame_unref(sw_frame_);
            sw_frame_->format = AV_PIX_FMT_NV12;
            if (av_hwframe_transfer_data(sw_frame_, av_frame_, 0) < 0)
            {
                av_frame_unref(av_frame_);
                continue;
            }
            frame_ptr = sw_frame_;
        }

        const int width = frame_ptr->width, height = frame_ptr->height;
        if (!sws_ || width != sw_ || height != sh_)
        {
            if (sws_)
                sws_freeContext(sws_);

            sw_ = width;
            sh_ = height;
            sws_ = sws_getContext(width, height, static_cast<AVPixelFormat>(frame_ptr->format),
                                  width, height, AV_PIX_FMT_BGRA,
                                  SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        }

        auto frame = std::make_shared<RawFrame>();
        frame->timestamp_us = packet_ptr->timestamp_us;
        frame->width = width;
        frame->height = height;
        frame->linesize = width * 4;
        frame->format = PixelFormat::BGRA;
        frame->data.resize(width * 4 * height);

        uint8_t *dst[1] = {frame->data.data()};
        int dst_ls[1] = {width * 4};

        sws_scale(sws_, frame_ptr->data, frame_ptr->linesize, 0, height, dst, dst_ls);

        av_frame_unref(av_frame_);   // give the GPU surface back to the decoder's pool
        callback(std::move(frame));
    }
}

DecoderFFmpeg::~DecoderFFmpeg() {
    cleanup();
}

void DecoderFFmpeg::cleanup()
{
    if (sws_)
    {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (av_packet_)
        av_packet_free(&av_packet_);
    if (sw_frame_)
        av_frame_free(&sw_frame_);
    if (av_frame_)
        av_frame_free(&av_frame_);       // before ctx_: GPU surfaces belong to the decoder's pool
    if (codec_context_)
        avcodec_free_context(&codec_context_);
    if (hardware_device_)
        av_buffer_unref(&hardware_device_);

    // NEW: a failed candidate must not leak state into the next one
    hardware_pixel_format_ = AV_PIX_FMT_NONE;
    path_logged_ = false;
    sw_ = sh_ = 0;
}