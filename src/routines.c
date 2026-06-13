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
#include "bk.h"
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
 * декомпилированных процедур (которые сами потом делают rts_pc). */
static void R_14030_body(void)
{
    /* Параметры функции:
     *   r0 — новый код кадра.
     *   r2 — база массива XOR-масок (приходит снаружи; используется для
     *        второй пары вызовов R_14354).
     *   r4 — указатель на anim-структуру.
     * Оригинал начинается с `mov r2, -(sp)` — это сохранение ВХОДНОГО r2,
     * чтобы `mov (sp), r2` на 0x1854 потом восстановил именно его (а не
     * `mem[r4+2]`, как казалось при первом разборе). */
    uint16_t xor_base = cpu.r[2];

    uint16_t anim    = cpu.r[4];
    uint16_t old_code = bk_read_w(anim);
    uint16_t char_ptr = bk_read_w((uint16_t)(anim + 2u));
    uint16_t phase    = bk_read_w((uint16_t)(anim + 4u));
    uint16_t x        = bk_read_w((uint16_t)(anim + 6u));
    uint16_t new_code = cpu.r[0];

    /* 1) Первый glyph по char_ptr/x. */
    inline_draw_glyph(char_ptr, x);

    /* 2) Считаем смещения по таблице для СТАРОГО кода. */
    uint16_t row_old = (uint16_t)(ANIM_TABLE_BASE + (unsigned)old_code * 4u);
    uint16_t dchar_old = bk_read_w((uint16_t)(row_old + 2u));
    uint16_t dx_old    = bk_read_w((uint16_t)(row_old + 4u));

    uint16_t r2 = (uint16_t)(char_ptr + dchar_old);
    uint16_t r1 = (uint16_t)(x        + dx_old);

    /* Условный «зеркальный» режим для первой половины фазы.
     * Оригинал: `cmp #024, 4(r4); ble skip_mirror` — ble берётся при
     *   `24 <= phase`, т.е. phase >= 24. Значит зеркалим когда phase < 24. */
    if (phase < ANIM_PHASE_THRESHOLD) {
        r2 = (uint16_t)(r2 - 2u * dchar_old);
        r1 = (uint16_t)(r1 - 2u * dx_old);
    }

    /* 3) Второй glyph. */
    inline_draw_glyph(r2, r1);

    /* 4) Обновление структуры по новому коду. */
    uint16_t row_new = (uint16_t)(ANIM_TABLE_BASE + (unsigned)new_code * 4u);
    uint16_t mask_new  = bk_read_w(row_new);
    uint16_t dchar_new = bk_read_w((uint16_t)(row_new + 2u));
    uint16_t dx_new    = bk_read_w((uint16_t)(row_new + 4u));

    if (new_code != old_code) {
        /* Сменили направление — фаза := 24 (готовимся к bit-тесту). */
        phase = ANIM_PHASE_THRESHOLD;
    } else if (phase <= ANIM_PHASE_THRESHOLD) {
        /* phase <= 24 → продолжаем счётчик (bge берётся, goto 0x188c).
         * Граница включающая: `cmp #024, 4(r4); bge` срабатывает при
         * 24 >= phase, т.е. phase <= 24. */
        phase = (uint16_t)(phase + ANIM_PHASE_STEP);
    } else {
        /* phase > 24 → bit-тест: если mask(new_code) пересекается со
         * словом по char_ptr, продвигаем анимацию (сброс фазы, шаг
         * char_ptr/X), иначе ничего не меняем. */
        uint16_t cur_word = bk_read_w(char_ptr);
        if (mask_new & cur_word) {
            phase = 0;
            char_ptr = (uint16_t)(char_ptr + dchar_new);
            x        = (uint16_t)(x        + dx_new);
            bk_write_w((uint16_t)(anim + 2u), char_ptr);
            bk_write_w((uint16_t)(anim + 6u), x);
        }
    }

    /* Сохраняем новый код и фазу. */
    bk_write_w(anim,                  new_code);
    bk_write_w((uint16_t)(anim + 4u), phase);

    /* 5) Указатель на XOR-маску для (new_code, phase).
     *    Оригинал: на 0x1854 делает `mov (sp), r2` — восстанавливает
     *    ВХОДНОЕ значение r2 (xor_base, сохранённое в самом начале на
     *    0x1818). Затем `add (r4), r2; add 4(r4), r2`, где (r4)=new_code
     *    (только что записан) и 4(r4)=обновлённая фаза. */
    uint16_t mask_ptr = (uint16_t)(xor_base + new_code + phase);
    uint16_t mask_x   = bk_read_w((uint16_t)(anim + 6u)); /* свежий X */

    /* 6) XOR-маска первая. */
    inline_xor_sprite(mask_ptr, mask_x);

    /* 7) Смещение для второй XOR-маски — зеркалим при phase < 24,
     * по той же логике что в шаге 4. */
    uint16_t mx2 = (uint16_t)(mask_x + dx_new);
    if (phase < ANIM_PHASE_THRESHOLD) {
        mx2 = (uint16_t)(mx2 - 2u * dx_new);
    }
    uint16_t mp2 = (uint16_t)(mask_ptr + XOR_MASK_STRIDE);

    /* 8) XOR-маска вторая. */
    inline_xor_sprite(mp2, mx2);
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
    cpu.r[2] = xor_base;
    cpu.r[4] = anim;

    int16_t cnt = byte_op
        ? (int16_t)(int8_t)bk_read_b(counter_addr)
        : (int16_t)bk_read_w(counter_addr);

    if (threshold >= cnt) {
        /* `bge` берётся → инкремент счётчика. */
        int16_t nxt = (int16_t)(cnt + 1);
        if (byte_op) bk_write_b(counter_addr, (uint8_t)nxt);
        else         bk_write_w(counter_addr, (uint16_t)nxt);
    } else {
        /* Иначе — нарисовать спрайт и сбросить счётчик. */
        R_14030_body();
        if (byte_op) bk_write_b(counter_addr, 0);
        else         bk_write_w(counter_addr, 0);
    }
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
    cpu.r[0] = 0010u;
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

void R_06602_enemy_e2_handler(void)
{
    uint16_t old_code;
    uint16_t cp_e2, cp_hero;
    int16_t  e2_idx, hero_idx;
    uint16_t cell, neighbour;

    /* 0d82-0d8a: init + if (*r5 == 8) goto idle.
     * Оригинал на 0d82-0d88 делает `mov #14440, r5; mov #14422, r4`,
     * а на 0d8a — `cmp #10, (r5)+` (autoinc) → r5 становится = 0o14442.
     * Эти значения регистров остаются актуальными во ВСЕХ возможных
     * RTS-выходах функции (никакая ветка их потом не перезаписывает на
     * что-то иное). Caller-код **может** опираться на них — поэтому
     * в C-замене обязательно эмулируем эти финальные значения cpu.r[4]
     * и cpu.r[5], даже если внутренней логике они не нужны. */
    cpu.r[5] = ANIM_E2_CHAR_PTR;   /* = 0o14442, post-autoinc */
    cpu.r[4] = ANIM_HERO_CP_PTR;   /* = 0o14422 (но позже перезаписывается на 0o14440 в каждой ветке) */

    old_code = bk_read_w(ANIM_E2);
    if (old_code == 0010u) goto L_idle;

    /* 0d90-0da0: горизонтальная позиция e2 и героя. */
    cp_e2   = bk_read_w(ANIM_E2_CHAR_PTR);
    cp_hero = bk_read_w(ANIM_HERO_CP_PTR);
    e2_idx   = (int16_t)((cp_e2   - MAP_BASE) & 077u);
    hero_idx = (int16_t)((cp_hero - MAP_BASE) & 077u);

    /* 0da4-0da8: cmp r0,r1; beq vertical; bgt try_left. */
    if (e2_idx == hero_idx) goto L_vertical;
    if (e2_idx >  hero_idx) goto L_try_left;

    /* === 0daa-0dc6: e2 слева от hero → пробуем идти вправо === */
    cp_e2 = bk_read_w(ANIM_E2_CHAR_PTR);
    cell = bk_read_w(cp_e2);
    if ((cell & 0100000u) == 0) goto L_vertical;       /* нет «можно вправо» */
    if (bk_read_b((uint16_t)(cp_e2 + 2u)) == 011)
        goto L_vertical;                                /* справа — лестница */
    neighbour = (uint16_t)(cp_e2 + 2u);
    if (neighbour == bk_read_w(HERO_TARGET_PTR))
        goto L_vertical;                                /* там стоит герой */
    cpu.r[0] = 0;                                       /* направление = вправо */
    /* fall through в main_tail */

L_main_tail:
    /* В этой точке оригинал на 0dca делает `mov #14440, r4` — что
     * соответствует выходному значению r4 = ANIM_E2 во всех RTS,
     * проходящих через main_tail. (Все остальные пути выходят раньше
     * — но **в каждом** в какой-то момент идёт `mov #14440, r4`, а
     * `mov #14440, r5` нет, поэтому r5 уже выставлен в начале.) */
    cpu.r[4] = ANIM_E2;
    /* 0dc6-0de6: jsr R_07202 + проверка bit 04000 + jsr R_07242 + RTS.
     * Используем `anim_tick` напрямую (не публичную обёртку
     * R_07202_anim_tick_e2_move), потому что та сама делает rts_pc —
     * у нас в R_06602 «JSR-пуша» не было, и rts снял бы лишнее со
     * стека. */
    anim_tick(0021760u, 0014440u, 0017366u, 1, 7);  /* = тело R_07202 */
    /* После R_07202 — caller перезаписывает r4 = 014440 (anim_e2_code).
     * `bit #4000, @2(r4)` = test bit 04000 в слове по mem[r4+2] =
     * char_ptr_e2. Если стоит — выход. */
    cp_e2 = bk_read_w(ANIM_E2_CHAR_PTR);
    if (bk_read_w(cp_e2) & 04000u) return;
    /* mov (r5), r1; add #100, r1; cmp r1, @#14432; beq exit */
    {
        uint16_t below = (uint16_t)(bk_read_w(ANIM_E2_CHAR_PTR) + 0100u);
        if (below == bk_read_w(HERO_TARGET_PTR)) return;
    }
    anim_tick(0021760u, 0014440u, 0017370u, 1, 4);  /* = тело R_07242 */
    return;

    /* === 0de8: вертикальное движение === */
L_vertical:
    cp_e2   = bk_read_w(ANIM_E2_CHAR_PTR);
    cp_hero = bk_read_w(ANIM_HERO_CP_PTR);
    /* sub #14550, r0; bic #77, r0 → r0 &= 0177700 (высокие биты =
     * номер строки × 0100). */
    e2_idx   = (int16_t)((cp_e2   - MAP_BASE) & 0177700u);
    hero_idx = (int16_t)((cp_hero - MAP_BASE) & 0177700u);

    if (e2_idx == hero_idx) goto L_main_tail;          /* same row */
    if (e2_idx >  hero_idx) goto L_try_up;

    /* e2 выше hero → пробуем идти вниз (r0=6) */
    cp_e2 = bk_read_w(ANIM_E2_CHAR_PTR);
    cell = bk_read_w(cp_e2);
    if ((cell & 010000u) == 0) goto L_main_tail;       /* нет «можно вниз» */
    {
        uint16_t below = (uint16_t)(cp_e2 + 0100u);
        if (below == bk_read_w(HERO_TARGET_PTR))
            goto L_main_tail;
    }
    cpu.r[0] = 6;
    anim_tick(0021760u, 0014440u, 0017366u, 1, 7);  /* = тело R_07202 */
    goto L_main_tail;

    /* === 0e1e: e2 справа от hero → пробуем идти влево (r0=2) === */
L_try_left:
    cp_e2 = bk_read_w(ANIM_E2_CHAR_PTR);
    cell = bk_read_w(cp_e2);
    if ((cell & 040000u) == 0) goto L_main_tail;
    /* cmpb #11, 0177776(r1) — байт по адресу r1-2 (слева от текущей). */
    if (bk_read_b((uint16_t)(cp_e2 - 2u)) == 011) goto L_main_tail;
    neighbour = (uint16_t)(cp_e2 - 2u);
    if (neighbour == bk_read_w(HERO_TARGET_PTR)) goto L_main_tail;
    cpu.r[0] = 2;
    anim_tick(0021760u, 0014440u, 0017366u, 1, 7);  /* = тело R_07202 */
    goto L_main_tail;

    /* === 0e42: пробуем вверх (r0=4) === */
L_try_up:
    cp_e2 = bk_read_w(ANIM_E2_CHAR_PTR);
    cell = bk_read_w(cp_e2);
    if ((cell & 020000u) == 0) goto L_main_tail;
    {
        uint16_t above = (uint16_t)(cp_e2 - 0100u);
        if (above == bk_read_w(HERO_TARGET_PTR)) goto L_main_tail;
    }
    cpu.r[0] = 4;
    anim_tick(0021760u, 0014440u, 0017366u, 1, 7);  /* = тело R_07202 */
    goto L_main_tail;

    /* === 0e5e: idle-секция === */
L_idle:
    {
        uint16_t below = (uint16_t)(bk_read_w(ANIM_E2_CHAR_PTR) + 0100u);
        if (below == bk_read_w(HERO_TARGET_PTR)) goto L_idle_set6;
        cpu.r[0] = 0010u;
        anim_tick(0021760u, 0014440u, 0017370u, 1, 4);  /* = тело R_07242 */
        /* bit #2000, @2(r4) — test bit 02000 в mem[char_ptr_e2]. Если
         * стоит — НЕ ставим code=6, выход. */
        cp_e2 = bk_read_w(ANIM_E2_CHAR_PTR);
        if (bk_read_w(cp_e2) & 02000u) return;
    }
L_idle_set6:
    /* mov #6, @#14440 — устанавливаем код направления = 6 (вниз). */
    bk_write_w(ANIM_E2, 6u);
    return;
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
    { 0007202u, R_07202_anim_tick_e2_move },
    { 0007242u, R_07242_anim_tick_e2_idle },
    { 0010102u, R_10102_anim_tick_e1_move },
    { 0010142u, R_10142_anim_tick_e1_idle },
    { 0014030u, R_14030_draw_compound_sprite },
    { 0014302u, R_14302_draw_glyph_8x8 },
    { 0014354u, R_14354_xor_sprite_8rows },
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
