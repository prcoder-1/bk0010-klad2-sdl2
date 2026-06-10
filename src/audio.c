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

void audio_speaker_set(int level)
{
    g_level = level ? AUDIO_AMPLITUDE : -AUDIO_AMPLITUDE;
}

void audio_step(void)
{
    g_acc += g_samples_per_step;
    if (g_acc >= 1.0 && g_buf_pos < AUDIO_FRAME_BUF) {
        g_buf[g_buf_pos++] = g_level;
        g_acc -= 1.0;
    }
}

void audio_flush(void)
{
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
