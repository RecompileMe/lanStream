#pragma once
#include "encoder.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class EncoderFFmpeg : public IEncoder {
public:
    ~EncoderFFmpeg() override { cleanup(); }
    bool init(int width, int height,
              int fps = 60, int bitrate_kbps = 8000) override;
    void encode(RawFramePtr frame, PacketCallback cb) override;
    void flush(PacketCallback cb)                     override;
private:
    void cleanup();
    void do_encode(AVFrame* frame, PacketCallback& cb);
    AVCodecContext* ctx_     = nullptr;
    AVFrame*        avf_     = nullptr;
    AVPacket*       pkt_     = nullptr;
    SwsContext*     sws_     = nullptr;
    uint32_t        fid_     = 0;
    int             width_   = 0;
    int             height_  = 0;
};
