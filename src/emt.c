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
    BK_TEXT_FG   = 3, /* красный (в палитре по умолчанию) */
    BK_TEXT_BG   = 0  /* чёрный */
};

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
            unsigned c = ((fb >> p) & 1) ? BK_TEXT_FG : BK_TEXT_BG;
            b0 |= (uint8_t)(c << (p * 2));
        }
        /* Правые 4 пикселя — биты 4..7 → байт 1 экрана. */
        for (int p = 0; p < 4; p++) {
            unsigned c = ((fb >> (p + 4)) & 1) ? BK_TEXT_FG : BK_TEXT_BG;
            b1 |= (uint8_t)(c << (p * 2));
        }
        bk_mem[base + r * 64u + 0] = b0;
        bk_mem[base + r * 64u + 1] = b1;
    }
}

/* ---- EMT 016 — формирование символа / переключение режимов ------
 * R0 = код символа или управляющий код. Управляющие — частично; для
 * печатных глиф рисуется в текущей позиции курсора через шрифт БК.
 * Управляющие коды смены цвета 0o2xx пока логируются — добавим, когда
 * увидим, что именно КЛАД-2 шлёт. */
static void emt_016_putchar(void)
{
    unsigned ch = cpu.r[0] & 0xffu;
    switch (ch) {
    case 010: /* ВЛЕВО */
        if (g_cursor_x > 0) g_cursor_x--;
        return;
    case 012: /* ВК / перевод строки */
        g_cursor_x = 0;
        if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
        return;
    case 014: /* ОЧИСТКА ЭКРАНА */
        memset(&bk_mem[MEM_VIDEO], 0, 0x4000);
        g_cursor_x = g_cursor_y = 0;
        return;
    case 015: /* ВОЗВРАТ КАРЕТКИ */
        g_cursor_x = 0;
        return;
    case 031: /* ВПРАВО */
        g_cursor_x++;
        return;
    case 032: /* ВВЕРХ */
        if (g_cursor_y > 0) g_cursor_y--;
        return;
    case 027: /* ВНИЗ */
        if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
        return;
    }

    /* Печатный символ? */
    const uint8_t *glyph = bk_font_glyph((uint8_t)ch);
    if (glyph) {
        draw_glyph_color(g_cursor_x, g_cursor_y, (uint8_t)ch);
        g_cursor_x++;
        if (g_cursor_x >= BK_TEXT_COLS) {
            g_cursor_x = 0;
            if (g_cursor_y < BK_TEXT_ROWS - 1) g_cursor_y++;
        }
    } else {
        /* Управляющий код, который мы пока не понимаем (часто — смена
         * цвета / режима). Логируем без падения. */
        fprintf(stderr, "[EMT 016] управление %03o в (%u,%u)\n",
                ch, g_cursor_x, g_cursor_y);
    }
}

/* ---- EMT 020 — печать строки до нулевого символа ----------------
 * R1 = адрес начала строки. Реализуем «правильно» — обходим строку и
 * для каждого байта зовём ту же логику, что и EMT 016. */
static void emt_020_puts(void)
{
    uint16_t p = cpu.r[1];
    for (int n = 0; n < 256; n++) { /* предохранитель от бесконечной строки */
        uint8_t c = bk_read_b(p++);
        cpu.r[0] = (uint16_t)((cpu.r[0] & 0xff00u) | c);
        emt_016_putchar();
        if (c == 0) break;
    }
    cpu.r[1] = p;
}

/* ---- EMT 024 — установить курсор (R1=x, R2=y) ------------------- */
static void emt_024_set_cursor(void)
{
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
