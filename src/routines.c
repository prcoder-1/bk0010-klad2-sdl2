/*
 * Декомпилированные подпрограммы КЛАД-2.
 *
 * Каждая функция — ручная Си-реализация подпрограммы из KLAD2.BIN с
 * **точно той же** семантикой по отношению к bk_mem и cpu.r[].
 *
 * Соглашение по вызову — JSR PC, target / RTS PC: вход через JSR кладёт
 * на стек PC возврата, RTS его читает обратно. Сохранённые регистры
 * R1/R3/R4/R5 push/pop'аются внутри процедуры и НЕ меняются с точки
 * зрения вызывающего; поэтому в C-замене мы их просто не трогаем.
 *
 * Каждая замена в конце эмулирует RTS PC:
 *     cpu.r[7] = bk_read_w(cpu.r[6]);
 *     cpu.r[6] += 2;
 */

#include "routines.h"
#include "audio.h"
#include "bk.h"
#include "emt.h"
#include "pdp11.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Эмулирует «RTS PC» — снимает адрес возврата со стека. */
static inline void rts_pc(void)
{
    cpu.r[7] = bk_read_w(cpu.r[6]);
    cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
}

/* ---------------------------------------------------------------------
 * R_14302_draw_glyph_8x8 — рисует 8×8-пиксельный глиф шрифта в видео-
 * память (база 0o46000), MOV без XOR (затирает фон).
 *
 * Оригинал (0o14302..0o14320, итого 15 байт):
 *   mov   r5, -(sp)         ; save r5
 *   mov   r1, -(sp)         ; save r1
 *   movb  (r2), r3          ; r3 = байт-символ
 *   bic   #-400, r3         ; r3 &= 0xFF
 *   asl   r3 x4             ; r3 <<= 4  (×16 байт на глиф)
 *   add   #017450, r3       ; r3 += база шрифта КЛАД-2
 *   mov   #10, r5           ; 8 строк
 * loop:
 *   mov   (r3)+, 046000(r1) ; экранное слово = строка глифа
 *   add   #100, r1          ; следующая строка экрана (64 байта)
 *   sob   r5, loop
 *   mov   (sp)+, r1
 *   mov   (sp)+, r5
 *   rts   pc
 *
 * Вход:  r1 — байтовая X-позиция (0..63) в экранной строке (база — 0o46000),
 *        r2 — указатель на байт-символ.
 * Выход: r1, r2, r3, r5 — без изменений с точки зрения вызывающего.
 *        bk_mem[0o46000 + r1 + k*64] := font[(byte<<4)+2k..+2k+1], k=0..7.
 *
 * Шрифт КЛАД-2 хранится в bk_mem по 0o17450, 16 байт/глиф; каждое слово
 * шрифта = 8 пикселей в цветном режиме (4 пикс/байт × 2 байта). Поэтому
 * глиф — 8 пикселей в ширину, 8 пикселей в высоту.
 * ------------------------------------------------------------------- */
enum {
    KLAD2_FONT_BASE    = 0017450u,
    KLAD2_GLYPH_STRIDE = 16u,
    KLAD2_GLYPH_ROWS   = 8u,
    SCREEN_DRAW_BASE   = 0046000u,
    SCREEN_ROW_BYTES   = 0100u,
};

static void R_14302_draw_glyph_8x8(void)
{
    uint8_t  ch  = bk_read_b(cpu.r[2]);
    uint16_t src = (uint16_t)(KLAD2_FONT_BASE + (unsigned)ch * KLAD2_GLYPH_STRIDE);
    uint16_t x   = cpu.r[1];

    for (unsigned i = 0; i < KLAD2_GLYPH_ROWS; i++) {
        uint16_t w = bk_read_w(src);
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + x), w);
        src = (uint16_t)(src + 2u);
        x   = (uint16_t)(x + SCREEN_ROW_BYTES);
    }

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_14354_xor_sprite_8rows — XOR-блиттер: 8 строк × 1 слово шириной.
 *
 * Оригинал (0o14354..0o14416, итого 35 байт):
 *   mov   r5, -(sp)
 *   mov   r4, -(sp)
 *   mov   r3, -(sp)
 *   mov   r1, -(sp)
 *   mov   #10, r5            ; 8 строк
 *   mov   (r2), r3           ; r3 = *r2 (указатель на массив слов спрайта)
 * loop:
 *   mov   (r3)+, r4          ; r4 = слово спрайта, r3 += 2
 *   xor   r4, 046000(r1)     ; экранное слово ^= r4
 *   add   #100, r1
 *   sob   r5, loop
 *   pop r1,r3,r4,r5
 *   rts pc
 *
 * Вход:  r1 — байтовая X-позиция в видеообласти (база 0o46000),
 *        r2 — указатель на указатель на массив из 8 слов (спрайт).
 *        (*r2 → ptr → 16 байт данных)
 * Выход: r1..r5 — без изменений.
 *        bk_mem[0o46000 + r1 + k*64] ^= *((*r2) + k*2), k=0..7.
 *
 * Замечание о (r2): MOV (r2),r3 загружает r3 один раз — он не в цикле
 * (метка цикла — на следующей инструкции). Поэтому ОДИН разыменователь
 * для всех 8 итераций.
 * ------------------------------------------------------------------- */
static void R_14354_xor_sprite_8rows(void)
{
    uint16_t sp_ptr = bk_read_w(cpu.r[2]);   /* *r2 — адрес массива слов */
    uint16_t x      = cpu.r[1];

    for (unsigned i = 0; i < KLAD2_GLYPH_ROWS; i++) {
        uint16_t w   = bk_read_w(sp_ptr);
        uint16_t scr = bk_read_w((uint16_t)(SCREEN_DRAW_BASE + x));
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + x), (uint16_t)(scr ^ w));
        sp_ptr = (uint16_t)(sp_ptr + 2u);
        x      = (uint16_t)(x + SCREEN_ROW_BYTES);
    }

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_14030_draw_compound_sprite — рисователь «составного спрайта»:
 * два glyph + два xor-sprite, с обновлением анимационного состояния.
 *
 * Это родительская процедура для R_14302 и R_14354 — она их вызывает
 * по два раза. Адрес 0o14030 = 0x1818.
 *
 * Оригинал (0o14030..0o14302, 84 байта). Полный листинг:
 *   mov   r2, -(sp)         ; push r2
 *   mov   2(r4), r2         ; r2 = char-ptr (массив байт-символов)
 *   mov   6(r4), r1         ; r1 = X
 *   jsr   pc, R_14302       ; нарисовать первый glyph
 *   mov   (r4), r3          ; r3 = старый код кадра
 *   asl r3 x2               ; r3 <<= 2
 *   add   #020270, r3       ; r3 = таблица смещений + старый_код*4
 *   add   2(r3), r2         ; смещение к второму символу
 *   add   4(r3), r1         ; смещение к второй X
 *   cmp   #024, 4(r4)       ; фаза > 0o24?
 *   ble   0x1850            ; нет — рисуем смещённым
 *   sub   2(r3), r2 x2      ; да — зеркалим (вычитаем 2*dchar)
 *   sub   4(r3), r1 x2      ;        зеркалим (вычитаем 2*dx)
 * label_1850:
 *   jsr   pc, R_14302       ; нарисовать второй glyph
 *   mov   (sp), r2          ; восстановить локальный r2 (peek, не pop)
 *   mov   r0, r3            ; новый код кадра
 *   asl r3 x2
 *   add   #020270, r3       ; r3 = таблица + новый_код*4
 *   cmp   r0, (r4)          ; новый == старый?
 *   beq   label_186c
 *   mov   #024, 4(r4)       ; смена кода: фаза = 0o24 (вторая половина)
 *   br    label_1892
 * label_186c:
 *   cmp   #024, 4(r4)       ; фаза < 0o24?
 *   bge   label_188c        ; нет — следующая полу-фаза
 *   bit   (r3), *2(r4)      ; *(char_ptr) AND mask_word(new_code) — есть пересечение?
 *   beq   label_1892        ; нет — ничего не меняем
 *   clr   4(r4)             ; есть — сброс фазы
 *   add   2(r3), 2(r4)      ; char_ptr += dchar
 *   add   4(r3), 6(r4)      ; X += dx
 *   br    label_1892
 * label_188c:
 *   add   #012, 4(r4)       ; фаза += 0o12 (10), продолжаем "вторую половину"
 * label_1892:
 *   mov   r0, (r4)          ; запомнить новый код
 *   add   (r4), r2          ; r2 += новый_код (как слово!)
 *   add   4(r4), r2         ; r2 += фаза  → теперь r2 = data + new_code + phase
 *   mov   6(r4), r1         ; r1 = X
 *   jsr   pc, R_14354       ; XOR первой маски
 *   add   4(r3), r1         ; следующая X
 *   cmp   #024, 4(r4)
 *   ble   skip
 *   sub   4(r3), r1 x2      ; зеркалим
 * skip:
 *   add   #050, r2          ; смещение к второй маске (40 байт)
 *   jsr   pc, R_14354       ; XOR второй маски
 *   mov   (sp)+, r2         ; pop
 *   rts   pc
 *
 * Структура (r4):
 *   +0  — старый код кадра (word)
 *   +2  — указатель на массив байт-символов (word — указатель в bk_mem)
 *   +4  — фаза анимации (word, 0..N, граница 0o24=20)
 *   +6  — X-позиция в видеообласти (word, байтовая)
 *
 * Входной r0 = новый код кадра.
 *
 * Таблица 0o20270: каждый «код кадра» имеет 4-байтную запись:
 *   +0  — маска для теста столкновения (word)
 *   +2  — смещение по символу-указателю dchar (word, signed)
 *   +4  — смещение X на экране dx (word, signed)
 * Примечание: с разрешения проверки кода, +0 здесь — это просто «не
 * используется в этой процедуре, только +2 и +4». Маска (r3) тестируется
 * против слова *(char_ptr).
 *
 * Поскольку процедура вызывает R_14302 и R_14354 через JSR, C-замена
 * **не может** их вызвать напрямую (адреса диспетчеризации завязаны на
 * PC). Поэтому мы инлайним их семантику прямо в эту C-функцию.
 * ------------------------------------------------------------------- */

/* Инлайн draw_glyph_8x8 — те же действия что R_14302 без RTS. */
static void inline_draw_glyph(uint16_t char_ptr, uint16_t x)
{
    uint8_t  ch  = bk_read_b(char_ptr);
    uint16_t src = (uint16_t)(KLAD2_FONT_BASE + (unsigned)ch * KLAD2_GLYPH_STRIDE);
    for (unsigned i = 0; i < KLAD2_GLYPH_ROWS; i++) {
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + x), bk_read_w(src));
        src = (uint16_t)(src + 2u);
        x   = (uint16_t)(x + SCREEN_ROW_BYTES);
    }
}

/* Инлайн xor_sprite — те же действия что R_14354 без RTS. */
static void inline_xor_sprite(uint16_t sp_ptr_ptr, uint16_t x)
{
    uint16_t sp_ptr = bk_read_w(sp_ptr_ptr);
    for (unsigned i = 0; i < KLAD2_GLYPH_ROWS; i++) {
        uint16_t w   = bk_read_w(sp_ptr);
        uint16_t scr = bk_read_w((uint16_t)(SCREEN_DRAW_BASE + x));
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + x), (uint16_t)(scr ^ w));
        sp_ptr = (uint16_t)(sp_ptr + 2u);
        x      = (uint16_t)(x + SCREEN_ROW_BYTES);
    }
}

enum {
    ANIM_TABLE_BASE      = 0020270u,
    ANIM_PHASE_THRESHOLD = 0024u,    /* 20 десятичное */
    ANIM_PHASE_STEP      = 0012u,    /* 10 десятичное */
    XOR_MASK_STRIDE      = 0050u,    /* 40 байт между двумя XOR-масками */
};

/* Тело R_14030 без эмуляции RTS — удобно вызывать как inline из других
 * декомпилированных процедур (которые сами потом делают rts_pc).
 *
 * Переписано буквально по оригиналу: работаем с `cpu.r[N]` напрямую,
 * push/pop через `cpu.r[6]` и `bk_read_w/bk_write_w`. Это даёт байт-в-
 * байт совместимость с интерпретатором по регистрам и PSW.
 *
 * Финальные значения после возврата:
 *   r0 — unchanged (read-only)
 *   r1 — mask_x ± dx_new (последнее присваивание перед вторым XOR)
 *   r2 — initial r2 caller-а (восстанавливается через `mov (sp)+, r2`)
 *   r3 — ANIM_TABLE_BASE + new_code*4
 *   r4 — unchanged
 *   r5 — unchanged
 *   PSW.N/Z/V — установлены по итоговому r2 (от `mov (sp)+, r2`)
 *   PSW.C — unchanged (последняя операция, его меняющая, — где-то ранее) */
static void R_14030_body(void)
{
    uint16_t anim, dchar_old, dx_old, dchar_new, dx_new, mask_new;
    uint16_t phase;

    /* 0x1818: mov r2, -(sp) — сохраняем входной r2 на стеке. */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], cpu.r[2]);

    anim = cpu.r[4];

    /* 0x181a: mov 2(r4), r2 */
    cpu.r[2] = bk_read_w((uint16_t)(anim + 2u));
    /* 0x181e: mov 6(r4), r1 */
    cpu.r[1] = bk_read_w((uint16_t)(anim + 6u));

    /* 0x1822: jsr pc, R_14302 — первый glyph по (r2, r1). */
    inline_draw_glyph(cpu.r[2], cpu.r[1]);

    /* 0x1826-182c: r3 = ANIM_TABLE_BASE + old_code*4. */
    cpu.r[3] = (uint16_t)(ANIM_TABLE_BASE + bk_read_w(anim) * 4u);
    dchar_old = bk_read_w((uint16_t)(cpu.r[3] + 2u));
    dx_old    = bk_read_w((uint16_t)(cpu.r[3] + 4u));

    /* 0x1830-1834: r2 += dchar_old; r1 += dx_old. */
    cpu.r[2] = (uint16_t)(cpu.r[2] + dchar_old);
    cpu.r[1] = (uint16_t)(cpu.r[1] + dx_old);

    /* 0x1838-184c: если phase < 0o24 → зеркалим (вычитаем 2× dchar/dx). */
    phase = bk_read_w((uint16_t)(anim + 4u));
    if (phase < ANIM_PHASE_THRESHOLD) {
        cpu.r[2] = (uint16_t)(cpu.r[2] - 2u * dchar_old);
        cpu.r[1] = (uint16_t)(cpu.r[1] - 2u * dx_old);
    }

    /* 0x1850: jsr pc, R_14302 — второй glyph. */
    inline_draw_glyph(cpu.r[2], cpu.r[1]);

    /* 0x1854: mov (sp), r2 — peek, восстанавливаем входной r2. */
    cpu.r[2] = bk_read_w(cpu.r[6]);

    /* 0x1856-185c: r3 = ANIM_TABLE_BASE + new_code*4. */
    cpu.r[3] = (uint16_t)(ANIM_TABLE_BASE + cpu.r[0] * 4u);
    mask_new  = bk_read_w(cpu.r[3]);
    dchar_new = bk_read_w((uint16_t)(cpu.r[3] + 2u));
    dx_new    = bk_read_w((uint16_t)(cpu.r[3] + 4u));

    /* 0x1860-1892: state-машина обновления фазы. */
    if (cpu.r[0] != bk_read_w(anim)) {
        /* new != old → phase := 0o24. */
        bk_write_w((uint16_t)(anim + 4u), ANIM_PHASE_THRESHOLD);
    } else if (phase <= ANIM_PHASE_THRESHOLD) {
        /* phase <= 0o24 → phase += 0o12. */
        bk_write_w((uint16_t)(anim + 4u),
                   (uint16_t)(phase + ANIM_PHASE_STEP));
    } else {
        /* phase > 0o24 → bit-тест и шаг анимации, если есть. */
        uint16_t cur = bk_read_w(bk_read_w((uint16_t)(anim + 2u)));
        if (mask_new & cur) {
            bk_write_w((uint16_t)(anim + 4u), 0);
            bk_write_w((uint16_t)(anim + 2u),
                       (uint16_t)(bk_read_w((uint16_t)(anim + 2u)) + dchar_new));
            bk_write_w((uint16_t)(anim + 6u),
                       (uint16_t)(bk_read_w((uint16_t)(anim + 6u)) + dx_new));
        }
    }
    /* 0x1892: mov r0, (r4). */
    bk_write_w(anim, cpu.r[0]);

    /* 0x1894-1896: r2 += (r4); r2 += 4(r4). */
    cpu.r[2] = (uint16_t)(cpu.r[2] + bk_read_w(anim));
    cpu.r[2] = (uint16_t)(cpu.r[2] + bk_read_w((uint16_t)(anim + 4u)));

    /* 0x189a: mov 6(r4), r1. */
    cpu.r[1] = bk_read_w((uint16_t)(anim + 6u));

    /* 0x189e: jsr pc, R_14354 — первый XOR-спрайт. */
    inline_xor_sprite(cpu.r[2], cpu.r[1]);

    /* 0x18a2-18b2: r1 += dx_new; если phase < 0o24 → r1 -= 2*dx_new. */
    cpu.r[1] = (uint16_t)(cpu.r[1] + dx_new);
    /* «свежая» phase нужна для проверки — после возможного обновления выше. */
    phase = bk_read_w((uint16_t)(anim + 4u));
    if (phase < ANIM_PHASE_THRESHOLD) {
        cpu.r[1] = (uint16_t)(cpu.r[1] - 2u * dx_new);
    }

    /* 0x18b6: r2 += 0o50. */
    cpu.r[2] = (uint16_t)(cpu.r[2] + XOR_MASK_STRIDE);

    /* 0x18ba: jsr pc, R_14354 — второй XOR-спрайт. */
    inline_xor_sprite(cpu.r[2], cpu.r[1]);

    /* PSW.C: оригинал устанавливает её последним `add #100, r1` 8-й
     * итерации цикла внутри второго R_14354. В нашем inline_xor_sprite
     * r1 локальный, в cpu.r[1] он не записывается — поэтому эмулируем
     * переполнение явно от текущего cpu.r[1] (= initial X второго xor):
     *   r1_7 = r1 + 7*100;  r1_8 = r1_7 + 100;  C = (r1_8 < r1_7).
     * В редких случаях (r1 близок к 0xFFFF), последний add переполняется
     * → C=1; иначе → C=0. mov (sp)+, r2 PSW.C не трогает. */
    {
        uint16_t r1_prev = (uint16_t)(cpu.r[1] + 7u * SCREEN_ROW_BYTES);
        uint16_t r1_last = (uint16_t)(r1_prev  + SCREEN_ROW_BYTES);
        if (r1_last < r1_prev) cpu.psw |= PSW_C;
        else                    cpu.psw &= (uint16_t)~PSW_C;
    }

    /* 0x18be: mov (sp)+, r2 — pop. Устанавливает PSW.N/Z/V по итоговому r2. */
    cpu.r[2] = bk_read_w(cpu.r[6]);
    cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.psw  = (uint16_t)(cpu.psw & ~(PSW_N | PSW_Z | PSW_V));
    if ((int16_t)cpu.r[2] < 0) cpu.psw |= PSW_N;
    if (cpu.r[2] == 0)         cpu.psw |= PSW_Z;
}

void R_14030_draw_compound_sprite(void)
{
    R_14030_body();
    rts_pc();
}

/* ---------------------------------------------------------------------
 * Anim-таймеры героя/врагов (0o07202, 0o07242, 0o10102, 0o10142).
 *
 * Каждая — короткая обёртка над R_14030: поднимает константы (r2 =
 * xor_base, r4 = anim-структура, опционально r0 = 8 — «idle»-направление),
 * проверяет внутренний счётчик в системной памяти и либо инкрементирует
 * его, либо вызывает R_14030 и сбрасывает счётчик. Это вычурный
 * «делитель частоты» для анимации: фактическое движение героя/врага
 * случается только на каждом (threshold+2)-м кадре.
 *
 * Оригинал 0o07202 (16 байт):
 *   mov   #021760, r2          ; xor_base
 *   mov   #014440, r4          ; anim struct
 *   cmpb  #7, @#017366         ; signed cmp 7 c counter byte
 *   bge   incr                 ; берётся если 7 >= counter, т.е. counter <= 7
 *   jsr   pc, @#014030         ; иначе — R_14030
 *   clrb  @#017366             ;        counter = 0
 *   br    out
 * incr:
 *   incb  @#017366
 * out:
 *   rts   pc
 *
 * Остальные три — изоморфные, отличаются 5 константами:
 *   xor_base, anim-структурой, адресом и шириной (byte/word) счётчика,
 *   threshold, и есть ли pre-установка r0 = 8 (idle-режим).
 * ------------------------------------------------------------------- */
static void anim_tick(uint16_t xor_base, uint16_t anim,
                      uint16_t counter_addr, int byte_op, int16_t threshold)
{
    /* mov #xor_base, r2 — N=msb, Z=(==0), V=0, C unchanged. */
    cpu.r[2] = xor_base;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if (xor_base & 0x8000u) cpu.psw |= PSW_N;
    if (xor_base == 0)      cpu.psw |= PSW_Z;

    /* mov #anim, r4 — те же эффекты. */
    cpu.r[4] = anim;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if (anim & 0x8000u) cpu.psw |= PSW_N;
    if (anim == 0)      cpu.psw |= PSW_Z;

    /* cmpb/cmp #threshold, @#counter_addr — устанавливает PSW по
     * результату вычитания threshold - cnt. */
    int16_t cnt = byte_op
        ? (int16_t)(int8_t)bk_read_b(counter_addr)
        : (int16_t)bk_read_w(counter_addr);
    {
        int16_t res = (int16_t)(threshold - cnt);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if (res < 0)         cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (byte_op) {
            if ((uint16_t)((threshold ^ cnt) & (threshold ^ res) & 0x80u))
                cpu.psw |= PSW_V;
            if ((uint16_t)(uint8_t)threshold < (uint16_t)(uint8_t)cnt)
                cpu.psw |= PSW_C;
        } else {
            if ((uint16_t)((threshold ^ cnt) & (threshold ^ res) & 0x8000u))
                cpu.psw |= PSW_V;
            if ((uint16_t)threshold < (uint16_t)cnt)
                cpu.psw |= PSW_C;
        }
    }

    /* bge берётся при threshold >= cnt (т.е. PSW.N == PSW.V). */
    if (threshold >= cnt) {
        /* incb/inc @#counter_addr — N=msb, Z=(==0), V=overflow, C unchanged. */
        int16_t nxt = (int16_t)(cnt + 1);
        if (byte_op) {
            uint8_t r = (uint8_t)nxt;
            bk_write_b(counter_addr, r);
            cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
            if (r & 0x80u) cpu.psw |= PSW_N;
            if (r == 0)    cpu.psw |= PSW_Z;
            if ((uint8_t)cnt == 0x7Fu) cpu.psw |= PSW_V;
        } else {
            uint16_t r = (uint16_t)nxt;
            bk_write_w(counter_addr, r);
            cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
            if (r & 0x8000u) cpu.psw |= PSW_N;
            if (r == 0)      cpu.psw |= PSW_Z;
            if ((uint16_t)cnt == 0x7FFFu) cpu.psw |= PSW_V;
        }
    } else {
        /* jsr pc, R_14030 + clrb/clr.
         * R_14030_body уже корректно эмулирует все регистры и PSW
         * (N/Z/V/C по итоговому r2 + сброс C). После него идёт
         * clrb/clr — это финальная инструкция, она и определяет PSW. */
        R_14030_body();
        if (byte_op) bk_write_b(counter_addr, 0);
        else         bk_write_w(counter_addr, 0);
        /* PSW по clr: N=0, Z=1, V=0, C=0. */
        cpu.psw &= (uint16_t)~(PSW_N | PSW_V | PSW_C);
        cpu.psw |= PSW_Z;
    }
    /* `br out` и `rts pc` не трогают PSW. */
}

/* 0o07202 (0x0e82): враг #2 в движении, byte-счётчик, threshold=7,
 * r0 приходит снаружи (текущее направление). */
void R_07202_anim_tick_e2_move(void)
{
    anim_tick(0021760u, 0014440u, 0017366u, 1, 7);
    rts_pc();
}

/* 0o07242 (0x0ea2): враг #2 в покое, byte-счётчик, threshold=4,
 * r0 жёстко = 8 (idle-направление). */
void R_07242_anim_tick_e2_idle(void)
{
    /* mov #10, r0 — устанавливает PSW.N/Z/V (0o10 = 8 > 0, msb=0). */
    cpu.r[0] = 0010u;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    /* msb(0o10) = 0 → N=0, 0o10 != 0 → Z=0. */
    anim_tick(0021760u, 0014440u, 0017370u, 1, 4);
    rts_pc();
}

/* 0o10102 (0x1042): враг #1 в движении, WORD-счётчик, threshold=7. */
void R_10102_anim_tick_e1_move(void)
{
    anim_tick(0021760u, 0014430u, 0017362u, 0, 7);
    rts_pc();
}

/* 0o10142 (0x1062): враг #1 в покое, WORD-счётчик, threshold=4, r0=8. */
void R_10142_anim_tick_e1_idle(void)
{
    cpu.r[0] = 0010u;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    anim_tick(0021760u, 0014430u, 0017364u, 0, 4);
    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_06602_enemy_e2_handler — обработчик ИИ врага #2 (0x0d82..0x0e80,
 * 254 байта, ~100 инструкций). Точка входа из главного цикла.
 *
 * Алгоритм:
 *   - Если код кадра врага == 8 («стоит») → переход в idle-секцию.
 *   - Иначе вычисляются X-координаты врага и героя:
 *       (char_ptr - 0o14550) & 0o77 — индекс внутри строки видеообласти,
 *       0o14550 — база карты (середина экрана 0o100 × 0o57 = 0o14400+).
 *   - Сравнение X:
 *       e2.x  < hero.x → пробуем идти вправо (направление r0=0);
 *       e2.x  > hero.x → пробуем идти влево (r0=2);
 *       e2.x == hero.x → переходим в вертикальную ветку.
 *   - В каждой попытке движения проверяем 3 препятствия:
 *       (1) bit в текущей клетке (битмаска направления движения);
 *       (2) для горизонтальных — cmpb #11, 2(r1)/-2(r1) (тип
 *           соседней клетки == 11 — лестница, нельзя пройти);
 *       (3) следующая клетка == HERO_TARGET_PTR (там стоит герой
 *           — стоп, чтобы не наложиться).
 *   - При неудаче — fallthrough в вертикальную или idle ветку.
 *   - После jsr R_07202 (move) проверяется bit 0o4000 в новой клетке
 *     (стоит ли в углу/яме) — если да, выход. Иначе проверка коллизии
 *     с героем под собой → если нет — jsr R_07242 (idle).
 *
 * Bit-маски направлений в клетке карты:
 *   0o100000 — можно вправо
 *   0o040000 — можно влево
 *   0o020000 — можно вверх
 *   0o010000 — можно вниз
 *   0o004000 — клетка «удержание» (после движения, выход без idle)
 *   0o002000 — клетка-ловушка (в idle-ветке: если попал — set code 6)
 *
 * Структура anim:
 *   anim_e2  = 0o14440: +0 code, +2 char_ptr, +4 phase, +6 X
 *   anim_h+2 = 0o14422: char_ptr героя
 *   HERO_TARGET_PTR @0o14432 — позиция выстрела/прицел героя.
 * ------------------------------------------------------------------- */
enum {
    ANIM_E2          = 0014440u,   /* code */
    ANIM_E2_CHAR_PTR = 0014442u,   /* +2 от ANIM_E2 */
    ANIM_HERO_CP_PTR = 0014422u,   /* char_ptr героя — указатель на байт в карте */
    HERO_TARGET_PTR  = 0014432u,   /* позиция «прицела/тени» героя */
    MAP_BASE         = 0014550u,
};

/* ---------------------------------------------------------------------
 * Общий хелпер «телепорт объекта» (исп. R_06462 и R_07306).
 *
 * Изоморфен оригиналам — отличаются 5 константами:
 *   cp_addr       — адрес ячейки `char_ptr` объекта
 *   x_addr        — адрес ячейки `X` объекта
 *   table_dch_off — смещение в таблице @#17430 для нового char_ptr
 *   table_dx_off  — смещение в таблице @#17430 для нового X
 *   timer_addr    — адрес таймера, сбрасываемого в финале
 *
 * Семантика: если byte по char_ptr равен 0o15 (13 — тайл «дверь»),
 * стираем спрайт объекта (2 glyph по текущим и смещённым на (-100,
 * -1000) координатам), загружаем новые координаты из таблицы @#17430
 * и сбрасываем таймер. Иначе ранний RTS.
 * ------------------------------------------------------------------- */
static void teleport_object(uint16_t cp_addr, uint16_t x_addr,
                            uint16_t table_dch_off, uint16_t table_dx_off,
                            uint16_t timer_addr)
{
    cpu.r[0] = bk_read_w(cp_addr);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if ((int16_t)cpu.r[0] < 0) cpu.psw |= PSW_N;
    if (cpu.r[0] == 0)         cpu.psw |= PSW_Z;

    uint8_t b = bk_read_b(cpu.r[0]);
    {
        uint8_t res = (uint8_t)(0015u - b);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int8_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if ((uint8_t)((0015u ^ b) & (0015u ^ res) & 0x80u)) cpu.psw |= PSW_V;
        if (0015u < b) cpu.psw |= PSW_C;
    }
    if (b != 0015u) { rts_pc(); return; }

    cpu.r[2] = bk_read_w(cp_addr);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if ((int16_t)cpu.r[2] < 0) cpu.psw |= PSW_N;
    if (cpu.r[2] == 0)         cpu.psw |= PSW_Z;

    cpu.r[1] = bk_read_w(x_addr);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if ((int16_t)cpu.r[1] < 0) cpu.psw |= PSW_N;
    if (cpu.r[1] == 0)         cpu.psw |= PSW_Z;

    /* jsr pc, R_14302 — первый glyph (push/pop r5/r1, эмуляция r3, PSW.C). */
    {
        uint8_t ch = bk_read_b(cpu.r[2]);
        cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[5]);
        cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[1]);
        inline_draw_glyph(cpu.r[2], cpu.r[1]);
        cpu.r[3] = (uint16_t)(KLAD2_FONT_BASE + (unsigned)ch * 16u + 16u);
        uint16_t r1_prev = (uint16_t)(cpu.r[1] + 7u * SCREEN_ROW_BYTES);
        uint16_t r1_last = (uint16_t)(r1_prev + SCREEN_ROW_BYTES);
        if (r1_last < r1_prev) cpu.psw |= PSW_C;
        else                    cpu.psw &= (uint16_t)~PSW_C;
        cpu.r[1] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
        cpu.r[5] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)cpu.r[5] < 0) cpu.psw |= PSW_N;
        if (cpu.r[5] == 0)         cpu.psw |= PSW_Z;
    }

    /* sub #100, r2. */
    {
        uint16_t a = cpu.r[2], src = 0100u;
        uint16_t r = (uint16_t)(a - src);
        cpu.r[2] = r;
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int16_t)r < 0) cpu.psw |= PSW_N;
        if (r == 0)          cpu.psw |= PSW_Z;
        if ((a ^ src) & (a ^ r) & 0x8000u) cpu.psw |= PSW_V;
        if (a < src) cpu.psw |= PSW_C;
    }
    /* sub #1000, r1. */
    {
        uint16_t a = cpu.r[1], src = 01000u;
        uint16_t r = (uint16_t)(a - src);
        cpu.r[1] = r;
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int16_t)r < 0) cpu.psw |= PSW_N;
        if (r == 0)          cpu.psw |= PSW_Z;
        if ((a ^ src) & (a ^ r) & 0x8000u) cpu.psw |= PSW_V;
        if (a < src) cpu.psw |= PSW_C;
    }

    /* jsr pc, R_14302 — второй glyph. */
    {
        uint8_t ch = bk_read_b(cpu.r[2]);
        cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[5]);
        cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[1]);
        inline_draw_glyph(cpu.r[2], cpu.r[1]);
        cpu.r[3] = (uint16_t)(KLAD2_FONT_BASE + (unsigned)ch * 16u + 16u);
        uint16_t r1_prev = (uint16_t)(cpu.r[1] + 7u * SCREEN_ROW_BYTES);
        uint16_t r1_last = (uint16_t)(r1_prev + SCREEN_ROW_BYTES);
        if (r1_last < r1_prev) cpu.psw |= PSW_C;
        else                    cpu.psw &= (uint16_t)~PSW_C;
        cpu.r[1] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
        cpu.r[5] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)cpu.r[5] < 0) cpu.psw |= PSW_N;
        if (cpu.r[5] == 0)         cpu.psw |= PSW_Z;
    }

    /* mov @#17430, r0; mov table_dch_off(r0), @#cp_addr;
     * mov table_dx_off(r0), @#x_addr; clr @#timer_addr. */
    cpu.r[0] = bk_read_w(0017430u);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if ((int16_t)cpu.r[0] < 0) cpu.psw |= PSW_N;
    if (cpu.r[0] == 0)         cpu.psw |= PSW_Z;
    {
        uint16_t v = bk_read_w((uint16_t)(cpu.r[0] + table_dch_off));
        bk_write_w(cp_addr, v);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)v < 0) cpu.psw |= PSW_N;
        if (v == 0)          cpu.psw |= PSW_Z;
    }
    {
        uint16_t v = bk_read_w((uint16_t)(cpu.r[0] + table_dx_off));
        bk_write_w(x_addr, v);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)v < 0) cpu.psw |= PSW_N;
        if (v == 0)          cpu.psw |= PSW_Z;
    }
    bk_write_w(timer_addr, 0);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_V | PSW_C);
    cpu.psw |= PSW_Z;

    rts_pc();
}

/* 0x0d32 (= 0o6462). Телепорт врага E2 через тайл-«дверь» (тип 13).
 * Одна из 7 функций главного цикла; вызывается с 0x039a. */
void R_06462_e2_teleport(void)
{
    teleport_object(0014442u, 0014446u, 020u, 022u, 0017376u);
}

/* 0x0ec6 (= 0o7306). Телепорт героя — изоморфен R_06462, но через
 * char_ptr_hero @ 0o14432, X @ 0o14436, табл. @#17430+0o14/0o16,
 * таймер @#17360. Одна из 7 функций главного цикла; вызывается с 0x0396. */
void R_07306_hero_teleport(void)
{
    teleport_object(0014432u, 0014436u, 014u, 016u, 0017360u);
}

/* ---------------------------------------------------------------------
 * R_07462_main_e1 — обработчик ИИ врага E1 ВЕРХНЕГО УРОВНЯ
 * (0x0f32..0x1040, 270 байт). Изоморфен R_06632 (E2) с отличиями:
 *   - Нет dead-check tst @#14442 в начале (E2 такой имеет).
 *   - Таймер: `cmp #400, @#17360; bge inc+rts` (срабатывает когда
 *     cell > 0o400, не cell == 0o100 как у E2).
 *   - Адреса: anim_e1=0o14430 (vs 0o14440), таймер @#17360 (vs 17376),
 *     hero_target=@#14442 (= позиция E2!).
 *   - JSR R_10102/R_10142 вместо R_07202/R_07242.
 *
 * Точка входа: вызывается через `JSR pc, @#0x0f32` с адреса 0x0392
 * (одна из 7 функций главного цикла).
 *
 * Реализация байт-аккуратная: каждая инструкция эмулирует регистры и
 * PSW по образцу R_14030_body. Не использует старую R_06602 (которая
 * содержала логические баги).
 * ------------------------------------------------------------------- */
enum {
    ANIM_E1            = 0014430u,
    ANIM_E1_CHAR_PTR   = 0014432u,   /* = ANIM_E1 + 2 (post-autoinc r5) */
    TIMER_E1           = 0017360u,
    HERO_TARGET_E1     = 0014442u,   /* hero-target для E1 = char_ptr_e2 */
    TIMER_THRESHOLD_E1 = 0000400u,
};

/* Хелпер: установить PSW.N/Z/V по слову (mov-стиль; V=0, C unchanged). */
static inline void psw_nz_w(uint16_t v)
{
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if ((int16_t)v < 0) cpu.psw |= PSW_N;
    if (v == 0)         cpu.psw |= PSW_Z;
}

/* Хелпер: PSW по результату cmp src - dst (word). */
static inline void psw_cmp_w(uint16_t src, uint16_t dst)
{
    uint16_t res = (uint16_t)(src - dst);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
    if ((int16_t)res < 0) cpu.psw |= PSW_N;
    if (res == 0)        cpu.psw |= PSW_Z;
    if ((src ^ dst) & (src ^ res) & 0x8000u) cpu.psw |= PSW_V;
    if (src < dst)       cpu.psw |= PSW_C;
}

void R_07462_main_e1(void)
{
    uint16_t v;

    /* 0x0f32: cmp #400, @#17360 — установить PSW. */
    v = bk_read_w(TIMER_E1);
    psw_cmp_w(TIMER_THRESHOLD_E1, v);

    /* 0x0f38: bge 0x0f3c — берётся при 0o400 >= timer.
     *         signed >= ↔ !(N XOR V). */
    if ((int16_t)TIMER_THRESHOLD_E1 >= (int16_t)v) {
        /* 0x0f3c: inc @#17360. PSW.N/Z/V; C unchanged. */
        uint16_t nxt = (uint16_t)(v + 1);
        bk_write_w(TIMER_E1, nxt);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)nxt < 0) cpu.psw |= PSW_N;
        if (nxt == 0)         cpu.psw |= PSW_Z;
        if (v == 0x7FFFu)     cpu.psw |= PSW_V;
        /* 0x0f40: rts pc. */
        rts_pc();
        return;
    }
    /* 0x0f3a: br 0x0f42 — продолжаем в main path. */

    /* 0x0f42: mov #14430, r5. */
    cpu.r[5] = ANIM_E1;
    psw_nz_w(cpu.r[5]);

    /* 0x0f46: mov #14422, r4. */
    cpu.r[4] = ANIM_HERO_CP_PTR;
    psw_nz_w(cpu.r[4]);

    /* 0x0f4a: cmp #10, (r5)+ — set PSW; r5 += 2 → r5 = 14432. */
    {
        uint16_t old_code = bk_read_w(cpu.r[5]);
        cpu.r[5] = (uint16_t)(cpu.r[5] + 2u);
        psw_cmp_w(0010u, old_code);
        /* 0x0f4e: beq 0x101e — old_code == 8 → idle. */
        if (old_code == 0010u) goto L_idle;
    }

    /* 0x0f50: mov (r5), r0 → char_ptr_e1. */
    cpu.r[0] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[0]);

    /* 0x0f52-56: sub #14550, r0; bic #177700, r0 → e1_x_idx. */
    cpu.r[0] = (uint16_t)(cpu.r[0] - MAP_BASE);
    cpu.r[0] &= 0077u;

    /* 0x0f5a: mov (r4), r1 → char_ptr_hero. */
    cpu.r[1] = bk_read_w(cpu.r[4]);
    psw_nz_w(cpu.r[1]);

    /* 0x0f5c-60: sub #14550, r1; bic #177700, r1 → hero_x_idx. */
    cpu.r[1] = (uint16_t)(cpu.r[1] - MAP_BASE);
    cpu.r[1] &= 0077u;

    /* 0x0f64: cmp r0, r1 — set PSW.
     * 0x0f66: beq 0x0fa8 — e1_x == hero_x → vertical.
     * 0x0f68: bgt 0x0fde — e1_x > hero_x → try_left. */
    psw_cmp_w(cpu.r[0], cpu.r[1]);
    if (cpu.r[0] == cpu.r[1]) goto L_vertical;
    if ((int16_t)cpu.r[0] > (int16_t)cpu.r[1]) goto L_try_left;

    /* === try_right (e1 слева от hero) === */
    /* 0x0f6a: mov (r5), r1. */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);

    /* 0x0f6c: bit #100000, (r1). */
    v = bk_read_w(cpu.r[1]);
    {
        uint16_t res = (uint16_t)(0100000u & v);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_vertical;  /* beq */
    }

    /* 0x0f72: cmpb #11, 2(r1). */
    {
        uint8_t bv = bk_read_b((uint16_t)(cpu.r[1] + 2u));
        uint8_t res = (uint8_t)(0011u - bv);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int8_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if ((uint8_t)((0011u ^ bv) & (0011u ^ res) & 0x80u)) cpu.psw |= PSW_V;
        if ((uint8_t)0011u < bv) cpu.psw |= PSW_C;
        if (bv == 0011u) goto L_vertical;
    }

    /* 0x0f7a: add #2, r1. */
    cpu.r[1] = (uint16_t)(cpu.r[1] + 2u);

    /* 0x0f7e: cmp r1, @#14442; beq 0x0fa8. */
    v = bk_read_w(HERO_TARGET_E1);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_vertical;

    /* 0x0f84: clr r0; 0x0f86: jsr R_10102. */
    cpu.r[0] = 0;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_V | PSW_C);
    cpu.psw |= PSW_Z;
    anim_tick(0021760u, ANIM_E1, 0017362u, 0, 7);  /* R_10102 body */
    /* fall through в main_tail */

L_main_tail:
    /* 0x0f8a: mov #14430, r4. */
    cpu.r[4] = ANIM_E1;
    psw_nz_w(cpu.r[4]);

    /* 0x0f8e: bit #4000, @2(r4) — test bit 04000 in mem[mem[r4+2]]. */
    {
        uint16_t cp = bk_read_w((uint16_t)(cpu.r[4] + 2u));
        uint16_t cell = bk_read_w(cp);
        uint16_t res = (uint16_t)(0004000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res != 0) {  /* bne 0x0fa6 → rts */
            rts_pc();
            return;
        }
    }

    /* 0x0f96: mov (r5), r1; 0x0f98: add #100, r1. */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    cpu.r[1] = (uint16_t)(cpu.r[1] + 0100u);

    /* 0x0f9c: cmp r1, @#14442; beq 0x0fa6 → rts. */
    v = bk_read_w(HERO_TARGET_E1);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) { rts_pc(); return; }

    /* 0x0fa2: jsr R_10142 (idle anim). */
    cpu.r[0] = 0010u;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    /* msb(0o10)=0 → N=0; ≠0 → Z=0. */
    anim_tick(0021760u, ANIM_E1, 0017364u, 0, 4);  /* R_10142 body */

    /* 0x0fa6: rts pc. */
    rts_pc();
    return;

L_vertical:
    /* 0x0fa8: vertical block — сравнение Y-координат. */
    cpu.r[0] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[0]);
    cpu.r[0] = (uint16_t)(cpu.r[0] - MAP_BASE);
    cpu.r[0] &= 0177700u;
    cpu.r[1] = bk_read_w(cpu.r[4]);
    psw_nz_w(cpu.r[1]);
    cpu.r[1] = (uint16_t)(cpu.r[1] - MAP_BASE);
    cpu.r[1] &= 0177700u;

    /* 0x0fbc: cmp r0, r1; beq 0x0f8a (main_tail); bgt 0x1002 (try_up). */
    psw_cmp_w(cpu.r[0], cpu.r[1]);
    if (cpu.r[0] == cpu.r[1]) goto L_main_tail;
    if ((int16_t)cpu.r[0] > (int16_t)cpu.r[1]) goto L_try_up;

    /* === try_down (e1 выше hero) === */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0010000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_main_tail;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] + 0100u);
    v = bk_read_w(HERO_TARGET_E1);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_main_tail;
    cpu.r[0] = 6;
    psw_nz_w(cpu.r[0]);
    anim_tick(0021760u, ANIM_E1, 0017362u, 0, 7);  /* R_10102 */
    goto L_main_tail;

L_try_left:
    /* === try_left (e1 правее hero) === */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0040000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_vertical;
    }
    /* cmpb #11, 177776(r1) — байт по адресу r1 - 2. */
    {
        uint8_t bv = bk_read_b((uint16_t)(cpu.r[1] - 2u));
        uint8_t res = (uint8_t)(0011u - bv);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int8_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if ((uint8_t)((0011u ^ bv) & (0011u ^ res) & 0x80u)) cpu.psw |= PSW_V;
        if ((uint8_t)0011u < bv) cpu.psw |= PSW_C;
        if (bv == 0011u) goto L_vertical;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] - 2u);
    v = bk_read_w(HERO_TARGET_E1);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_vertical;
    cpu.r[0] = 2;
    psw_nz_w(cpu.r[0]);
    anim_tick(0021760u, ANIM_E1, 0017362u, 0, 7);
    goto L_main_tail;

L_try_up:
    /* === try_up (e1 ниже hero) === */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0020000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_main_tail;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] - 0100u);
    v = bk_read_w(HERO_TARGET_E1);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_main_tail;
    cpu.r[0] = 4;
    psw_nz_w(cpu.r[0]);
    anim_tick(0021760u, ANIM_E1, 0017362u, 0, 7);
    goto L_main_tail;

L_idle:
    /* === 0x101e: idle path === */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    cpu.r[1] = (uint16_t)(cpu.r[1] + 0100u);
    v = bk_read_w(HERO_TARGET_E1);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_idle_set6;

    /* 0x102a: mov #10, r0; 0x102e: jsr R_10142. */
    cpu.r[0] = 0010u;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    anim_tick(0021760u, ANIM_E1, 0017364u, 0, 4);

    /* 0x1032: bit #2000, @2(r4). */
    {
        uint16_t cp = bk_read_w((uint16_t)(cpu.r[4] + 2u));
        uint16_t cell = bk_read_w(cp);
        uint16_t res = (uint16_t)(0002000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res != 0) { rts_pc(); return; }  /* bne 0x0fa6 */
    }

L_idle_set6:
    /* 0x103a: mov #6, @#14430. */
    bk_write_w(ANIM_E1, 6);
    psw_nz_w(6);
    /* 0x1040: br 0x0fa6 → rts. */
    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_06632_main_e2 — обработчик ИИ врага E2 ВЕРХНЕГО УРОВНЯ
 * (0x0d6a..0x0e80, 278 байт). Изоморфен R_07462 с отличиями:
 *   - В начале dead-check `tst @#14442; bne; rts` (если char_ptr_e2==0,
 *     ранний выход — E1 такой проверки не имеет).
 *   - Таймер: `cmp #400, @#17376; bne inc+rts; br main` — main path
 *     срабатывает когда cell == 0o400 точно (vs E1 cell > 0o400).
 *   - Адреса: anim_e2=0o14440, таймер @#17376, hero_target=@#14432.
 *   - JSR R_07202/R_07242 вместо R_10102/R_10142.
 *
 * Точка входа: вызывается через `JSR pc, @#0x0d6a` с адреса 0x038e
 * (одна из 7 функций главного цикла).
 * ------------------------------------------------------------------- */
enum {
    TIMER_E2           = 0017376u,
    HERO_TARGET_E2     = 0014432u,
    TIMER_THRESHOLD_E2 = 0000400u,
};

void R_06632_main_e2(void)
{
    uint16_t v;

    /* === dead-check: tst @#14442; bne 0x0d72; rts === */
    v = bk_read_w(ANIM_E2_CHAR_PTR);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
    if ((int16_t)v < 0) cpu.psw |= PSW_N;
    if (v == 0)         cpu.psw |= PSW_Z;
    if (v == 0) { rts_pc(); return; }

    /* === Таймер: cmp #400, @#17376; bne inc+rts === */
    v = bk_read_w(TIMER_E2);
    psw_cmp_w(TIMER_THRESHOLD_E2, v);
    if (v != TIMER_THRESHOLD_E2) {
        uint16_t nxt = (uint16_t)(v + 1);
        bk_write_w(TIMER_E2, nxt);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)nxt < 0) cpu.psw |= PSW_N;
        if (nxt == 0)         cpu.psw |= PSW_Z;
        if (v == 0x7FFFu)     cpu.psw |= PSW_V;
        rts_pc();
        return;
    }

    /* === main path init === */
    cpu.r[5] = ANIM_E2;
    psw_nz_w(cpu.r[5]);
    cpu.r[4] = ANIM_HERO_CP_PTR;
    psw_nz_w(cpu.r[4]);
    {
        uint16_t old_code = bk_read_w(cpu.r[5]);
        cpu.r[5] = (uint16_t)(cpu.r[5] + 2u);
        psw_cmp_w(0010u, old_code);
        if (old_code == 0010u) goto L_e2_idle;
    }

    cpu.r[0] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[0]);
    cpu.r[0] = (uint16_t)(cpu.r[0] - MAP_BASE);
    cpu.r[0] &= 0077u;
    cpu.r[1] = bk_read_w(cpu.r[4]);
    psw_nz_w(cpu.r[1]);
    cpu.r[1] = (uint16_t)(cpu.r[1] - MAP_BASE);
    cpu.r[1] &= 0077u;
    psw_cmp_w(cpu.r[0], cpu.r[1]);
    if (cpu.r[0] == cpu.r[1]) goto L_e2_vertical;
    if ((int16_t)cpu.r[0] > (int16_t)cpu.r[1]) goto L_e2_try_left;

    /* try_right */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0100000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_e2_vertical;
    }
    {
        uint8_t bv = bk_read_b((uint16_t)(cpu.r[1] + 2u));
        uint8_t res = (uint8_t)(0011u - bv);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int8_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if ((uint8_t)((0011u ^ bv) & (0011u ^ res) & 0x80u)) cpu.psw |= PSW_V;
        if ((uint8_t)0011u < bv) cpu.psw |= PSW_C;
        if (bv == 0011u) goto L_e2_vertical;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] + 2u);
    v = bk_read_w(HERO_TARGET_E2);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_e2_vertical;
    cpu.r[0] = 0;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_V | PSW_C);
    cpu.psw |= PSW_Z;
    anim_tick(0021760u, ANIM_E2, 0017366u, 1, 7);

L_e2_main_tail:
    cpu.r[4] = ANIM_E2;
    psw_nz_w(cpu.r[4]);
    {
        uint16_t cp = bk_read_w((uint16_t)(cpu.r[4] + 2u));
        uint16_t cell = bk_read_w(cp);
        uint16_t res = (uint16_t)(0004000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res != 0) { rts_pc(); return; }
    }
    cpu.r[1] = bk_read_w(cpu.r[5]);
    cpu.r[1] = (uint16_t)(cpu.r[1] + 0100u);
    v = bk_read_w(HERO_TARGET_E2);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) { rts_pc(); return; }
    cpu.r[0] = 0010u;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    anim_tick(0021760u, ANIM_E2, 0017370u, 1, 4);
    rts_pc();
    return;

L_e2_vertical:
    cpu.r[0] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[0]);
    cpu.r[0] = (uint16_t)(cpu.r[0] - MAP_BASE);
    cpu.r[0] &= 0177700u;
    cpu.r[1] = bk_read_w(cpu.r[4]);
    psw_nz_w(cpu.r[1]);
    cpu.r[1] = (uint16_t)(cpu.r[1] - MAP_BASE);
    cpu.r[1] &= 0177700u;
    psw_cmp_w(cpu.r[0], cpu.r[1]);
    if (cpu.r[0] == cpu.r[1]) goto L_e2_main_tail;
    if ((int16_t)cpu.r[0] > (int16_t)cpu.r[1]) goto L_e2_try_up;

    /* try_down */
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0010000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_e2_main_tail;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] + 0100u);
    v = bk_read_w(HERO_TARGET_E2);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_e2_main_tail;
    cpu.r[0] = 6;
    psw_nz_w(cpu.r[0]);
    anim_tick(0021760u, ANIM_E2, 0017366u, 1, 7);
    goto L_e2_main_tail;

L_e2_try_left:
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0040000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_e2_vertical;
    }
    {
        uint8_t bv = bk_read_b((uint16_t)(cpu.r[1] - 2u));
        uint8_t res = (uint8_t)(0011u - bv);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int8_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if ((uint8_t)((0011u ^ bv) & (0011u ^ res) & 0x80u)) cpu.psw |= PSW_V;
        if ((uint8_t)0011u < bv) cpu.psw |= PSW_C;
        if (bv == 0011u) goto L_e2_vertical;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] - 2u);
    v = bk_read_w(HERO_TARGET_E2);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_e2_vertical;
    cpu.r[0] = 2;
    psw_nz_w(cpu.r[0]);
    anim_tick(0021760u, ANIM_E2, 0017366u, 1, 7);
    goto L_e2_main_tail;

L_e2_try_up:
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    {
        uint16_t cell = bk_read_w(cpu.r[1]);
        uint16_t res = (uint16_t)(0020000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res == 0) goto L_e2_main_tail;
    }
    cpu.r[1] = (uint16_t)(cpu.r[1] - 0100u);
    v = bk_read_w(HERO_TARGET_E2);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_e2_main_tail;
    cpu.r[0] = 4;
    psw_nz_w(cpu.r[0]);
    anim_tick(0021760u, ANIM_E2, 0017366u, 1, 7);
    goto L_e2_main_tail;

L_e2_idle:
    cpu.r[1] = bk_read_w(cpu.r[5]);
    psw_nz_w(cpu.r[1]);
    cpu.r[1] = (uint16_t)(cpu.r[1] + 0100u);
    v = bk_read_w(HERO_TARGET_E2);
    psw_cmp_w(cpu.r[1], v);
    if (cpu.r[1] == v) goto L_e2_idle_set6;
    cpu.r[0] = 0010u;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    anim_tick(0021760u, ANIM_E2, 0017370u, 1, 4);
    {
        uint16_t cp = bk_read_w((uint16_t)(cpu.r[4] + 2u));
        uint16_t cell = bk_read_w(cp);
        uint16_t res = (uint16_t)(0002000u & cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)res < 0) cpu.psw |= PSW_N;
        if (res == 0)        cpu.psw |= PSW_Z;
        if (res != 0) { rts_pc(); return; }
    }
L_e2_idle_set6:
    bk_write_w(ANIM_E2, 6);
    psw_nz_w(6);
    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_14d6_bel — обёртка над EMT 016 R0=7 (звук «BEL»).
 * (0x14d6..0x14e0, 11 байт, 5 инструкций.)
 *
 * Используется как помощник из R_06402 (sound+dec_lives) и R_12570
 * (обработчик специальных тайлов под героем).
 *
 * Оригинал:
 *   mov   r0, -(sp)    ; push r0
 *   mov   #7, r0       ; r0 = 7
 *   emt   016          ; BEL — у нас обрабатывается в emt.c через
 *                      ; audio_emit_bell() для R0=7
 *   mov   (sp)+, r0    ; pop r0 (restore caller's r0)
 *   rts   pc
 *
 * Финал: cpu.r[0] = caller's r0 (unchanged); PSW: N/Z от r0, V=0,
 * C unchanged (mov (sp)+ не трогает C).
 * ------------------------------------------------------------------- */
void R_14d6_bel(void)
{
    /* mov r0, -(sp). */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], cpu.r[0]);

    /* mov #7, r0. PSW: msb(7)=0, !=0 → N=0, Z=0, V=0. */
    cpu.r[0] = 7;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);

    /* emt 016 → BEL через нашу аудио-подсистему. */
    audio_emit_bell();

    /* mov (sp)+, r0 — pop r0, устанавливает PSW.N/Z по pop'нутому. */
    cpu.r[0] = bk_read_w(cpu.r[6]);
    cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    psw_nz_w(cpu.r[0]);

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_15ce_clear_cell — устанавливает «пустую» ячейку карты по r5.
 * (0x15ce..0x15de, 16 байт, 4 инструкции.)
 *
 * Используется из R_12570 (обработчик специальных тайлов).
 *
 * Оригинал:
 *   movb  #11, (r5)              ; byte at r5 = 0o11 (тип «лестница»?)
 *   bis   #100000, 177776(r5)    ; mem[r5-2] |= 0o100000 (← бит «можно вправо»)
 *   bis   #40000,  2(r5)         ; mem[r5+2] |= 0o40000 (← бит «можно влево»)
 *   rts   pc
 *
 * Семантика: устанавливает в центральной клетке тип 0o11, и в соседних
 * слева/справа выставляет битмаски проходимости. Это что-то типа
 * «открыть лестницу» — когда герой подбирает определённый предмет.
 *
 * Регистры: cpu.r[5] unchanged. PSW от bis #40000 (последняя инструкция):
 *   N/Z по результату, V=0, C unchanged.
 * ------------------------------------------------------------------- */
void R_15ce_clear_cell(void)
{
    uint16_t r5 = cpu.r[5];

    /* movb #11, (r5). PSW.N/Z по записываемому байту, V=0. */
    bk_write_b(r5, 0011u);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    /* 0o11 = 9 > 0, msb=0 → N=0, Z=0. */

    /* bis #100000, 177776(r5) = bis #0o100000, mem[r5 - 2]. */
    {
        uint16_t a = (uint16_t)(r5 - 2u);
        uint16_t v = (uint16_t)(bk_read_w(a) | 0100000u);
        bk_write_w(a, v);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)v < 0) cpu.psw |= PSW_N;
        if (v == 0)         cpu.psw |= PSW_Z;
    }

    /* bis #40000, 2(r5) = bis #0o40000, mem[r5 + 2]. */
    {
        uint16_t a = (uint16_t)(r5 + 2u);
        uint16_t v = (uint16_t)(bk_read_w(a) | 0040000u);
        bk_write_w(a, v);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)v < 0) cpu.psw |= PSW_N;
        if (v == 0)         cpu.psw |= PSW_Z;
    }

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_0888_int_to_dec — преобразовать count 16-битных чисел в 6-байтовые
 * decimal-ASCII строки. (0x0888..0x08fe, 120 байт, ~30 инструкций.)
 *
 * Параметры через стек caller-а (после JSR):
 *   sp+0:  PC возврата
 *   sp+2:  buffer_ptr — куда писать (6 байт на число)
 *   sp+4:  source_ptr — массив 16-битных чисел
 *   sp+6:  count      — число чисел
 *   sp+8..sp+19: 12 байт зарезервированы под save-регистров
 *                (выделены caller-ом через `sub #12, sp`)
 *
 * Формат каждого слота:
 *   - 6 байт, заполняются справа налево (digit-by-digit + знак + пробелы).
 *   - Слот «12345»  → "  1234"  (нет, ширина 6 → " 12345" если 5 цифр).
 *   - Слот -123    → "  -123".
 *
 * Алгоритм:
 *   for каждое из count чисел:
 *     прочитать число num;
 *     если < 0 → запомнить флаг, num = -num;
 *     извлечь digits справа налево через num %= 10, num /= 10;
 *     если был знак → записать '-';
 *     дополнить пробелами слева до 6 байт.
 *
 * После работы caller-а R_07aa также делает `sub #12, sp` повторно
 * (под второй буфер) и второй JSR на R_0888. Так что финальный SP
 * после R_0888 должен указывать на caller-а — это обеспечивается
 * эмуляцией всех stack-операций.
 *
 * Сохраняемые регистры: r1, r2, r4, r5 (по push/pop в стек-слоты).
 *
 * Финальное PSW от `mov (sp)+, r5`: N/Z по итоговому r5 (= caller's r5).
 *
 * Вызывается 7 раз в коде (печать счёта, попыток и т.п.).
 * ------------------------------------------------------------------- */
void R_0888_int_to_dec(void)
{
    uint16_t sp_0 = cpu.r[6];

    /* Сохранение caller-овских регистров в зарезервированные слоты. */
    uint16_t saved_r1 = cpu.r[1];
    uint16_t saved_r2 = cpu.r[2];
    uint16_t saved_r4 = cpu.r[4];
    uint16_t saved_r5 = cpu.r[5];

    /* Чтение аргументов со стека. */
    uint16_t pc_ret      = bk_read_w(sp_0);
    uint16_t buffer_ptr  = bk_read_w((uint16_t)(sp_0 + 2));
    uint16_t source_ptr  = bk_read_w((uint16_t)(sp_0 + 4));
    int16_t  count       = (int16_t)bk_read_w((uint16_t)(sp_0 + 6));

    /* Эмуляция «save регистров в зарезервированные стек-слоты».
     * Оригинал: 0888-0896 пишет r1/r2/r4/r5 в (sp+8..sp+14).
     * Затем 0898 копирует PC_return в (sp+16). */
    bk_write_w((uint16_t)(sp_0 +  8), saved_r1);
    bk_write_w((uint16_t)(sp_0 + 10), saved_r2);
    bk_write_w((uint16_t)(sp_0 + 12), saved_r4);
    bk_write_w((uint16_t)(sp_0 + 14), saved_r5);
    bk_write_w((uint16_t)(sp_0 + 16), pc_ret);

    /* Цикл: для каждого числа. Оригинал начинает с r5 = buffer_ptr - 6
     * и делает r5 += 6 в начале каждой итерации. */
    uint16_t buffer = (uint16_t)(buffer_ptr - 6u);
    uint16_t source = source_ptr;

    while (1) {
        buffer = (uint16_t)(buffer + 6u);     /* add #6, r5 */
        bk_write_b(buffer, 0);                 /* clrb (r5) — null terminator */
        count--;                               /* dec (sp) */
        if (count < 0) break;                  /* bmi exit */

        uint16_t slot_end = (uint16_t)(buffer + 6u);
        int16_t  width = 6;

        /* Прочитать число; если < 0 — взять модуль. */
        uint16_t raw = bk_read_w(source);
        int16_t  num = (int16_t)raw;
        int      negative = num < 0;
        uint16_t abs_num = negative ? (uint16_t)(-num) : (uint16_t)num;

        /* Извлекаем цифры справа налево. */
        do {
            uint16_t digit = abs_num % 10u;
            abs_num = abs_num / 10u;
            slot_end = (uint16_t)(slot_end - 1u);
            bk_write_b(slot_end, (uint8_t)(digit | 0060u));
            width--;
        } while (abs_num != 0);

        source = (uint16_t)(source + 2u);      /* tst (r4)+ */

        /* Если число было отрицательным — '-' перед цифрами. */
        if (negative) {
            slot_end = (uint16_t)(slot_end - 1u);
            bk_write_b(slot_end, 0055u);       /* '-' */
            width--;
            if (width == 0) continue;
        }

        /* Заполняем оставшееся пробелами. */
        while (width > 0) {
            slot_end = (uint16_t)(slot_end - 1u);
            bk_write_b(slot_end, 0040u);       /* ' ' */
            width--;
        }
    }

    /* Восстановление регистров из стек-слотов (по тому же порядку, что
     * оригинал — pop r1, r2, r4, r5). */
    cpu.r[1] = bk_read_w((uint16_t)(sp_0 +  8));
    cpu.r[2] = bk_read_w((uint16_t)(sp_0 + 10));
    cpu.r[4] = bk_read_w((uint16_t)(sp_0 + 12));
    cpu.r[5] = bk_read_w((uint16_t)(sp_0 + 14));

    /* Финальное PSW от mov (sp)+, r5: N/Z по итоговому r5, V=0, C unchanged. */
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    if ((int16_t)cpu.r[5] < 0) cpu.psw |= PSW_N;
    if (cpu.r[5] == 0)         cpu.psw |= PSW_Z;

    /* Оригинал заканчивает с SP = sp_0 + 16 (указывает на PC_return copy);
     * затем rts pc снимает PC и SP += 2 → sp_0 + 18. */
    cpu.r[6] = (uint16_t)(sp_0 + 16);
    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_07aa_print_score — печать строки «Счёт N Попыток M» через EMT
 * 020/024 + двойной вызов R_0888 для int → ASCII.
 * (0x07aa..0x07e4, 60 байт, 11 инструкций.)
 *
 * Оригинал:
 *   mov   #1, r2          ; cursor.y = 1
 *   clr   r1              ; cursor.x = 0
 *   emt   024             ; set cursor
 *   sub   #12, sp         ; reserve 10 dec (= 0o12) bytes для R_0888 work
 *   mov   #1, -(sp)       ; arg3: count = 1
 *   mov   #17440, -(sp)   ; arg2: source = &счёт
 *   mov   #2162, -(sp)    ; arg1: buffer = строка для счёта
 *   jsr   pc, @#R_0888    ; convert счёт → ASCII
 *   sub   #12, sp         ; reserve again
 *   mov   #1, -(sp)       ; count = 1
 *   mov   #17436, -(sp)   ; source = &попытки
 *   mov   #2204, -(sp)    ; buffer = строка для попыток
 *   jsr   pc, @#R_0888
 *   mov   #2154, r1       ; r1 = адрес «Счёт» строки
 *   mov   #1437, r2       ; r2 = параметры для EMT 020 (длина+разделитель)
 *   emt   020             ; print string
 *   rts   pc
 *
 * R_0888 кладёт результат конверсии в буфер [r1..r1+5], которая входит
 * в составную строку «Счёт XXXXXX Попыток XXXXXX», адрес которой
 * передаётся в EMT 020.
 *
 * Стек-баланс: каждый цикл `sub #12, sp; pushes; jsr R_0888` тратит
 * 18 байт, а R_0888 возвращает SP +18. Итого нет потерь.
 * ------------------------------------------------------------------- */
void R_07aa_print_score(void)
{
    /* mov #1, r2 — PSW.N=0, Z=0, V=0, C unchanged. */
    cpu.r[2] = 1;
    psw_nz_w(1);

    /* clr r1 — PSW: N=0, Z=1, V=0, C=0. */
    cpu.r[1] = 0;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_V | PSW_C);
    cpu.psw |= PSW_Z;

    /* emt 024 — set cursor (r1, r2). */
    emt_dispatch(024u);

    /* sub #10dec (= 0o12), sp. */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 10u);

    /* mov #1, -(sp). */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 1);
    psw_nz_w(1);

    /* mov #17440 octal (= 0x1F20), -(sp). */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 017440u);
    psw_nz_w(017440u);

    /* mov #2162 octal (= 0x472), -(sp). */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 002162u);
    psw_nz_w(002162u);

    /* jsr pc, @#R_0888 — push PC return (0x07c6), call R_0888. */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 0x07c6);
    R_0888_int_to_dec();

    /* sub #10dec, sp (second time). */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 10u);

    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 1);
    psw_nz_w(1);

    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 017436u);
    psw_nz_w(017436u);

    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 002204u);
    psw_nz_w(002204u);

    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], 0x07da);
    R_0888_int_to_dec();

    /* mov #2154 octal, r1. */
    cpu.r[1] = 002154u;
    psw_nz_w(002154u);

    /* mov #1437 octal, r2. */
    cpu.r[2] = 001437u;
    psw_nz_w(001437u);

    /* emt 020 — print string at r1 with len/delim from r2. */
    emt_dispatch(020u);

    rts_pc();
}

/* Тело R_1558 без rts_pc (для inline-использования из R_1522). */
static void R_1558_body(void)
{
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[3]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[2]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[1]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[5]);
    cpu.r[5] = 0010u;
    psw_nz_w(cpu.r[5]);
    for (unsigned i = 0; i < 8; i++) {
        uint16_t w = bk_read_w(cpu.r[2]);
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + cpu.r[1]), w);
        cpu.r[2] = (uint16_t)(cpu.r[2] + 2u);
        cpu.r[1] = (uint16_t)(cpu.r[1] + SCREEN_ROW_BYTES);
        cpu.r[5]--;
    }
    {
        uint16_t r1_prev = (uint16_t)(cpu.r[1] - SCREEN_ROW_BYTES);
        if (cpu.r[1] < r1_prev) cpu.psw |= PSW_C;
        else                     cpu.psw &= (uint16_t)~PSW_C;
    }
    cpu.r[5] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[1] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[2] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[3] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    psw_nz_w(cpu.r[3]);
}

/* ---------------------------------------------------------------------
 * R_1522_draw_map — полный рендер карты уровня (22×32 = 704 ячейки).
 * (0x1522..0x1556, 54 байта, 14 инструкций.)
 *
 * Двойной цикл sob: outer 22 раза, inner 32 раза. На каждой итерации:
 *   r2 = byte из карты (4 low bits = индекс глифа в шрифте);
 *   r2 <<= 4 → font offset;
 *   r2 += 0o17450 → font_base + offset;
 *   jsr R_1558 (нарисовать 8 строк по позиции r1);
 *   r1 += 2 (следующая колонка).
 * После inner: pop r1; r1 += 0o1000 (= 512, восемь строк экрана).
 *
 * Вход: ничего (использует свои константы).
 * Финал:
 *   r1 = 22 * 0o1000 = 0o26000
 *   r2 = font ptr последнего глифа
 *   r3 = 0 (sob exit)
 *   r4 = 0o14550 + 1408 байт = 0o17350 (= конец карты)
 *   r5 = 0 (outer sob exit)
 *   PSW от последнего add #1000, r1.
 * ------------------------------------------------------------------- */
void R_1522_draw_map(void)
{
    cpu.r[4] = MAP_BASE;  /* 0o14550 */
    psw_nz_w(cpu.r[4]);
    cpu.r[1] = 0;
    psw_nz_w(cpu.r[1]);
    cpu.r[5] = 0026u;     /* 22 */
    psw_nz_w(cpu.r[5]);

    while (1) {
        /* push r1. */
        cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
        bk_write_w(cpu.r[6], cpu.r[1]);

        cpu.r[3] = 0040u;  /* 32 */
        psw_nz_w(cpu.r[3]);

        while (1) {
            /* mov (r4)+, r2. */
            cpu.r[2] = bk_read_w(cpu.r[4]);
            cpu.r[4] = (uint16_t)(cpu.r[4] + 2u);

            /* bic #177760, r2; asl x4; add #17450, r2. */
            cpu.r[2] &= 0017u;
            cpu.r[2] <<= 4;
            cpu.r[2] = (uint16_t)(cpu.r[2] + 017450u);

            /* jsr pc, R_1558 — inline тело. */
            R_1558_body();

            /* add #2, r1. */
            cpu.r[1] = (uint16_t)(cpu.r[1] + 2u);

            /* sob r3 — decrement, jump back if not zero. */
            cpu.r[3]--;
            if (cpu.r[3] == 0) break;
        }

        /* pop r1. */
        cpu.r[1] = bk_read_w(cpu.r[6]);
        cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);

        /* add #1000, r1. */
        uint16_t r1_prev = cpu.r[1];
        cpu.r[1] = (uint16_t)(cpu.r[1] + 01000u);

        /* sob r5. */
        cpu.r[5]--;
        if (cpu.r[5] == 0) {
            /* Финальное PSW от add #1000, r1: N/Z/V/C. */
            cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
            if ((int16_t)cpu.r[1] < 0) cpu.psw |= PSW_N;
            if (cpu.r[1] == 0)         cpu.psw |= PSW_Z;
            if ((uint16_t)(~(r1_prev ^ 01000u) & (r1_prev ^ cpu.r[1]) & 0x8000u))
                cpu.psw |= PSW_V;
            if (cpu.r[1] < r1_prev)    cpu.psw |= PSW_C;
            break;
        }
    }

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_0a46_draw_8rows_v2 — копия R_1558 семантически.
 * (0x0a46..0x0a64, 32 байта, 11 инструкций.)
 *
 * Различия с R_1558:
 *   - push r5, r4, r3, r1 (R_1558: r3, r2, r1, r5)
 *   - pop r1, r3, r4, r5 (соответственно)
 *
 * Тело идентично: r5=8; loop 8 times: copy word from (r2)+ to
 * 46000(r1), r1 += 0o100; pop, rts.
 *
 * Семантика та же: рисует 8-rows sprite напрямую из массива (без
 * подстановки через шрифт).
 *
 * Вызывается из 2-х мест в коде.
 * ------------------------------------------------------------------- */
void R_0a46_draw_8rows_v2(void)
{
    /* push r5, r4, r3, r1. */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[5]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[4]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[3]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[1]);

    /* mov #10, r5. */
    cpu.r[5] = 0010u;
    psw_nz_w(cpu.r[5]);

    /* Loop 8 раз. */
    for (unsigned i = 0; i < 8; i++) {
        uint16_t w = bk_read_w(cpu.r[2]);
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + cpu.r[1]), w);
        cpu.r[2] = (uint16_t)(cpu.r[2] + 2u);
        cpu.r[1] = (uint16_t)(cpu.r[1] + SCREEN_ROW_BYTES);
        cpu.r[5]--;
    }

    /* PSW.C от последнего add #100, r1. */
    {
        uint16_t r1_prev = (uint16_t)(cpu.r[1] - SCREEN_ROW_BYTES);
        if (cpu.r[1] < r1_prev) cpu.psw |= PSW_C;
        else                     cpu.psw &= (uint16_t)~PSW_C;
    }

    /* pop r1, r3, r4, r5. */
    cpu.r[1] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[3] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[4] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[5] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);

    /* Финал: PSW от mov (sp)+, r5 — N/Z по r5, V=0, C unchanged. */
    psw_nz_w(cpu.r[5]);

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_1558_draw_8rows — копирует 8 word'ов из массива в видеообласть.
 * (0x1558..0x1576, 32 байта, 11 инструкций.)
 *
 * Похоже на R_14302, но БЕЗ подстановки через шрифт — r2 уже указывает
 * на массив 8 слов спрайта (готовый sprite-data, не char-code).
 *
 * Оригинал:
 *   mov   r3, -(sp)
 *   mov   r2, -(sp)
 *   mov   r1, -(sp)
 *   mov   r5, -(sp)
 *   mov   #10, r5             ; counter = 8
 * loop:
 *   mov   (r2)+, 46000(r1)    ; mem[0o46000+r1] = *r2++
 *   add   #100, r1            ; r1 += 64
 *   sob   r5, loop
 *   mov   (sp)+, r5
 *   mov   (sp)+, r1
 *   mov   (sp)+, r2
 *   mov   (sp)+, r3
 *   rts   pc
 *
 * Вход: r1 = X (байтовая позиция), r2 = указатель на 8 слов спрайта.
 * Финал: r1, r2, r3, r5 = caller's (восстановлены через push/pop).
 *        PSW: N/Z по последнему pop'нутому r3; C от последнего add #100.
 *
 * Вызывается из 0x1578 (R_12570) и одного другого места.
 * ------------------------------------------------------------------- */
void R_1558_draw_8rows(void)
{
    /* push r3, r2, r1, r5. */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[3]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[2]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[1]);
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u); bk_write_w(cpu.r[6], cpu.r[5]);

    /* mov #10, r5. */
    cpu.r[5] = 0010u;
    psw_nz_w(cpu.r[5]);

    /* Loop 8 раз. */
    for (unsigned i = 0; i < 8; i++) {
        uint16_t w = bk_read_w(cpu.r[2]);
        bk_write_w((uint16_t)(SCREEN_DRAW_BASE + cpu.r[1]), w);
        cpu.r[2] = (uint16_t)(cpu.r[2] + 2u);
        cpu.r[1] = (uint16_t)(cpu.r[1] + SCREEN_ROW_BYTES);
        cpu.r[5]--;
    }

    /* PSW.C от последнего add #100, r1. */
    {
        uint16_t r1_prev = (uint16_t)(cpu.r[1] - SCREEN_ROW_BYTES);
        if (cpu.r[1] < r1_prev) cpu.psw |= PSW_C;
        else                     cpu.psw &= (uint16_t)~PSW_C;
    }

    /* pop r5, r1, r2, r3. */
    cpu.r[5] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[1] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[2] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    cpu.r[3] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);

    /* Финальное PSW от mov (sp)+, r3: N/Z по r3, V=0, C unchanged. */
    psw_nz_w(cpu.r[3]);

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_0262_check_key1 — проверка нажатия клавиши '1'.
 * (0x0262..0x026e, 14 байт, 4 инструкции.)
 *
 * Оригинал:
 *   cmp   #61, r0           ; r0 == 0o61 (= 49 = ASCII '1')?
 *   bne   rts                ; нет — выход
 *   mov   #400, @#1312       ; mem[0o1312] = 0o400 (флаг)
 *   rts   pc
 *
 * Семантика: если последний прочитанный код клавиатуры был '1',
 * устанавливаем флаг в системной ячейке 0o1312. Видимо, обработчик
 * меню при старте — выбор уровня.
 *
 * Регистры: cpu.r[0] не меняется. PSW от последней значимой инструкции
 * (либо cmp если bne taken, либо mov если fall-through).
 * ------------------------------------------------------------------- */
void R_0262_check_key1(void)
{
    /* cmp #61, r0. */
    {
        uint16_t src = 0061u, dst = cpu.r[0];
        uint16_t r = (uint16_t)(src - dst);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int16_t)r < 0) cpu.psw |= PSW_N;
        if (r == 0)          cpu.psw |= PSW_Z;
        if ((src ^ dst) & (src ^ r) & 0x8000u) cpu.psw |= PSW_V;
        if (src < dst)       cpu.psw |= PSW_C;
    }

    /* bne 0x026e — берётся при r0 != 0o61. */
    if (cpu.r[0] != 0061u) {
        rts_pc();
        return;
    }

    /* mov #400, @#1312. PSW.N/Z по 0o400, V=0, C unchanged. */
    bk_write_w(0001312u, 0000400u);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    /* 0o400 > 0, msb=0 → N=0; != 0 → Z=0. */

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_0f1a_anim_timer3 — таймер анимации для третьего объекта.
 * (0x0f1a..0x0f30, 24 байта, 5 инструкций.)
 *
 * Оригинал:
 *   cmp   #3, @#17374        ; если cell <= 3 → bge inc+rts
 *   bge   inc
 *   jsr   pc, @#R_14030      ; иначе R_14030 (рисователь)
 *   clr   @#17374             ; сброс таймера
 *   br    rts
 * inc:
 *   inc   @#17374
 * rts:
 *   rts   pc
 *
 * Отличается от R_07202/R_10102 (anim_tick): НЕ устанавливает
 * cpu.r[2]/r[4] перед вызовом R_14030. Это значит **caller** уже
 * подготовил их (передаёт через регистры).
 *
 * Вызывается из 2-х мест в зоне dispatcher'а — вероятно, анимация
 * какого-то третьего объекта (не E1/E2, может быть «падающий камень»
 * или подобный игровой элемент).
 * ------------------------------------------------------------------- */
void R_0f1a_anim_timer3(void)
{
    /* cmp #3, @#17374. */
    uint16_t cell = bk_read_w(0017374u);
    {
        uint16_t src = 3u;
        uint16_t r = (uint16_t)(src - cell);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
        if ((int16_t)r < 0) cpu.psw |= PSW_N;
        if (r == 0)          cpu.psw |= PSW_Z;
        if ((src ^ cell) & (src ^ r) & 0x8000u) cpu.psw |= PSW_V;
        if (src < cell)      cpu.psw |= PSW_C;
    }

    /* bge inc — берётся при 3 >= cell. */
    if ((int16_t)3 >= (int16_t)cell) {
        /* inc @#17374. */
        uint16_t nxt = (uint16_t)(cell + 1u);
        bk_write_w(0017374u, nxt);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)nxt < 0) cpu.psw |= PSW_N;
        if (nxt == 0)         cpu.psw |= PSW_Z;
        if (cell == 0x7FFFu)  cpu.psw |= PSW_V;
    } else {
        /* jsr pc, R_14030 — caller подготовил cpu.r[0/2/4]. */
        R_14030_body();
        /* clr @#17374. PSW: N=0, Z=1, V=0, C=0. */
        bk_write_w(0017374u, 0);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_V | PSW_C);
        cpu.psw |= PSW_Z;
    }

    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_01720_bel_dec_lives — звук гибели + декремент счётчика жизней.
 * (0x03d0..0x03d8, 8 байт, 3 инструкции.)
 *
 * Оригинал:
 *   jsr   pc, @#R_14d6       ; BEL звук
 *   dec   15470(pc)           ; --mem[pc + 0o15470] = --mem[0x1f10]
 *   rts   pc
 *
 * 15470(pc) = mem[PC + 0o15470 (= 0x1b38)]. PC после fetch'а X = 0x03d8,
 * так что эффективный адрес = 0x03d8 + 0x1b38 = 0x1f10 (= 0o17420).
 *
 * Это счётчик жизней/попыток («попыток N» на верхнем экране).
 * Вызывается из R_07f4 (которая в свою очередь вызывается R_12570 при
 * cmpb #4 → попадание в специальный тайл «убивающий»).
 *
 * Финал: cpu.r[0] = caller's r0 (восстановлен push/pop в R_14d6).
 * PSW от `dec`: N/Z/V по новому значению, C unchanged.
 * ------------------------------------------------------------------- */

/* Тело R_01720 без rts (для inline-использования в R_07f4). */
static void R_01720_body(void)
{
    /* === Inline R_14d6 (= push r0; r0=7; BEL; pop r0). === */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2u);
    bk_write_w(cpu.r[6], cpu.r[0]);
    cpu.r[0] = 7;
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
    audio_emit_bell();
    cpu.r[0] = bk_read_w(cpu.r[6]);
    cpu.r[6] = (uint16_t)(cpu.r[6] + 2u);
    psw_nz_w(cpu.r[0]);

    /* dec @ 0x1f10 (= 0o17420). */
    {
        uint16_t v = bk_read_w(0x1F10u);
        uint16_t nxt = (uint16_t)(v - 1u);
        bk_write_w(0x1F10u, nxt);
        cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V);
        if ((int16_t)nxt < 0) cpu.psw |= PSW_N;
        if (nxt == 0)         cpu.psw |= PSW_Z;
        if (v == 0x8000u)     cpu.psw |= PSW_V;
    }
}

void R_01720_bel_dec_lives(void)
{
    R_01720_body();
    rts_pc();
}

/* ---------------------------------------------------------------------
 * R_07f4_lose_one — гибель/штраф: вызов R_01720 + штраф к счётчику.
 * (0x07f4..0x07fe, 10 байт, 3 инструкции.)
 *
 * Оригинал:
 *   jsr   pc, R_01720       ; BEL + dec lives
 *   add   #12, @#17440      ; mem[0o17440] += 0o12 (= 10 dec)
 *   rts   pc
 *
 * 0o17440 = 0x1F20 — счётчик каких-то очков/штрафа (увеличивается на 10
 * при попадании в специальный тайл).
 *
 * Вызывается из R_12570 в ветке `cmpb #4, (r3)` (= попадание в
 * специальный тайл «убивающий»).
 *
 * PSW от `add`: N/Z по результату, V/C по переполнению.
 * ------------------------------------------------------------------- */
void R_07f4_lose_one(void)
{
    R_01720_body();

    /* add #12, @#17440. */
    uint16_t a = bk_read_w(0x1F20u);
    uint16_t src = 0012u;
    uint16_t r = (uint16_t)(a + src);
    bk_write_w(0x1F20u, r);
    cpu.psw &= (uint16_t)~(PSW_N | PSW_Z | PSW_V | PSW_C);
    if ((int16_t)r < 0) cpu.psw |= PSW_N;
    if (r == 0)          cpu.psw |= PSW_Z;
    if ((uint16_t)(~(a ^ src) & (a ^ r) & 0x8000u)) cpu.psw |= PSW_V;
    if (r < a)           cpu.psw |= PSW_C;

    rts_pc();
}

/* ---------------------------------------------------------------------
 * Таблица регистрации. Расширяется по мере декомпиляции новых процедур.
 * Линейный поиск — таблица маленькая (десятки записей в обозримом
 * будущем), бинарный поиск преждевременен.
 * ------------------------------------------------------------------- */
static const struct {
    uint16_t   pc;
    routine_fn fn;
} g_routines[] = {
    /* R_06602_enemy_e2_handler сюда НЕ регистрируется: 0x0d82 — это
     * не самостоятельная процедура, а внутренняя branch-метка более
     * крупного блока. JSR-вызовов на 0x0d82 в коде нет (только BR/BVC
     * с 0x0d04 и 0x0d7a). При вызове через диспетчер моя rts_pc снимает
     * со стека адрес возврата НЕ R_06602, а внешней охватывающей
     * процедуры — и игра ломается. Декомпиляция остаётся в файле как
     * рабочая разметка алгоритма ИИ, но не используется в продакшене.
     * AB-инфраструктура её тоже не покрывает корректно по той же
     * причине (snapshot/restore работают только для функций с честной
     * JSR/RTS-семантикой). */
    { 0006462u, R_06462_e2_teleport },
    { 0006632u, R_06632_main_e2 },
    { 0007202u, R_07202_anim_tick_e2_move },
    { 0007242u, R_07242_anim_tick_e2_idle },
    { 0007306u, R_07306_hero_teleport },
    { 0007462u, R_07462_main_e1 },
    { 0010102u, R_10102_anim_tick_e1_move },
    { 0010142u, R_10142_anim_tick_e1_idle },
    { 0014030u, R_14030_draw_compound_sprite },
    { 0014302u, R_14302_draw_glyph_8x8 },
    { 0014354u, R_14354_xor_sprite_8rows },
    /* R_0262_check_key1 (0o1142): НЕ регистрируется — это не само-
     * стоятельная процедура, а первое звено цепочки проверок клавиш
     * '1','2','3','4',... через `bne next`. Моя C-замена для r0 != '1'
     * делает RTS, а оригинал переходит на 0x0270 (= cmp '2'). Подмена
     * через диспетчер сломает обработку клавиш 2-9 в меню. Тело
     * оставлено в файле как разметка. */
    { 0001720u, R_01720_bel_dec_lives },  /* 0x03d0 */
    { 0003764u, R_07f4_lose_one },        /* 0x07f4 */
    { 0003652u, R_07aa_print_score },     /* 0x07aa */
    { 0004210u, R_0888_int_to_dec },      /* 0x0888 */
    { 0005106u, R_0a46_draw_8rows_v2 },   /* 0x0a46 */
    { 0007432u, R_0f1a_anim_timer3 },     /* 0x0f1a */
    { 0012442u, R_1522_draw_map },        /* 0x1522 */
    { 0012530u, R_1558_draw_8rows },      /* 0x1558 */
    { 0012326u, R_14d6_bel },             /* 0x14d6 */
    { 0012716u, R_15ce_clear_cell },      /* 0x15ce */
};


/* Опциональная инструментация: при env KLAD2_TRACE_C считаем, сколько
 * раз каждая C-замена сработала, и при выходе/HALT-е выводим сводку.
 * Если KLAD2_NO_C_ROUTINES — диспетчер всегда возвращает NULL (полезно
 * для A/B-проверки: тот же бинарник, тот же scenarist, но без замен). */
static unsigned g_hit_count[sizeof g_routines / sizeof g_routines[0]];
static int      g_disabled = -1;   /* -1 — ещё не читали env */
static int      g_trace_c  = -1;

static void check_env(void)
{
    if (g_disabled < 0) g_disabled = getenv("KLAD2_NO_C_ROUTINES") ? 1 : 0;
    if (g_trace_c  < 0) g_trace_c  = getenv("KLAD2_TRACE_C")       ? 1 : 0;
}

int routines_runtime_disabled = 0;

routine_fn routines_lookup(uint16_t pc)
{
    check_env();
    if (g_disabled) return NULL;
    if (routines_runtime_disabled) return NULL;
    for (size_t i = 0; i < sizeof g_routines / sizeof g_routines[0]; i++) {
        if (g_routines[i].pc == pc) {
            if (g_trace_c) g_hit_count[i]++;
            return g_routines[i].fn;
        }
    }
    return NULL;
}

__attribute__((destructor))
static void routines_print_stats(void)
{
    if (g_trace_c <= 0) return;
    fprintf(stderr, "C-замены, сводка вызовов:\n");
    for (size_t i = 0; i < sizeof g_routines / sizeof g_routines[0]; i++) {
        fprintf(stderr, "  %06o : %u\n",
                (unsigned)g_routines[i].pc, g_hit_count[i]);
    }
}
