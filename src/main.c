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
    /* Начальное значение SP перед запуском .BIN. Верх пользовательского
     * ОЗУ — прямо под видеобуфером MEM_VIDEO = 0o40000. */
    INITIAL_SP = 0040000u - 2u
};

/* Коэффициент увеличения окна (по-умолчанию ×2). Регулируется
 * флагом `--scale N` (1..4). Не static — пригодится для будущей
 * отрисовки. */
static int g_scale = 2;

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
        BK_SCREEN_W * g_scale, BK_SCREEN_H * g_scale,
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

/* ---------------- Snapshot для мгновенного старта -------------------- *
 *
 * PIC-релокатор КЛАД-2 (адреса 0x01DA..~0x027F) тратит ~50000 шагов
 * интерпретатора на распаковку самого себя в финальный layout. Эта
 * работа детерминирована — каждый запуск даёт ровно тот же образ.
 * Чтобы не тратить эти 5–10 секунд при каждом старте порта, мы
 * сохраняем состояние ВМ сразу после релокатора (bk_mem + регистры +
 * PSW) в файл и при старте загружаем его, минуя релокацию.
 *
 * Признак «вышли из релокатора» — первый PC ≥ 0x300: всё ниже либо
 * в системной области (0..0x40), либо в самом релокаторе.
 *
 * Формат файла klad2.snap:
 *   bytes 0..3   — magic "KLD2"
 *   bytes 4..7   — версия (uint32_t LE), сейчас 1
 *   bytes 8..23  — регистры R0..R7 (8 × uint16_t LE)
 *   bytes 24..25 — PSW
 *   bytes 26..29 — резерв (0)
 *   bytes 30..65565 — bk_mem (64 КиБ)
 */
enum { SNAP_MAGIC_VER = 1, SNAP_HDR_SIZE = 30 };

static int snap_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "snap_save: не могу открыть '%s'\n", path);
        return -1;
    }
    uint32_t ver = SNAP_MAGIC_VER;
    uint32_t resv = 0;
    fwrite("KLD2", 1, 4, f);
    fwrite(&ver, sizeof ver, 1, f);
    fwrite(cpu.r, sizeof(uint16_t), 8, f);
    fwrite(&cpu.psw, sizeof cpu.psw, 1, f);
    fwrite(&resv, sizeof resv, 1, f);
    size_t n = fwrite(bk_mem, 1, BK_MEM_SIZE, f);
    fclose(f);
    if (n != BK_MEM_SIZE) {
        fprintf(stderr, "snap_save: записано %zu из %u\n",
                n, (unsigned)BK_MEM_SIZE);
        return -1;
    }
    fprintf(stderr, "snapshot записан в %s (PC=%06o, %u байт)\n",
            path, (unsigned)cpu.r[7],
            (unsigned)(SNAP_HDR_SIZE + BK_MEM_SIZE));
    return 0;
}

static int snap_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snap_load: не могу открыть '%s'\n", path);
        return -1;
    }
    char magic[4]; uint32_t ver, resv;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "KLD2", 4) != 0) {
        fprintf(stderr, "snap_load: неверная сигнатура в '%s'\n", path);
        fclose(f); return -1;
    }
    if (fread(&ver, sizeof ver, 1, f) != 1 || ver != SNAP_MAGIC_VER) {
        fprintf(stderr, "snap_load: версия %u не поддерживается\n", ver);
        fclose(f); return -1;
    }
    fread(cpu.r, sizeof(uint16_t), 8, f);
    fread(&cpu.psw, sizeof cpu.psw, 1, f);
    fread(&resv, sizeof resv, 1, f);
    size_t n = fread(bk_mem, 1, BK_MEM_SIZE, f);
    fclose(f);
    if (n != BK_MEM_SIZE) {
        fprintf(stderr, "snap_load: прочитано %zu из %u\n",
                n, (unsigned)BK_MEM_SIZE);
        return -1;
    }
    cpu.halted = 0;
    fprintf(stderr,
            "snapshot загружен из %s: PC=%06o, SP=%06o, "
            "R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o\n",
            path, cpu.r[7], cpu.r[6],
            cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5]);
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
    const char *snap_save_path = NULL;
    const char *snap_load_path = NULL;

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
        } else if (strcmp(argv[i], "--save-state") == 0 && i + 1 < argc) {
            /* Один раз прогнать релокатор и записать состояние в файл. */
            snap_save_path = argv[++i];
        } else if (strcmp(argv[i], "--fast-start") == 0 && i + 1 < argc) {
            /* Мгновенный старт: загрузить snapshot, минуя релокатор. */
            snap_load_path = argv[++i];
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            int s = (int)strtol(argv[++i], NULL, 0);
            if (s < 1 || s > 4) {
                fprintf(stderr,
                        "--scale: ожидаемо 1..4, получено %d\n", s);
                return EXIT_FAILURE;
            }
            g_scale = s;
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                    "Неизвестный флаг: %s\n"
                    "Использование: %s [--trace] [--max-steps N] "
                    "[--rom path/to/mon.rom] [--speed N] [--scale N (1..4)] "
                    "[--dump-memory path] [--save-state path] "
                    "[--fast-start path] [путь/к/BIN]\n",
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
    if (snap_load_path) {
        /* Мгновенный старт: bk_mem и регистры берём из snapshot,
         * BIN-файл не нужен (но загруженный ROM-шрифт и образ ROM —
         * нужны, потому что snapshot их НЕ хранит: они одинаковы на
         * каждом запуске и приходят из локального файла). */
        if (snap_load(snap_load_path) != 0) {
            video_shutdown(); input_shutdown(); audio_shutdown(); sdl_teardown();
            return EXIT_FAILURE;
        }
        pdp11_set_emt_handler(emt_dispatch);
        pdp11_set_trace(trace);
        run_cpu = 1;
    } else if (bin_path) {
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

        /* --save-state: прогнать релокатор по pdp11_step до первого
         * «обычного» PC (≥ 0x300), сохранить состояние, выйти. */
        if (snap_save_path) {
            int steps = 0;
            while (!cpu.halted && cpu.r[7] < 0x0300 && steps < 5000000) {
                pdp11_step();
                steps++;
            }
            if (cpu.halted) {
                fprintf(stderr,
                        "save-state: ЦП остановлен до выхода из "
                        "релокатора (PC=%06o, шагов %d)\n",
                        cpu.r[7], steps);
            } else {
                fprintf(stderr,
                        "save-state: релокатор завершён за %d шагов, "
                        "PC=%06o\n", steps, cpu.r[7]);
                snap_save(snap_save_path);
            }
            video_shutdown(); input_shutdown();
            audio_shutdown(); sdl_teardown();
            return cpu.halted ? EXIT_FAILURE : EXIT_SUCCESS;
        }
    } else {
        fill_demo_pattern();
    }

    SDL_Event ev;
    int running = 1;
    int paused = 0;
    int fullscreen = 0;
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
                } else if (ev.key.keysym.sym == SDLK_F11) {
                    /* Toggle полноэкранный режим (без смены разрешения —
                     * SDL_WINDOW_FULLSCREEN_DESKTOP сохраняет рабочий стол). */
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(g_window,
                        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                } else if (ev.key.keysym.sym == SDLK_PAUSE ||
                           ev.key.keysym.sym == SDLK_F9) {
                    /* Pause/F9 — переключение паузы (без обработки ввода
                     * игрой; CPU не тикает). */
                    paused = !paused;
                    SDL_SetWindowTitle(g_window,
                        paused ? "КЛАД-2 (пауза)" : "КЛАД-2");
                } else input_handle_sdl_event(&ev);
                break;
            default:
                input_handle_sdl_event(&ev);
                break;
            }
        }

        if (run_cpu && !cpu.halted && !paused) {
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
