/**
 * @file present.c
 * @brief Presentation: the finished frame scaled to the window.
 *
 * The renderer produces the view in GBA pixels (240x160 up to the extended
 * view size). This stage uploads it to a GPU texture and draws it into the
 * window, as large as fits with square pixels (integer multiples with
 * integer_scaling), centered with black bars. The scaling is nearest
 * neighbour, so every GBA pixel becomes a crisp block.
 *
 * This is the place for whole-picture post-processing and upscaling: a GPU
 * shader on the texture (CRT, scanlines, smoothing), or a CPU / AI upscaler
 * that turns the frame into a larger image before it is uploaded. Per-object
 * changes (only the bosses, only scaled sprites) belong in scaling.c instead,
 * because here the picture is already flat.
 */
#include "shader.h"

#include <SDL.h>

SDL_Texture* Present_CreateTexture(SDL_Renderer* renderer, int maxW, int maxH) {
    /* how the texture is filtered when drawn larger: "nearest" (alternatives: "linear", "best") */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    return SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, maxW, maxH);
}

void Present_Frame(SDL_Renderer* renderer, SDL_Texture* texture, const uint32_t* frame, int pitchPixels, int viewW,
                   int viewH, bool integerScaling) {
    int outW, outH;
    SDL_Rect src, dst;
    float scale;
    SDL_GetRendererOutputSize(renderer, &outW, &outH);
    SDL_UpdateTexture(texture, NULL, frame, pitchPixels * 4);
    src.x = 0;
    src.y = 0;
    src.w = viewW;
    src.h = viewH;
    /* fit the logical view into the window keeping square pixels */
    scale = (float)outW / viewW;
    if ((float)outH / viewH < scale)
        scale = (float)outH / viewH;
    if (integerScaling && scale >= 1.0f)
        scale = (float)(int)scale;
    dst.w = (int)(viewW * scale);
    dst.h = (int)(viewH * scale);
    dst.x = (outW - dst.w) / 2;
    dst.y = (outH - dst.h) / 2;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, &src, &dst);
    SDL_RenderPresent(renderer);
}
