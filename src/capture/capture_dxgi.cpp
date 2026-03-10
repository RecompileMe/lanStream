#ifdef PLATFORM_WINDOWS
#include "capture_dxgi.hpp"
#include <iostream>

bool CaptureDXGI::init(int display_index) {
    D3D_FEATURE_LEVEL feat;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device_, &feat, &context_);
    if (FAILED(hr)) { std::cerr << "D3D11CreateDevice failed\n"; return false; }

    Microsoft::WRL::ComPtr<IDXGIDevice>  dxgi_dev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput>  output;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    device_.As(&dxgi_dev);
    dxgi_dev->GetAdapter(&adapter);
    if (FAILED(adapter->EnumOutputs(display_index, &output))) {
        std::cerr << "EnumOutputs failed\n"; return false;
    }
    DXGI_OUTPUT_DESC desc{}; output->GetDesc(&desc);
    width_  = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
    height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

    output.As(&output1);
    hr = output1->DuplicateOutput(device_.Get(), &dup_);
    if (FAILED(hr)) { std::cerr << "DuplicateOutput failed\n"; return false; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_; td.Height = height_;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device_->CreateTexture2D(&td, nullptr, &staging_);
    return true;
}

bool CaptureDXGI::start(FrameCallback cb) {
    running_ = true;
    thread_ = std::thread([this, cb]{ capture_loop(cb); });
    return true;
}

void CaptureDXGI::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void CaptureDXGI::capture_loop(FrameCallback cb) {
    while (running_) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        Microsoft::WRL::ComPtr<IDXGIResource> res;
        HRESULT hr = dup_->AcquireNextFrame(16, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;
        if (fi.LastPresentTime.QuadPart == 0) { dup_->ReleaseFrame(); continue; }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        context_->CopyResource(staging_.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);

        auto f = std::make_shared<RawFrame>();
        f->timestamp_us = now_us();
        f->width    = width_;
        f->height   = height_;
        f->linesize = (int)mapped.RowPitch;
        f->format   = PixelFormat::BGRA;
        f->data.resize(mapped.RowPitch * height_);
        memcpy(f->data.data(), mapped.pData, f->data.size());

        context_->Unmap(staging_.Get(), 0);
        dup_->ReleaseFrame();
        cb(std::move(f));
    }
}
#endif
