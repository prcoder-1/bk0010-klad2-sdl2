/*
 * SDL2-аудио для эмулятора КЛАД-2.
 *
 * Динамик БК-0010 — это один бит EXT_DEV_MAG_KEY (бит 6) в регистре
 * REG_EXT_DEV = 0o177716. Звуки в играх генерируются прямым переключе-
 * нием этого бита по таймингу CPU-цикла, так что характерные «писк»,
 * «бум» и «трель» — это меандр с частотой, заданной задержкой между
 * переключениями.
 *
 * Здесь мы:
 *   - открываем SDL2-аудио на 44.1 кГц 16-битный signed моно;
 *   - audio_speaker_set(level) вызывается из bk.c на каждое изменение
 *     бита — обновляет текущий уровень «динамика» (+/- амплитуда);
 *   - audio_step() — вызывается после каждого pdp11_step из главного
 *     цикла, накапливает дробное число «выходных сэмплов на инструк-
 *     цию», и при достижении 1.0 кладёт сэмпл в кадровый буфер;
 *   - audio_flush() — после кадра выкидывает накопленные сэмплы в
 *     SDL_QueueAudio.
 *
 * Частота сэмплинга подобрана под текущую скорость интерпретатора
 * (g_steps_per_frame × 60 fps ≈ 240 тыс. инструкций/с), чтобы выход
 * по времени совпадал с реальным проигрыванием. Это значит, что
 * меандр получится «более низким» по частоте, чем на оригинальной БК,
 * пропорционально замедлению — но узнаваемым.
 */

#include "audio.h"
#include <SDL2/SDL.h>
#include <stdio.h>

#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_AMPLITUDE    9000          /* безопасный уровень громкости */
#define AUDIO_FRAME_BUF    2048          /* запас по сэмплам на один кадр */

/* Скорость интерпретатора в инструкциях/с (см. main.c, g_steps_per_frame
 * × 60 fps по умолчанию = 240000). По этому числу настраиваем «сколько
 * аудио-сэмплов приходится на одну инструкцию». */
extern int g_steps_per_frame;

static SDL_AudioDeviceID g_dev;
static int16_t           g_level = -AUDIO_AMPLITUDE;
static double            g_acc;          /* накопитель дробных сэмплов */
static double            g_samples_per_step; /* AUDIO_RATE / CPU_RATE */
static int16_t           g_buf[AUDIO_FRAME_BUF];
static int               g_buf_pos;

int audio_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio_init: SDL_InitSubSystem AUDIO: %s\n",
                SDL_GetError());
        return -1;
    }

    SDL_AudioSpec want = {0}, have = {0};
    want.freq     = AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = NULL; /* используем SDL_QueueAudio (push-режим) */

    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) {
        fprintf(stderr, "audio_init: SDL_OpenAudioDevice: %s\n",
                SDL_GetError());
        return -1;
    }

    /* Подсчёт частоты сэмплинга относительно скорости CPU.
     * При g_steps_per_frame=4000 и 60 fps это ≈ 0.184 сэмпла на
     * инструкцию (44100 / 240000). */
    int cpu_rate = g_steps_per_frame * 60;
    if (cpu_rate < 1) cpu_rate = 240000;
    g_samples_per_step = (double)AUDIO_SAMPLE_RATE / (double)cpu_rate;

    SDL_PauseAudioDevice(g_dev, 0);
    return 0;
}

void audio_shutdown(void)
{
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
}

static unsigned g_write_count;   /* любые записи в EXT_DEV.MAG_KEY */
static unsigned g_toggle_count;  /* только смены уровня */

void audio_speaker_set(int level)
{
    int16_t new_level = level ? AUDIO_AMPLITUDE : -AUDIO_AMPLITUDE;
    g_write_count++;
    if (new_level != g_level) g_toggle_count++;
    g_level = new_level;
}

/* Раз в секунду печатает счётчики:
 *   write   — все записи в EXT_DEV (даже с тем же значением);
 *   toggle  — только реальные переходы 0↔1.
 * Если КЛАД-2 для звука действительно тикает динамик, оба счётчика
 * должны расти. Если оба 0 — КЛАД-2 вообще не пишет в EXT_DEV
 * во время игры. */
static void audio_log_tick(void)
{
    static Uint32 last_tick;
    if (!getenv("KLAD2_AUDIO_LOG")) return;
    Uint32 now = SDL_GetTicks();
    if (now - last_tick >= 1000) {
        fprintf(stderr, "[audio] write=%u toggle=%u за секунду\n",
                g_write_count, g_toggle_count);
        g_write_count = 0;
        g_toggle_count = 0;
        last_tick = now;
    }
}

void audio_step(void)
{
    g_acc += g_samples_per_step;
    if (g_acc >= 1.0 && g_buf_pos < AUDIO_FRAME_BUF) {
        g_buf[g_buf_pos++] = g_level;
        g_acc -= 1.0;
    }
}

void audio_emit_bell(void)
{
    if (!g_dev) return;

    /* Точная копия ROM-процедуры из bk10_017_mon.rom (адрес 0o102032):
     *
     *   mov $50, r2          ; 50 итераций внешнего цикла
     *   mov $100, r3         ; r3 = 100 — счётчик задержки
     * loop:
     *   mov $320, @#177716   ; бит 6 (динамик) = 1
     *   mov r3, r0           ; задержка через SOB-цикл
     *   sob r0, *            ;
     *   mov $220, @#177716   ; бит 6 = 0
     *   mov r3, r0
     *   sob r0, *
     *   dec r3               ; уменьшаем r3 → частота растёт (свип)
     *   sob r2, loop         ; повторить 50 раз
     *
     * На БК-3МГц SOB ≈ 10 циклов ≈ 3.3 мкс, поэтому r3=100 даёт
     * полупериод 333 мкс (1500 Гц), а r3=50 — 165 мкс (3000 Гц).
     * Итог: ~25 мс «плик» со свипом 1.5 → 3 кГц. */
    enum {
        BELL_OUTER     = 50,
        BELL_R3_START  = 100,
        BELL_R3_END    = 50,
        BELL_AMP       = 9000,
    };
    /* Время одной итерации SOB на БК-3МГц, в микросекундах: */
    const double CYCLE_PER_SOB_US = 3.3;

    int16_t buf[4096];
    int idx = 0;
    int16_t level = BELL_AMP;

    for (int i = 0; i < BELL_OUTER; i++) {
        int r3 = BELL_R3_START
               - (BELL_R3_START - BELL_R3_END) * i / (BELL_OUTER - 1);
        double half_period_us = r3 * CYCLE_PER_SOB_US;
        int half_samples =
            (int)(half_period_us * AUDIO_SAMPLE_RATE / 1.0e6 + 0.5);
        if (half_samples < 1) half_samples = 1;

        /* Полупериод высокого уровня (бит 6 = 1). */
        for (int s = 0; s < half_samples; s++) {
            if (idx >= (int)(sizeof buf / sizeof buf[0])) goto done;
            buf[idx++] = level;
        }
        level = -level;
        /* Полупериод низкого уровня (бит 6 = 0). */
        for (int s = 0; s < half_samples; s++) {
            if (idx >= (int)(sizeof buf / sizeof buf[0])) goto done;
            buf[idx++] = level;
        }
        level = -level;
    }
done:
    SDL_QueueAudio(g_dev, buf, (Uint32)(idx * sizeof(int16_t)));
}

void audio_flush(void)
{
    audio_log_tick();
    if (!g_dev || g_buf_pos == 0) return;
    /* Защита от длительного отставания: если в очереди > 0.5 сек звука,
     * сбросим лишнее, чтобы не было «эха»/задержки при паузе.  */
    Uint32 queued = SDL_GetQueuedAudioSize(g_dev);
    if (queued > (Uint32)(AUDIO_SAMPLE_RATE * sizeof(int16_t) / 2)) {
        SDL_ClearQueuedAudio(g_dev);
    }
    SDL_QueueAudio(g_dev, g_buf, (Uint32)(g_buf_pos * sizeof(int16_t)));
    g_buf_pos = 0;
}
