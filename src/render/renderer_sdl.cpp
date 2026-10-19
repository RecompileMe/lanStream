// renderer_sdl.cpp
#include "renderer_sdl.hpp"
#include <iostream>

bool RendererSDL::init(int width, int height, const std::string& title)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
        return false;

    width_ = width; height_ = height;
    window_ = SDL_CreateWindow(title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    if (!window_)
        return false;

    renderer_ = SDL_CreateRenderer(window_, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_)
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);

    texture_ = SDL_CreateTexture(renderer_,
        SDL_PIXELFORMAT_BGRA32,
        SDL_TEXTUREACCESS_STREAMING, width, height);

    if (texture_)
        return true;
    else
        return false;
}

bool RendererSDL::render(RawFramePtr frame_ptr)
{
    if (!frame_ptr)
        return true;

    // Rebuild the texture when the resolution changes
    if (frame_ptr->width != width_ || frame_ptr->height != height_)
    {
        width_ = frame_ptr->width; height_ = frame_ptr->height;
        SDL_DestroyTexture(texture_);
        texture_ = SDL_CreateTexture(renderer_,
            SDL_PIXELFORMAT_BGRA32,
            SDL_TEXTUREACCESS_STREAMING, width_, height_);
        SDL_SetWindowSize(window_, width_, height_);
    }
    SDL_UpdateTexture(texture_, nullptr, frame_ptr->data.data(), frame_ptr->linesize);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
    return true;
}

bool RendererSDL::poll_events()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            return false;

        if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
            return false;
    }
    return true;
}

void RendererSDL::cleanup()
{
    if (texture_)
        SDL_DestroyTexture(texture_);

    if (renderer_)
        SDL_DestroyRenderer(renderer_);

    if (window_)
        SDL_DestroyWindow(window_);

    SDL_Quit();
}

RendererSDL::~RendererSDL()
{
    cleanup();
}