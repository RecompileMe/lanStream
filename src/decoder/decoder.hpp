#pragma once
#include "common/frame.hpp"
#include <functional>

class IDecoder
{
public:
    using FrameCallback = std::function<void(RawFramePtr)>;
    virtual ~IDecoder() = default;
    virtual bool init()                                        = 0;
    virtual void decode(EncodedPacketPtr packet_ptr, FrameCallback callback) = 0;
};
