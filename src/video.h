/*
 * Видео-бэкенд: каждый кадр конвертирует содержимое видеоОЗУ БК
 * (bk_mem[MEM_VIDEO..MEM_VIDEO+0x4000]) в RGBA-фрейм и выводит его
 * через SDL_Renderer.
 *
 * Фаза 1 — цветной режим 256×256, палитра БК по умолчанию
 * (чёрный/синий/зелёный/красный). Расширенная память и
 * чёрно-белый 512×256 — позже.
 */

#ifndef KLAD2_VIDEO_H
#define KLAD2_VIDEO_H

#include <SDL2/SDL.h>

int  video_init(SDL_Renderer *renderer);
void video_shutdown(void);

/* Вывести один кадр: пересборка текстуры из bk_mem + SDL_RenderCopy.
 * Сам SDL_RenderPresent — забота вызывающего. */
void video_render_frame(SDL_Renderer *renderer);

#endif /* KLAD2_VIDEO_H */
