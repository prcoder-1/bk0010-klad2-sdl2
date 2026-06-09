/*
 * Главный модуль Linux/SDL2-порта КЛАД-2.
 *
 * Фаза 1: подключена «виртуальная БК» (bk_mem + перехват регистров),
 * видеорендер и заглушки ввода/звука. Чтобы было видно, что цепочка
 * `bk_mem[MEM_VIDEO] → SDL_Texture → окно` работает, в видеоОЗУ кладётся
 * тестовый паттерн (4 цветные вертикальные полосы + 1-пиксельная полоса
 * ниже для проверки порядка пикселей в байте).
 *
 * Окно — 256×256 (логически), целочисленный масштаб 2×.
 * Esc/SDL_QUIT — выход.
 */

#include "audio.h"
#include "bk.h"
#include "input.h"
#include "video.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    BK_SCREEN_W   = 256,
    BK_SCREEN_H   = 256,
    INITIAL_SCALE = 2
};

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;

static int sdl_setup(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    g_window = SDL_CreateWindow(
        "КЛАД-2",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        BK_SCREEN_W * INITIAL_SCALE, BK_SCREEN_H * INITIAL_SCALE,
        SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(g_renderer, BK_SCREEN_W, BK_SCREEN_H);
    SDL_RenderSetIntegerScale(g_renderer, SDL_TRUE);
    return 0;
}

static void sdl_teardown(void)
{
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
}

/*
 * Тестовый паттерн в bk_mem[MEM_VIDEO]:
 *  - строки 0..127:  четыре вертикальных полосы цветов 0..3
 *    (по 16 байт = 64 пикселя каждая);
 *  - строки 128..255: байт 0xE4 = биты 11_10_01_00 — пиксели по 4 пикс/байт
 *    идут как чёрный/синий/зелёный/красный слева направо.
 * Если оба пояса смотрятся «как ожидалось» — палитра и LSB-first
 * упаковка пикселей корректны.
 */
static void fill_demo_pattern(void)
{
    uint8_t *vid = &bk_mem[MEM_VIDEO];

    for (int y = 0; y < SCREEN_PIX_HEIGHT / 2; y++) {
        uint8_t *row = &vid[y * SCREEN_BYTE_WIDTH];
        for (int xb = 0; xb < SCREEN_BYTE_WIDTH; xb++) {
            /* 64 байта в строке делим на 4 равные полосы по 16 байт. */
            unsigned color = (unsigned)(xb / 16) & 0x3;
            row[xb] = (uint8_t)(color * 0x55u); /* C|C<<2|C<<4|C<<6 */
        }
    }
    for (int y = SCREEN_PIX_HEIGHT / 2; y < SCREEN_PIX_HEIGHT; y++) {
        uint8_t *row = &vid[y * SCREEN_BYTE_WIDTH];
        for (int xb = 0; xb < SCREEN_BYTE_WIDTH; xb++) {
            row[xb] = 0xE4u; /* 11 10 01 00 — пиксели 0..3 = ч/с/з/к */
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (sdl_setup() != 0) {
        sdl_teardown();
        return EXIT_FAILURE;
    }

    bk_reset();
    if (audio_init() != 0 || input_init() != 0 ||
        video_init(g_renderer) != 0)
    {
        video_shutdown();
        input_shutdown();
        audio_shutdown();
        sdl_teardown();
        return EXIT_FAILURE;
    }

    fill_demo_pattern();

    SDL_Event ev;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else input_handle_sdl_event(&ev);
                break;
            default:
                input_handle_sdl_event(&ev);
                break;
            }
        }
        video_render_frame(g_renderer);
        SDL_RenderPresent(g_renderer);
    }

    video_shutdown();
    input_shutdown();
    audio_shutdown();
    sdl_teardown();
    return EXIT_SUCCESS;
}
