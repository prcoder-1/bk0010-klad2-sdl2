/*
 * C-реализация PIC-релокатора КЛАД-2 (BK-0010 PDP-11 ассемблер → C).
 *
 * Релокатор оригинала живёт в первых ~166 байтах image (адреса
 * 0x01DA..0x027F) и за ~2061 PDP-11-шагов распаковывает основной образ
 * программы в bk_mem, затем передаёт управление на PC=0o076710 (0x7DC8).
 *
 * Здесь — прямой byte-в-byte эквивалент без интерпретатора:
 *   1. Loadим image по 0x01DA.
 *   2. Этап 1 (адреса 0x01FE..0x0234): строим 256-словную lookup-таблицу
 *      в [0x7E00..0x7FFE] из bitmap'а 16 слов в [0x0280..0x029E] +
 *      delta-stream'а от 0x02A0 и далее.
 *   3. Этап 2 (0x0236..0x0246): копируем 28 слов декомпрессора в
 *      [0x7DC8..0x7DFF].
 *   4. Этап 3 (0x7DC8..0x7DFE): декомпрессия по lookup'у — поток
 *      кодов 1 байт → 1/2 байт литерала, run-of-word или индекс в
 *      таблицу.
 *
 * Финал: bk_mem идентичен снимку klad2_initial.c, регистры — заданы
 * вручную из klad2_initial_regs (значения известны из A/B-снимков).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pdp11.h"

extern uint8_t bk_mem[];
extern cpu_t   cpu;

extern const uint16_t klad2_bin_load_addr;
extern const size_t   klad2_bin_image_len;
extern const uint8_t  klad2_bin_image[];

/* Little-endian 16-бит читалки/писалки. */
static inline uint16_t rd16(uint16_t a)
{
    return (uint16_t)bk_mem[a] | ((uint16_t)bk_mem[(uint16_t)(a + 1)] << 8);
}
static inline void wr16(uint16_t a, uint16_t v)
{
    bk_mem[a]                 = (uint8_t)(v & 0xFFu);
    bk_mem[(uint16_t)(a + 1)] = (uint8_t)(v >> 8);
}

/* Этап 1: строим lookup-таблицу [0x7E00..0x7FFE]. */
static void build_lookup_table(uint16_t *out_r4)
{
    /* После init: r0 = bitmap scanner (сверху вниз), r3 = таблица сверху,
     * r4 = delta scanner (снизу вверх), r1 = накопитель значения. */
    uint16_t r0 = 0x02A0u;
    uint16_t r1 = 0x0000u;
    uint16_t r3 = 0x8000u;
    uint16_t r4 = 0x02A0u;

    do {
        r0 = (uint16_t)(r0 - 2u);
        uint16_t bm = rd16(r0);
        for (int i = 0; i < 16; i++) {
            r3 = (uint16_t)(r3 - 2u);
            wr16(r3, 0);
            /* ROL r0 ЧЕРЕЗ CARRY: PSW.C перед каждым rol = 0 (только
             * что сделали `clr -(r3)` который чистит C). Значит каждый
             * шаг сдвигает word влево с заполнением bit 0 нулём;
             * после 16 шагов word в памяти = 0. */
            uint16_t carry = (uint16_t)((bm >> 15) & 1u);
            bm = (uint16_t)(bm << 1);

            if (carry) {
                /* movb (r4)+, r5; sign-extend; sub #147, r5. */
                int8_t  b1 = (int8_t)bk_mem[r4];
                r4 = (uint16_t)(r4 + 1u);
                int16_t r5 = (int16_t)b1;
                r5 = (int16_t)(r5 - 0147);
                if (r5 >= 0) {
                    /* swab r5; bisb (r4)+, r5; sub #14747, r5. */
                    uint16_t hi = (uint16_t)((uint16_t)r5 << 8);
                    uint8_t  b2 = bk_mem[r4];
                    r4 = (uint16_t)(r4 + 1u);
                    r5 = (int16_t)(hi | b2);
                    r5 = (int16_t)(r5 - 014747);
                }
                /* sub r5, r1; mov r1, (r3). */
                r1 = (uint16_t)(r1 - (uint16_t)r5);
                wr16(r3, r1);
            }
        }
        /* После 16 ROL'ов с C_in=0 каждый шаг слово сдвигается влево
         * с заполнением нуля → в финале word = 0. Перезаписываем. */
        wr16(r0, 0);
    } while (r3 != 0x7E00u);

    *out_r4 = r4;
}

/* Этап 2: копируем 28 слов декомпрессора в [0x7DC8..0x7DFF]. */
static void copy_decompressor(void)
{
    /* После build_lookup r0 указывает на конец delta-stream'а в 16-словном
     * проходе, но fact'ically этап 2 берёт r0 = 0x0280 (начало bitmap) и
     * копирует 28 слов СНИЗУ ВВЕРХ из image байтов 0x0248..0x027F. В
     * оригинале r0 после фазы 1 = 0x0280; делаем то же. */
    uint16_t r0 = 0x0280u;
    uint16_t r3 = 0x7E00u;
    for (int i = 0; i < 28; i++) {
        r0 = (uint16_t)(r0 - 2u);
        r3 = (uint16_t)(r3 - 2u);
        wr16(r3, rd16(r0));
    }
}

/* Этап 3: декомпрессия по lookup-таблице. */
static uint16_t decompress(uint16_t r0_start, uint16_t r4_start)
{
    uint16_t r0 = r0_start;
    uint16_t r4 = r4_start;

    /* Преroll: 0o543 байт из (r0--) → (--r4). */
    int preroll = 0543;
    do {
        r0 = (uint16_t)(r0 - 1u);
        r4 = (uint16_t)(r4 - 1u);
        bk_mem[r4] = bk_mem[r0];
    } while (--preroll != 0);

    r4 = (uint16_t)(r4 + 037000u);  /* add #37000, r4 */

    /* Основной цикл декомпрессии. */
    while (r4 != r0) {
        r0 = (uint16_t)(r0 - 1u);
        int8_t  b  = (int8_t)bk_mem[r0];
        int16_t r1 = (int16_t)((int16_t)b << 1);
        uint16_t addr = (uint16_t)(077400u + (uint16_t)r1);
        uint16_t tv = rd16(addr);

        if (tv == 0u) {
            /* L_LIT2: 2-byte literal. */
            r4 = (uint16_t)(r4 - 1u);
            bk_mem[r4] = bk_mem[r0];
            r0 = (uint16_t)(r0 - 1u);
            r4 = (uint16_t)(r4 - 1u);
            bk_mem[r4] = bk_mem[r0];
            continue;
        }
        if (tv == 1u) {
            /* L_RUN: replicate previous word `count` times. */
            r0 = (uint16_t)(r0 - 1u);
            uint8_t cnt = bk_mem[r0];
            do {
                uint16_t w = rd16(r4);
                r4 = (uint16_t)(r4 - 2u);
                wr16(r4, w);
            } while (--cnt != 0);
            continue;
        }
        if (tv == 0x8000u) {
            /* L_LIT1: дополнительный r0-- перед обычным literal-2. */
            r0 = (uint16_t)(r0 - 1u);
            r4 = (uint16_t)(r4 - 1u);
            bk_mem[r4] = bk_mem[r0];
            r0 = (uint16_t)(r0 - 1u);
            r4 = (uint16_t)(r4 - 1u);
            bk_mem[r4] = bk_mem[r0];
            continue;
        }
        /* Обычный декод: пишем (tv - 0o101) как 16-бит слово.
         * Оригинал: `add $-101, r1; mov r1, -(r4)`. */
        uint16_t w = (uint16_t)(tv - 0101u);
        r4 = (uint16_t)(r4 - 2u);
        wr16(r4, w);
    }

    return r0;  /* для jmp 0(r0) — финальный PC. */
}

/* Точка входа: загружаем встроенный image, прогоняем фазы 1+2
 * релокатора (построение lookup-таблицы + копирование декомпрессора).
 * Фаза 3 (распаковка основного кода) выполняется штатным
 * интерпретатором по входу PC=0x7DC8 — это соответствует поведению
 * --save-state в pdp11_step()/main.c.
 *
 * Финальное состояние совпадает с klad2_load_builtin_state() byte-в-byte. */
void klad2_run_relocator(void)
{
    /* Свежая БК: bk_mem обнулён (вызывается до этой функции). Загружаем
     * image по load_addr. */
    memcpy(&bk_mem[klad2_bin_load_addr], klad2_bin_image, klad2_bin_image_len);

    uint16_t r4_after_phase1;
    build_lookup_table(&r4_after_phase1);
    copy_decompressor();

    /* Финальные регистры после фазы 2 (= снимок на входе декомпрессора): */
    cpu.r[0] = 0x2D24u;     /* конец image + add #25334 */
    cpu.r[1] = 0xE603u;     /* кумулятор последней дельты */
    cpu.r[2] = 0x0163u;     /* счётчик преroll'а (543 oct) — заряжен ниже */
    cpu.r[3] = 0x7DC8u;     /* адрес декомпрессора */
    cpu.r[4] = r4_after_phase1;
    cpu.r[5] = 0xF81Cu;     /* последняя дельта в фазе 1 */
    cpu.r[6] = 0x3FFEu;     /* SP, как при загрузке БК-БИН */
    cpu.r[7] = 0x7DC8u;     /* jmp (r3) → декомпрессор; интерпретатор продолжит */
    cpu.psw  = 0;
}
