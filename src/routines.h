/*
 * Диспетчер C-замен для подпрограмм КЛАД-2 (Фаза 6 плана).
 *
 * Идея: оригинальный KLAD2.BIN продолжает крутиться через интерпретатор
 * PDP-11, но если очередной PC попал в адрес, для которого мы уже
 * написали ручную реализацию на Си, интерпретатор не декодирует
 * инструкции, а сразу вызывает C-функцию. Функция:
 *   - читает входные параметры из cpu.r[] и bk_mem (по соглашению
 *     gcc-pdp11: R0..R5 — параметры/возврат, R6 — SP, R7 — PC);
 *   - выполняет эквивалентную работу над bk_mem;
 *   - эмулирует RTS: PC = pop(), SP += 2.
 *
 * Соглашение: имя функции — `R_<oct-addr>_<смысл>`. oct-addr — это
 * адрес процедуры в дампе после релокации (asm/klad2_unpacked.dis),
 * в восьмеричной форме без ведущего нуля.
 *
 * Адреса берутся из дампа `asm/klad2_unpacked.bin` (снят на 50000-й
 * инструкции, когда PIC-релокатор КЛАД-2 уже разложил код по
 * фиксированным местам). Те же адреса валидны в реальном runtime,
 * потому что релокатор детерминирован.
 *
 * Регистрация — статическая таблица в `routines.c`.
 */

#ifndef KLAD2_ROUTINES_H
#define KLAD2_ROUTINES_H

#include <stdint.h>

typedef void (*routine_fn)(void);

/* Возвращает C-замену для адреса pc или NULL, если её нет. */
routine_fn routines_lookup(uint16_t pc);

/* Если 1 — routines_lookup всегда возвращает NULL (диспетчер C-замен
 * отключён). Используется AB-инфраструктурой, чтобы во время
 * интерпретированной фазы НЕ вызвать C-замену повторно: иначе AB
 * сравнивает C с самой собой, а не с интерпретатором. */
extern int routines_runtime_disabled;

/* Прямой доступ для A/B-режима (когда замена не зарегистрирована в
 * g_routines[], но мы хотим запустить её для сравнения с интерпрета-
 * тором). По мере декомпиляции — добавляем декларации сюда. */
void R_14030_draw_compound_sprite(void);
void R_07202_anim_tick_e2_move(void);
void R_07242_anim_tick_e2_idle(void);
void R_10102_anim_tick_e1_move(void);
void R_10142_anim_tick_e1_idle(void);
void R_06632_main_e2(void);
void R_06462_e2_teleport(void);
void R_07306_hero_teleport(void);
void R_07462_main_e1(void);
void R_14d6_bel(void);
void R_15ce_clear_cell(void);
void R_01720_bel_dec_lives(void);
void R_07f4_lose_one(void);
void R_0262_check_key1(void);
void R_0f1a_anim_timer3(void);
void R_1558_draw_8rows(void);
void R_0a46_draw_8rows_v2(void);
void R_1522_draw_map(void);
void R_0888_int_to_dec(void);
void R_07aa_print_score(void);
void R_0c2c_draw_hero(void);

#endif /* KLAD2_ROUTINES_H */
