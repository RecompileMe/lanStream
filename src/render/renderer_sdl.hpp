// renderer_sdl.hpp
#pragma once
#include "renderer.hpp"
#include <SDL2/SDL.h>

class RendererSDL : public IRenderer
{
public:
    ~RendererSDL() override;
    bool init(int width, int height, const std::string& title) override;
    bool render(RawFramePtr frame_ptr) override;
    bool poll_events() override;
private:
    void cleanup();
    SDL_Window*   window_   = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture*  texture_  = nullptr;
    int width_ = 0, height_ = 0;
};
