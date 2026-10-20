#pragma once
#include "common/frame.hpp"
#include <functional>

class IEncoder
{
public:
    using PacketCallback = std::function<void(EncodedPacketPtr)>;
    virtual ~IEncoder() = default;
    virtual bool init(int width, int height, int fps, int bitrate_kbps) = 0;
    virtual void encode(RawFramePtr frame, PacketCallback callback) = 0;
    virtual void flush(PacketCallback callback) = 0;
};
