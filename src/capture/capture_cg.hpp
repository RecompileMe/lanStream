#pragma once
#ifdef PLATFORM_MACOS
#include "capture.hpp"
#include <thread>
#include <atomic>

class CaptureCG : public ICapture {
public:
    ~CaptureCG() override;
    bool init(int display_index = 0) override;
    bool start(FrameCallback cb)     override;
    void stop()                      override;
    int  width()  const override { return width_;  }
    int  height() const override { return height_; }
private:
    struct Impl;
    int           display_index_ = 0;
    int           width_ = 0, height_ = 0;
    std::thread   thread_;
    std::atomic<bool> running_{false};
    FrameCallback cb_;
};
#endif
