/*
 * Реализация EMT-вызовов БК-ROM, нужных КЛАД-2.
 *
 * Семантика каждой команды берётся из digger-bk0010/emt.h
 * (там доксиген описывает контракт «вход в Rn → выход в Rn / память»).
 * Здесь — те же эффекты на bk_mem и регистры виртуальной БК.
 *
 * Незнакомые номера логируются и пропускаются (продолжаем выполнение):
 * это удобно для разведки — видно, что игра следующим попросит.
 */

#include "emt.h"
#include "bk.h"
#include "pdp11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- состояние виртуального текстового драйвера ------------------- */
static unsigned g_cursor_x; /* колонка курсора (0..63 в моно, 0..31 в цвете) */
static unsigned g_cursor_y; /* строка курсора (0..23) */

/* ---- EMT 014 — инициализация драйверов системного ПЗУ ------------
 * digger emt.h:
 *   «Команда обеспечивает инициализацию всех драйверов системного ПЗУ,
 *   осуществляет сброс рабочих ячеек в исходное состояние, установку
 *   всех векторов прерываний, очистку экрана, установку исходных
 *   режимов отображения информации, очистку порта ввода-вывода,
 *   установку системного порта в исходное состояние, установку
 *   скорости обмена по линии 9600 бод. Стек в исходное состояние не
 *   устанавливается.»
 * Для нас критичны только видимые эффекты:
 *  - очистить видеоОЗУ (16 КиБ);
 *  - сбросить системные ячейки 040..057 (слово состояния дисплея);
 *  - сбросить SYS_LAST_KEY и его флаг.
 * Векторы прерываний наш интерпретатор не использует (EMT перехватываем
 * напрямую), поэтому их инициализация — no-op. */
static void emt_014_init_rom_drivers(void)
{
    memset(&bk_mem[MEM_VIDEO], 0, 0x4000);
    memset(&bk_mem[0040], 0, 020);
    bk_mem[SYS_LAST_KEY]      = 0;
    bk_mem[SYS_LAST_KEY_FLAG] = 0;
}

/* Раскладка экрана БК-0010 в цветном текстовом режиме (32 символа):
 *   - 32 колонки × 25 строк; каждая ячейка 8 пикс × 10 строк;
 *   - в каждом байте 4 пикселя (2 бита/пиксель), LSB-first;
 *   - значит одна строка глифа занимает 2 байта видеоОЗУ;
 *   - строка экрана = 64 байта, шаг строки символа = 64 × 10 = 640.
 *
 * Фон / цвет рисования — пока константа (передний = 3 «красный»,
 * задний = 0 «чёрный»). Управляющие коды смены цвета 0o2xx из
 * BK-ROM пока не интерпретируются; настоящая цветовая модель —
 * следующий шаг, когда поймём, как КЛАД-2 их шлёт. */
enum {
    BK_TEXT_COLS = 32,
    BK_TEXT_ROWS = 25,
    BK_TEXT_BG   = 0  /* чёрный (фон не меняется управляющими кодами) */
};

/* Текущий цвет переднего плана (0..3 — палитра БК).
 * Меняется управляющими кодами 0o221..0o224 (см. GRAF.mac в исходниках
 * БК-ROM-Бейсика): 221 — красный, 222 — зелёный, 223 — синий,
 * 224 — чёрный (невидимый на чёрном фоне; КЛАД-2 использует это для
 * скрытия первой копии «Баранов» на заставке). */
static unsigned g_text_fg = 3; /* по умолчанию красный */

static void draw_glyph_color(unsigned col, unsigned row, uint8_t code)
{
    const uint8_t *g = bk_font_glyph(code);
    if (!g) return;
    if (col >= BK_TEXT_COLS || row >= BK_TEXT_ROWS) return;

    uint16_t base = (uint16_t)(MEM_VIDEO + row * 10u * 64u + col * 2u);
    for (int r = 0; r < 10; r++) {
        uint8_t fb = g[r];
        uint8_t b0 = 0, b1 = 0;
        /* Левые 4 пикселя — биты 0..3 шрифта → байт 0 экрана. */
        for (int p = 0; p < 4; p++) {
            unsigned c = ((fb >> p) & 1) ? g_text_fg : BK_TEXT_BG;
            b0 |= (uint8_t)(c << (p * 2));
        }
        /* Правые 4 пикселя — биты 4..7 → байт 1 экрана. */
        for (int p = 0; p < 4; p++) {
            unsigned c = ((fb >> (p + 4)) & 1) ? g_text_fg : BK_TEXT_BG;
            b1 |= (uint8_t)(c << (p * 2));
        }
        bk_mem[base + r * 64u + 0] = b0;
        bk_mem[base + r * 64u + 1] = b1;
    }
}

/* ---- EMT 016 — формирование символа
 *
 * Реальный BK-Monitor 017 для большинства кодов 0..017 не делает ничего,
 * а CLR/LF/CR реализованы в BASIC-расширении. Но КЛАД-2 явно рассчитывает
 * на BASIC-семантику: шлёт 0o14 для очистки между сценами, 0o12 для
 * перевода строки и т.д. Без этой обработки экран не очищается между
 * заставкой и меню. Поэтому реализуем BASIC-расширенный набор:
 *   010      — backspace (двигаем курсор влево);
 *   012      — line feed (курсор вниз);
 *   014      — clear screen + home (как Form Feed);
 *   015      — carriage return (курсор в столбец 0);
 *   027/031/032 — БК-стрелки (вниз/вправо/вверх);
 *   020..0177, 0240..0377 — рисуем глиф;
 *   прочее   — no-op (логируется в KLAD2_EMT_LOG). */
static void emt_016_putchar(void)
{
    unsigned ch = cpu.r[0] & 0xffu;

    /* Управляющие коды БК-0010 по «Таблице 1. Коды символов»
     * (руководство пользователя БК). */
    switch (ch) {
    case 003: /* КТ — отказ от вводимой строки */                return;
    case 007: /* СУ/G — звонок (нет аудио в EMT 016)         */  return;
    case 010: /* ← курсор влево */
        if (g_cursor_x > 0) g_cursor_x--;
        return;
    case 012: /* ┘ ВК — Enter (CR+LF) */
        g_cursor_x = 0;
        if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
        return;
    case 014: /* СБР — очистка экрана + курсор домой */
        memset(&bk_mem[MEM_VIDEO], 0, 0x4000);
        g_cursor_x = g_cursor_y = 0;
        return;
    case 015: /* СУ/М — установка позиции табуляции (no-op) */    return;
    case 016: /* РУС — переключение знаконабора (no-op) */        return;
    case 017: /* ЛАТ — то же */                                   return;
    case 020: /* СУ/P — сброс позиции табуляции */                return;
    case 022: /* СУ/R — исходная установка курсора (cursor home) */
        g_cursor_x = g_cursor_y = 0;
        return;
    case 023: /* ВС — возврат строки (CR) */
        g_cursor_x = 0;
        return;
    case 024: /* СУ/Т — курсор на 8 позиций вправо */
        g_cursor_x = (g_cursor_x + 8 >= BK_TEXT_COLS)
                     ? BK_TEXT_COLS - 1 : g_cursor_x + 8;
        return;
    case 025: /* СУ/U — перевод курсора в начало следующей строки */
        g_cursor_x = 0;
        if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
        return;
    case 026: /* Сдвижка в строке (insert — пока no-op) */       return;
    case 027: /* Раздвижка в строке (push — пока no-op) */       return;
    case 030: /* Забой — удалить символ перед курсором */
        if (g_cursor_x > 0) g_cursor_x--;
        /* В чисто текстовом режиме надо было бы стирать; на нашем
         * пиксельном экране это бы потребовало рисовать пробел поверх,
         * чего КЛАД-2 в gameplay-фазе не делает — оставим только сдвиг. */
        return;
    case 031: /* → курсор вправо */
        if (g_cursor_x < BK_TEXT_COLS - 1) g_cursor_x++;
        return;
    case 032: /* ↑ курсор вверх */
        if (g_cursor_y > 0) g_cursor_y--;
        return;
    case 033: /* ↓ курсор вниз */
        if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
        return;
    case 034: case 035: case 036: case 037:
        /* Диагональные/прочие перемещения курсора — пока no-op. */
        return;
    /* Цвет переднего плана (GRAF.mac в исходниках БК-ROM-Бейсика,
     * подтверждено разделом 8.3.2.3 руководства БК-0010). */
    case 0221: case 0222: case 0223: case 0224: {
        static const char *names[] = {"чёрный","синий","зелёный","красный"};
        unsigned new_fg = (ch == 0221) ? 3 :
                          (ch == 0222) ? 2 :
                          (ch == 0223) ? 1 : 0;
        g_text_fg = new_fg;
        if (getenv("KLAD2_EMT_LOG")) {
            fprintf(stderr, "[EMT 016] цвет %03o → %s (fg=%u) в (%u,%u)\n",
                    ch, names[new_fg], new_fg, g_cursor_x, g_cursor_y);
        }
        return;
    }
    }

    const uint8_t *glyph = bk_font_glyph((uint8_t)ch);
    if (!glyph) {
        if (getenv("KLAD2_EMT_LOG")) {
            fprintf(stderr, "[EMT 016] нет глифа для %03o в (%u,%u)\n",
                    ch, g_cursor_x, g_cursor_y);
        }
        return;
    }

    if (getenv("KLAD2_EMT_LOG")) {
        char vis = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
        fprintf(stderr, "[EMT 016] печать %03o '%c' в (%u,%u)\n",
                ch, vis, g_cursor_x, g_cursor_y);
    }

    draw_glyph_color(g_cursor_x, g_cursor_y, (uint8_t)ch);
    g_cursor_x++;
    if (g_cursor_x >= BK_TEXT_COLS) {
        g_cursor_x = 0;
        if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
    }
}

/* ---- EMT 020 — печать строки --------------------------------------
 * R1 = адрес начала строки.
 * R2 = младший байт — макс. длина, старший — символ-ограничитель.
 *      Если R2=0 (вариант «до нулевого символа»), работаем как
 *      EMT_20 в digger/emt.h (clr r2; emt 020 — стопит на 0).
 *      Если R2≠0, это вариант EMT_20_l (digger/emt.h:143) — стопит
 *      по длине ИЛИ по символу-ограничителю.
 * КЛАД-2 кладёт в R2 ограничитель, после которого данные в его
 * строке-данных идут «для скроллинга / технические» — без R2-стопа
 * у нас рендерится hе и т.п. */
static void emt_020_puts(void)
{
    uint16_t p = cpu.r[1];
    uint16_t r2 = cpu.r[2];
    uint8_t  max_len = (uint8_t)(r2 & 0xffu);
    uint8_t  delim   = (uint8_t)((r2 >> 8) & 0xffu);
    int      use_len = (max_len != 0);
    int      use_dlm = (delim   != 0);

    if (getenv("KLAD2_EMT_LOG")) {
        fprintf(stderr,
                "[EMT 020] R1=%06o R2=%06o (max_len=%u delim=%03o)\n",
                p, r2, max_len, delim);
    }

    int n = 0;
    const int hard_limit = 512; /* предохранитель */
    while (n < hard_limit) {
        uint8_t c = bk_read_b(p++);
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0xff00u) | c);
        emt_016_putchar();
        n++;
        if (c == 0) break;                       /* всегда стопим на 0 */
        if (use_len && n >= max_len) break;      /* по длине */
        if (use_dlm && c == delim) break;        /* по ограничителю */
    }
    cpu.r[1] = p;
}

/* ---- EMT 024 — установить курсор (R1=x, R2=y) ------------------- */
static void emt_024_set_cursor(void)
{
    if (getenv("KLAD2_EMT_LOG")) {
        fprintf(stderr, "[EMT 024] курсор в (%u,%u)\n",
                cpu.r[1], cpu.r[2]);
    }
    g_cursor_x = cpu.r[1];
    g_cursor_y = cpu.r[2];
}

/* ---- EMT 006 — чтение кода клавиши с клавиатуры ----------------
 * Возвращает код в R0. Реальная БК-ROM блокирует до прихода кода;
 * у нас интерпретатор однопоточный, поэтому возвращаем 0, если в
 * REG_KEY_STATE нет готового байта (КЛАД-2 будет крутиться в своём
 * цикле опроса, а input.c наполнит REG_KEY_DATA/STATE при следующем
 * SDL-событии). */
static void emt_006_getchar(void)
{
    uint8_t state = bk_mem[REG_KEY_STATE];
    if (state & (1u << KEY_STATE_STATE)) {
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0xff00u) | bk_mem[REG_KEY_DATA]);
        /* сбросить бит готовности — символ «вычитан» */
        bk_mem[REG_KEY_STATE] = (uint8_t)(state & ~(1u << KEY_STATE_STATE));
    } else {
        cpu.r[0] = (uint16_t)(cpu.r[0] & 0xff00u);
    }
}

/* ---- Диспетчер -------------------------------------------------- */

void emt_dispatch(unsigned no)
{
    switch (no) {
    case 006: emt_006_getchar();           return;
    case 014: emt_014_init_rom_drivers();  return;
    case 016: emt_016_putchar();           return;
    case 020: emt_020_puts();              return;
    case 024: emt_024_set_cursor();        return;
    default:
        fprintf(stderr,
                "EMT %03o по PC=%06o (R0=%06o R1=%06o R2=%06o): "
                "не реализован, продолжаем\n",
                no, (unsigned)cpu.r[7],
                cpu.r[0], cpu.r[1], cpu.r[2]);
        return;
    }
}
