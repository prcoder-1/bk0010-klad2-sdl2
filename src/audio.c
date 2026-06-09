/*
 * Звуковой бэкенд — заглушка Фазы 1.
 *
 * Сюда приходят переключения бита EXT_DEV_MAG_KEY из bk.c. В этой фазе
 * мы их молча проглатываем (запоминаем последнее значение для отладки).
 * Реальный SDL_AudioSpec / очередь PCM-генерации появится в Фазе 8.
 */

#include "audio.h"

static int g_speaker_level;

int audio_init(void)
{
    g_speaker_level = 0;
    return 0;
}

void audio_shutdown(void) {}

void audio_speaker_set(int level)
{
    g_speaker_level = level ? 1 : 0;
}
