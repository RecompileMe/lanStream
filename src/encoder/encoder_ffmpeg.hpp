#pragma once
#include "encoder.hpp"
#include <array>
#include <cstdint>
#include <string>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>              // VAAPI device / frames context
#include <libswscale/swscale.h>
}

class EncoderFFmpeg : public IEncoder
{
public:
    ~EncoderFFmpeg() override
    {
        cleanup();
    }
    bool init(int width, int height, int fps, int bitrate_kbps) override;
    void encode(RawFramePtr frame, PacketCallback callback) override;
    void flush(PacketCallback callback) override;
private:
    bool open_codec(const AVCodec* codec, int fps, int bitrate_kbps);
    void cleanup();
    void do_encode(AVFrame* frame, PacketCallback& callback);

    AVCodecContext* av_codec_context_     = nullptr;
    AVFrame*        av_frame_     = nullptr;   // CPU frame (NV12)
    AVFrame*        hardware_frame_  = nullptr;   // GPU surface, VAAPI only
    AVBufferRef*    hardware_device_  = nullptr;   // VAAPI device
    AVPacket*       av_packet_      = nullptr;
    SwsContext*     sws_     = nullptr;

    uint32_t        frame_id_     = 0;         // frame_id, counts OUTPUT packets
    int64_t         pts_     = 0;         // counts INPUT frames
    int             width_   = 0;
    int             height_  = 0;

    // NEW: render node of the Intel GPU found by detect_gpus(); empty = let libva choose.
    std::string     vaapi_node_;

    // NEW: set by init() when h264_vaapi refuses bitrate-based rate control;
    // open_codec() then uses constant-QP (CQP) instead.
    bool            force_cqp_ = false;

    // Input timestamp per pts, so a packet gets the time its frame went IN,
    // even if the encoder buffers a few frames. Indexed by pts % kRing.
    static constexpr int kRing = 64;
    std::array<int64_t, kRing> in_us_{};
};