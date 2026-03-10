#pragma once
#ifdef PLATFORM_WINDOWS
#include "capture.hpp"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>

class CaptureDXGI : public ICapture {
public:
    ~CaptureDXGI() override { stop(); }
    bool init(int display_index = 0) override;
    bool start(FrameCallback cb)     override;
    void stop()                      override;
    int  width()  const override { return width_;  }
    int  height() const override { return height_; }
private:
    void capture_loop(FrameCallback cb);
    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        staging_;
    int  width_ = 0, height_ = 0;
    std::thread       thread_;
    std::atomic<bool> running_{false};
};
#endif
