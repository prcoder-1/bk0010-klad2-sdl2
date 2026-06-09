/*
 * Скелет SDL2-приложения для порта КЛАД-2 (Фаза 0).
 *
 * На этой фазе ещё нет виртуальной БК и интерпретатора — просто открыто
 * окно правильного размера, чтобы дальше его наполнять. Размер логического
 * экрана — 256×256 (как цветной экран БК-0010), стартовое окно вдвое
 * больше; целочисленный nearest-neighbour-масштаб.
 *
 * Выход — по Esc, SDL_QUIT или Ctrl-C.
 */

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    BK_SCREEN_W      = 256, /* Цветной режим, SCREEN_CLR_PIX_WIDTH */
    BK_SCREEN_H      = 256, /* SCREEN_PIX_HEIGHT */
    INITIAL_SCALE    = 2,   /* Стартовый множитель окна */
};

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;

static int sdl_setup(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
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

    /* Логический размер = БК-экран; целочисленный масштаб, nearest-neighbour. */
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

static void render_frame(void)
{
    /* Пока экран чёрный — позже сюда придёт video.c из bk_mem[MEM_VIDEO]. */
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (sdl_setup() != 0) {
        sdl_teardown();
        return EXIT_FAILURE;
    }

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
                break;
            }
        }
        render_frame();
    }

    sdl_teardown();
    return EXIT_SUCCESS;
}
