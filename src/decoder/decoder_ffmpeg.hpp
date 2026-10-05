// decoder_ffmpeg.hpp
#pragma once
#include "decoder.hpp"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

class DecoderFFmpeg : public IDecoder
{
public:
    ~DecoderFFmpeg() override;
    bool init()                                         override;
    void decode(EncodedPacketPtr pkt, FrameCallback cb) override;
private:
    void cleanup();
    AVCodecContext* ctx_    = nullptr;
    AVFrame*        avf_    = nullptr;
    AVPacket*       pkt_    = nullptr;
    SwsContext*     sws_    = nullptr;
    int             sw_  = 0, sh_ = 0;
};
