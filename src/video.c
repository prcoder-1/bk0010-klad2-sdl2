/*
 * Видео-бэкенд: bk_mem[MEM_VIDEO..+0x4000] → SDL_Texture → окно.
 *
 * Раскладка видеоОЗУ БК-0010 в цветном режиме:
 *  - 256 строк, по 64 байта;
 *  - в каждом байте 4 пикселя по 2 бита, младший пиксель — в младших
 *    битах (LSB-first): пиксель 0 = биты 0..1, пиксель 3 = биты 6..7;
 *  - палитра по умолчанию: 00 чёрный, 01 синий, 10 зелёный, 11 красный.
 *
 * Регистр REG_V_SCROLL пока не учитывается — выводим линейно с MEM_VIDEO.
 * Скролл, расширенная память и альтернативные палитры — позже.
 */

#include "video.h"
#include "bk.h"

#include <stdio.h>

/* Палитра БК-0010 по умолчанию (палитра 0). 4 цвета × RGBA8888. */
static const uint32_t bk_palette_default[4] = {
    0x000000FFu, /* 00 — чёрный */
    0x0000FFFFu, /* 01 — синий  */
    0x00FF00FFu, /* 10 — зелёный*/
    0xFF0000FFu  /* 11 — красный*/
};

static SDL_Texture *g_screen_tex;

int video_init(SDL_Renderer *renderer)
{
    g_screen_tex = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        SCREEN_CLR_PIX_WIDTH, SCREEN_PIX_HEIGHT);
    if (!g_screen_tex) {
        fprintf(stderr, "video_init: SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetTextureScaleMode(g_screen_tex, SDL_ScaleModeNearest);
    return 0;
}

void video_shutdown(void)
{
    if (g_screen_tex) {
        SDL_DestroyTexture(g_screen_tex);
        g_screen_tex = NULL;
    }
}

static void unpack_video_to_rgba(uint32_t *dst, int dst_pitch_px)
{
    const uint8_t *src = &bk_mem[MEM_VIDEO];

    for (int y = 0; y < SCREEN_PIX_HEIGHT; y++) {
        uint32_t *row = dst + y * dst_pitch_px;
        for (int xb = 0; xb < SCREEN_BYTE_WIDTH; xb++) {
            uint8_t b = src[y * SCREEN_BYTE_WIDTH + xb];
            int px = xb * SCREEN_CLR_PX_PER_BYTE;
            /* LSB-first: пиксель 0 — биты 0..1, пиксель 3 — биты 6..7. */
            row[px + 0] = bk_palette_default[(b >> 0) & 0x3];
            row[px + 1] = bk_palette_default[(b >> 2) & 0x3];
            row[px + 2] = bk_palette_default[(b >> 4) & 0x3];
            row[px + 3] = bk_palette_default[(b >> 6) & 0x3];
        }
    }
}

void video_render_frame(SDL_Renderer *renderer)
{
    void *pixels;
    int pitch_bytes;

    if (SDL_LockTexture(g_screen_tex, NULL, &pixels, &pitch_bytes) != 0) {
        fprintf(stderr, "video_render_frame: SDL_LockTexture: %s\n",
                SDL_GetError());
        return;
    }

    unpack_video_to_rgba((uint32_t *)pixels, pitch_bytes / 4);

    SDL_UnlockTexture(g_screen_tex);

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, g_screen_tex, NULL, NULL);
}
