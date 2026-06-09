/*
 * Звуковой бэкенд: получает «уровень динамика» (0/1) от записи в бит
 * EXT_DEV_MAG_KEY регистра REG_EXT_DEV и должен переводить это в PCM-
 * вывод через SDL2.
 *
 * В Фазе 1 — пустая заглушка, чтобы запись в регистр не падала. Реальный
 * генератор меандра/очередь запросов появятся в Фазе 8.
 */

#ifndef KLAD2_AUDIO_H
#define KLAD2_AUDIO_H

int  audio_init(void);
void audio_shutdown(void);

/* level: 0 — динамик «отпущен», 1 — «нажат». В реальной БК меняется
 * с частотой выходного тона. */
void audio_speaker_set(int level);

#endif /* KLAD2_AUDIO_H */
