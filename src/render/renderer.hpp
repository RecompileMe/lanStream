#pragma once
#include "common/frame.hpp"

class IRenderer
{
public:
    virtual ~IRenderer() = default;
    virtual bool init(int width, int height, const std::string& title) = 0;
    virtual bool render(RawFramePtr frame) = 0;
    virtual bool poll_events() = 0;
};
