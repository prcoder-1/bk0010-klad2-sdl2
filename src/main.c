/*
 * Главный модуль Linux/SDL2-порта КЛАД-2.
 *
 * Если в аргументах указан путь к .BIN — загружаем образ в виртуальную
 * БК, ставим PC на адрес загрузки и крутим интерпретатор PDP-11 между
 * кадрами SDL2. Без аргумента — старый Фазы-1 демо-паттерн в видеоОЗУ
 * (полезно для проверки видеоподсистемы изолированно).
 *
 * Использование:
 *   ./klad2 [--trace] [путь/к/BIN]
 *
 * Esc / SDL_QUIT — выход.
 */

#include "audio.h"
#include "bk.h"
#include "emt.h"
#include "input.h"
#include "pdp11.h"
#include "video.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    BK_SCREEN_W   = 256,
    BK_SCREEN_H   = 256,
    INITIAL_SCALE = 2,
    /* Начальное значение SP перед запуском .BIN. Верх пользовательского
     * ОЗУ — прямо под видеобуфером MEM_VIDEO = 0o40000. */
    INITIAL_SP = 0040000u - 2u
};

/* Сколько инструкций крутим между кадрами SDL2 (60 fps под VSYNC).
 * По-хорошему БК-0010 на 3 МГц даёт ≈ 600 тыс. инструкций/с,
 * т.е. ~10 тыс. на кадр при 60 fps. Но КЛАД-2 использует busy-loop
 * задержки и оптимизировался под средний (~5 c/instr) PDP-11; при
 * 10 000 инструкций/кадр геймплей идёт визуально быстрее оригинала.
 * Значение 4000 подобрано визуальным сравнением с эмулятором.
 * Регулируется флагом --speed N.
 * Не static — нужен из audio.c для подбора частоты сэмплинга. */
int g_steps_per_frame = 4000;

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;

/* ---------------- SDL setup/teardown --------------------------------- */

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

/* ---------------- Демо-паттерн Фазы 1 ------------------------------- */

static void fill_demo_pattern(void)
{
    uint8_t *vid = &bk_mem[MEM_VIDEO];
    for (int y = 0; y < SCREEN_PIX_HEIGHT / 2; y++) {
        uint8_t *row = &vid[y * SCREEN_BYTE_WIDTH];
        for (int xb = 0; xb < SCREEN_BYTE_WIDTH; xb++) {
            unsigned color = (unsigned)(xb / 16) & 0x3;
            row[xb] = (uint8_t)(color * 0x55u);
        }
    }
    for (int y = SCREEN_PIX_HEIGHT / 2; y < SCREEN_PIX_HEIGHT; y++) {
        uint8_t *row = &vid[y * SCREEN_BYTE_WIDTH];
        for (int xb = 0; xb < SCREEN_BYTE_WIDTH; xb++) row[xb] = 0xE4u;
    }
}

/* ---------------- main ----------------------------------------------- */

/* Дамп всей виртуальной БК-памяти в файл (64 КиБ).
 * Используется для разборки кода КЛАД-2 ПОСЛЕ работы PIC-релокатора —
 * сырой BIN распакован в bk_mem, и dump → pdp11-aout-objdump даёт
 * читаемый код, а не зашифрованную PIC-таблицу. */
static int dump_memory(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "dump_memory: не могу открыть '%s'\n", path);
        return -1;
    }
    size_t n = fwrite(bk_mem, 1, BK_MEM_SIZE, f);
    fclose(f);
    if (n != BK_MEM_SIZE) {
        fprintf(stderr, "dump_memory: записано %zu вместо %u\n",
                n, (unsigned)BK_MEM_SIZE);
        return -1;
    }
    fprintf(stderr, "Дамп %u байт записан в %s (PC=%06o)\n",
            (unsigned)BK_MEM_SIZE, path, (unsigned)cpu.r[7]);
    return 0;
}

int main(int argc, char **argv)
{
    int trace = 0;
    long max_steps = -1;
    const char *bin_path = NULL;
    const char *rom_path =
        "/home/prcoder/emulators/БК-0010-01/bkemu/ROM/bk10_017_mon.rom";
    const char *dump_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            g_steps_per_frame = (int)strtol(argv[++i], NULL, 0);
            if (g_steps_per_frame < 100) g_steps_per_frame = 100;
        } else if (strcmp(argv[i], "--dump-memory") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                    "Неизвестный флаг: %s\n"
                    "Использование: %s [--trace] [--max-steps N] "
                    "[--rom path/to/mon.rom] [--speed N] "
                    "[--dump-memory path] [путь/к/BIN]\n",
                    argv[i], argv[0]);
            return EXIT_FAILURE;
        } else {
            bin_path = argv[i];
        }
    }

    if (sdl_setup() != 0) { sdl_teardown(); return EXIT_FAILURE; }

    bk_reset();
    if (audio_init() != 0 || input_init() != 0 ||
        video_init(g_renderer) != 0)
    {
        video_shutdown(); input_shutdown(); audio_shutdown(); sdl_teardown();
        return EXIT_FAILURE;
    }

    /* Загрузим БК-шрифт из Monitor-ПЗУ, если он доступен. Без шрифта
     * EMT 016 не рисует символы, но всё прочее работает. */
    if (bk_load_rom_font(rom_path) != 0) {
        fprintf(stderr,
                "Шрифт БК недоступен (%s). EMT 016 будет молча "
                "пропускать печатные символы.\n", rom_path);
    } else {
        fprintf(stderr, "Шрифт БК загружен из %s\n", rom_path);
    }

    /* Также загрузим полный образ Monitor-ПЗУ в bk_mem[0o100000..],
     * чтобы КЛАД-2 могла вызывать ROM-функции напрямую через JSR
     * (в частности, звуковой сигнал «плюканье клавиш»). */
    if (bk_load_rom_image(rom_path) != 0) {
        fprintf(stderr,
                "Образ Monitor-ПЗУ не загружен — звуки выстрела/стен "
                "могут отсутствовать.\n");
    } else {
        fprintf(stderr, "Образ Monitor-ПЗУ загружен в bk_mem[0o100000]\n");
    }

    int run_cpu = 0;
    if (bin_path) {
        uint16_t entry;
        if (bk_load_bin(bin_path, &entry) != 0) {
            video_shutdown(); input_shutdown(); audio_shutdown(); sdl_teardown();
            return EXIT_FAILURE;
        }
        pdp11_set_emt_handler(emt_dispatch);
        pdp11_set_trace(trace);
        pdp11_reset(entry, INITIAL_SP);
        run_cpu = 1;
        fprintf(stderr,
                "Загружен '%s': точка входа %06o, SP=%06o\n",
                bin_path, entry, INITIAL_SP);
    } else {
        fill_demo_pattern();
    }

    SDL_Event ev;
    int running = 1;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0; break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else if (ev.key.keysym.sym == SDLK_F12) {
                    /* Снимок всей памяти БК (64 КиБ) в файл с меткой
                     * времени — нужен для прогрессивной декомпиляции:
                     * КЛАД-2 распаковывает разный код в разных режимах
                     * (меню/игра/смерть), и каждый дамп даёт срез того,
                     * что в данный момент уже лежит в памяти. */
                    char path[64];
                    time_t t = time(NULL);
                    struct tm tm; localtime_r(&t, &tm);
                    snprintf(path, sizeof path,
                             "klad2_dump_%02d%02d%02d.bin",
                             tm.tm_hour, tm.tm_min, tm.tm_sec);
                    dump_memory(path);
                } else input_handle_sdl_event(&ev);
                break;
            default:
                input_handle_sdl_event(&ev);
                break;
            }
        }

        if (run_cpu && !cpu.halted) {
            int n = pdp11_run_n(g_steps_per_frame);
            bk_tick_timer((unsigned)n);
            audio_flush();
            if (max_steps > 0) {
                max_steps -= n;
                if (max_steps <= 0) {
                    fprintf(stderr,
                            "max-steps достигнут на PC=%06o\n",
                            (unsigned)cpu.r[7]);
                    cpu.halted = 1;
                }
            }
            if (cpu.halted) {
                fprintf(stderr,
                        "ЦП остановлен (HALT/ошибка) на PC=%06o\n",
                        (unsigned)cpu.r[7]);
                if (dump_path) { dump_memory(dump_path); dump_path = NULL; }
            }
        }

        video_render_frame(g_renderer);
        SDL_RenderPresent(g_renderer);
    }

    video_shutdown(); input_shutdown(); audio_shutdown(); sdl_teardown();
    return EXIT_SUCCESS;
}
