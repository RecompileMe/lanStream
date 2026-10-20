// decoder_ffmpeg.hpp
#pragma once
#include "decoder.hpp"
#include <cstdint>
#include <string>
extern "C"
{
#include <libavutil/error.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

// How a decoder candidate gets its pictures out of the GPU.
enum class DecoderType
{
    Software,      // plain "h264" decoder on the CPU
    Native,        // named hardware decoder that returns CPU frames itself (h264_cuvid, h264_qsv)
    Vaapi,         // "h264" + VAAPI hwaccel: frames are GPU surfaces that must be downloaded
    VideoToolbox   // "h264" + VideoToolbox hwaccel (macOS): same download step as VAAPI
};

// One entry of the try-order: which FFmpeg decoder to open and how it hands back frames.
struct CodecType
{
    std::string label;   // what the user sees / RECEIVER_DECODER accepts
    std::string codec;   // FFmpeg decoder name
    DecoderType type;
};

class DecoderFFmpeg : public IDecoder
{
public:
    ~DecoderFFmpeg() override;
    bool init() override;
    void decode(EncodedPacketPtr packet_ptr, FrameCallback callback) override;
private:
    bool open_codec(const AVCodec* codec, DecoderType decoderType);
    // Create the hardware device and make the decoder output `pix_fmt` surfaces.
    bool setup_hwaccel(AVHWDeviceType type, const char* device, AVPixelFormat pixel_format);
    void cleanup();

    AVCodecContext* av_codec_context_ = nullptr;
    AVFrame*        av_frame_         = nullptr;   // decoder output (a GPU surface while a hwaccel is active)
    AVFrame*        sw_frame_    = nullptr;   // CPU copy of that GPU surface (hwaccel only)
    AVPacket*       av_packet_         = nullptr;
    AVBufferRef*    hardware_device_      = nullptr;   // hwaccel device (VAAPI / VideoToolbox only)
    SwsContext*     sws_         = nullptr;
    int             sw_ = 0, sh_ = 0;

    DecoderType     type_        = DecoderType::Software;
    AVPixelFormat   hardware_pixel_format_  = AV_PIX_FMT_NONE;  // surface format of the active hwaccel
    std::string     codec_;                          // which candidate won, for the log
    std::string     vaapi_node_;                     // Intel render node from detect_gpus()
    bool            path_logged_ = false;
};