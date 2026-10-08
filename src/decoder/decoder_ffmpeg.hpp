// decoder_ffmpeg.hpp
#pragma once
#include "decoder.hpp"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

class DecoderFFmpeg : public IDecoder {
public:
    ~DecoderFFmpeg() override;
    bool init()                                         override;
    void decode(EncodedPacketPtr pkt, FrameCallback cb) override;
private:
    bool open_codec(const AVCodec* codec);
    bool setup_vaapi();
    void cleanup();

    AVCodecContext* ctx_         = nullptr;
    AVFrame*        avf_         = nullptr;   // decoder output (a GPU surface while VAAPI is active)
    AVFrame*        sw_frame_    = nullptr;   // CPU copy of that GPU surface (VAAPI only)
    AVPacket*       pkt_         = nullptr;
    AVBufferRef*    hw_dev_      = nullptr;   // VAAPI device (VAAPI only)
    SwsContext*     sws_         = nullptr;
    int             sw_ = 0, sh_ = 0;
    bool            use_hw_      = false;
    bool            path_logged_ = false;
};