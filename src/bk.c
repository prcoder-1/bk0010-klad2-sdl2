/*
 * Виртуальная БК-0010: память, перехват регистров, загрузка .BIN.
 *
 * bk_mem[] — единое хранилище и для обычного ОЗУ, и для значений
 * периферийных регистров. Перехватчики bk_read_X и bk_write_X
 * добавляют побочные эффекты при доступе к диапазону
 * MEM_REGS..MEM_END. Соглашение: интерпретатор PDP-11 (Фаза 2) и
 * EMT-обработчики ходят только через эти функции; прямая правка
 * bk_mem[reg_addr] допустима лишь в коде самих периферийных шимов
 * (input.c, audio.c) и на инициализации.
 */

#include "bk.h"
#include "audio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t bk_mem[BK_MEM_SIZE];

/* ----- Шрифт БК-Monitor 017 ------------------------------------------ */
/* Загружается из bk10_017_mon.rom при старте, см. bk_load_rom_font().  */
#define BK_FONT_ROM_OFFSET  0x141Eu
#define BK_FONT_GLYPH_SIZE  10u
/* Максимальный индекс глифа: для кода 0o377 → idx = 0o377 - 0o60 = 0o317 = 207.
 * Значит нужно 208 глифов × 10 байт = 2080 байт. */
#define BK_FONT_NUM_GLYPHS  208u
static uint8_t g_font[BK_FONT_NUM_GLYPHS * BK_FONT_GLYPH_SIZE];
static int     g_font_loaded;

int bk_load_rom_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bk_load_rom_font: не могу открыть '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fseek(f, BK_FONT_ROM_OFFSET, SEEK_SET) != 0) {
        fprintf(stderr, "bk_load_rom_font: fseek в '%s': %s\n",
                path, strerror(errno));
        fclose(f);
        return -1;
    }
    size_t n = fread(g_font, 1, sizeof g_font, f);
    fclose(f);
    if (n != sizeof g_font) {
        fprintf(stderr,
                "bk_load_rom_font: прочитано %zu байт вместо %zu из '%s'\n",
                n, sizeof g_font, path);
        return -1;
    }
    g_font_loaded = 1;
    return 0;
}

const uint8_t *bk_font_glyph(uint8_t code)
{
    if (!g_font_loaded) return NULL;
    /* Семантика из дизассемблера EMT 016 в BK-Monitor 017:
     *  - коды 0..017     — управляющие, глифа нет;
     *  - коды 020..0177  — печатные, индекс = code - 020;
     *  - коды 0200..0237 — управляющие, глифа нет;
     *  - коды 0240..0377 — печатные, индекс = code - 060. */
    int idx;
    if (code < 020)        return NULL;
    else if (code < 0200)  idx = code - 020;
    else if (code < 0240)  return NULL;
    else                   idx = code - 060;
    if ((unsigned)idx >= BK_FONT_NUM_GLYPHS) return NULL;
    return &g_font[idx * BK_FONT_GLYPH_SIZE];
}

void bk_reset(void)
{
    memset(bk_mem, 0, sizeof bk_mem);
    /* Регистр вертикального смещения после сброса БК = 0o330 — стандартное
     * значение, при котором экран начинается с начала видеоОЗУ.
     * (crt0.s в digger-bk0010 кладёт туда 0o1330 для смещённого скролла;
     *  у нас в эту фазу скролл-эффекты не моделируются.) */
    bk_mem[REG_V_SCROLL]     = 0330;
    bk_mem[REG_V_SCROLL + 1] = 0;
}

/* ---------------- Таймер БК ----------------------------------------- */

static uint32_t g_timer_accum;       /* счётчик «накопленных инструкций» */
static int      g_timer_logged_once; /* однократный лог факта работы */

void bk_tick_timer(unsigned cpu_steps)
{
    uint8_t csr = bk_mem[REG_TVE_CSR];
    if (!(csr & (1u << TVE_CSR_RUN))) {
        g_timer_accum = 0;
        return;
    }

    /* Делители частоты. */
    unsigned divider = 1;
    if (csr & (1u << TVE_CSR_D4))  divider *= 4;
    if (csr & (1u << TVE_CSR_D16)) divider *= 16;

    /* Один тик таймера ≈ 25 PDP-11-инструкций × делитель.
     * (3 МГц / 128 ≈ 23,4 кГц базовых тиков; ≈ 600 тыс. инструкций
     * в секунду — итого ≈ 25 инструкций на один такт без делителей.) */
    unsigned threshold = 25u * divider;

    if (!g_timer_logged_once && getenv("KLAD2_TIMER_LOG")) {
        fprintf(stderr,
                "[таймер] RUN установлен, CSR=%03o, LIMIT=%06o\n",
                csr, (unsigned)(bk_mem[REG_TVE_LIMIT]
                              | (bk_mem[REG_TVE_LIMIT + 1] << 8)));
        g_timer_logged_once = 1;
    }

    g_timer_accum += cpu_steps;
    while (g_timer_accum >= threshold) {
        g_timer_accum -= threshold;

        uint16_t cnt = (uint16_t)(bk_mem[REG_TVE_COUNT]
                                | (bk_mem[REG_TVE_COUNT + 1] << 8));
        if (cnt == 0) {
            /* Перезагрузка из LIMIT либо остановка в однократном режиме. */
            uint16_t lim = (uint16_t)(bk_mem[REG_TVE_LIMIT]
                                    | (bk_mem[REG_TVE_LIMIT + 1] << 8));
            bk_mem[REG_TVE_COUNT]     = (uint8_t)(lim & 0xff);
            bk_mem[REG_TVE_COUNT + 1] = (uint8_t)(lim >> 8);
            /* Поднять флаг события. */
            bk_mem[REG_TVE_CSR] = (uint8_t)(csr | (1u << TVE_CSR_FL));
            if (csr & (1u << TVE_CSR_OS)) {
                bk_mem[REG_TVE_CSR] &= (uint8_t)~(1u << TVE_CSR_RUN);
                return;
            }
        } else {
            cnt--;
            bk_mem[REG_TVE_COUNT]     = (uint8_t)(cnt & 0xff);
            bk_mem[REG_TVE_COUNT + 1] = (uint8_t)(cnt >> 8);
        }
    }
}

/* ---------------- Перехватчики доступа к регистрам -------------------- */

static void on_write_ext_dev(uint16_t value)
{
    /* Бит 6 (EXT_DEV_MAG_KEY) — пьезодинамик. */
    audio_speaker_set((value >> EXT_DEV_MAG_KEY) & 1);

    /* Диагностика: логируем 16 первых отличающихся записей и периодически
     * раз в секунду — какие биты меняются. Если КЛАД-2 для звука дёргает
     * не бит 6 — увидим разницу значений. */
    if (getenv("KLAD2_AUDIO_LOG")) {
        static uint16_t last_value = 0;
        static unsigned diff_count;
        if (value != last_value) {
            if (diff_count < 16) {
                fprintf(stderr,
                        "[EXT_DEV] запись %06o (бин %c%c%c%c %c%c%c%c)\n",
                        value,
                        (value & 0200) ? '1':'0',
                        (value & 0100) ? '1':'0',
                        (value & 0040) ? '1':'0',
                        (value & 0020) ? '1':'0',
                        (value & 0010) ? '1':'0',
                        (value & 0004) ? '1':'0',
                        (value & 0002) ? '1':'0',
                        (value & 0001) ? '1':'0');
            }
            diff_count++;
            last_value = value;
        }
    }
}

/* ---------------- Чтение ---------------------------------------------- */

uint8_t bk_read_b(uint16_t addr)
{
    /* Регистры периферии в Фазе 1 ведут себя как обычная RAM
     * (значения подкладывают input.c и др. напрямую). */
    return bk_mem[addr];
}

uint16_t bk_read_w(uint16_t addr)
{
    /* PDP-11 — little-endian; нечётное выравнивание словного доступа
     * округляем вниз (на железе был бы trap; контроль будет в Фазе 2). */
    addr &= 0xFFFEu;
    return (uint16_t)bk_mem[addr] | ((uint16_t)bk_mem[addr + 1] << 8);
}

/* ---------------- Запись ---------------------------------------------- */

void bk_write_b(uint16_t addr, uint8_t value)
{
    bk_mem[addr] = value;

    if (addr >= MEM_REGS) {
        switch (addr & 0xFFFEu) {
        case REG_EXT_DEV:
            /* Учитываем, что байтовая запись могла попасть в чётный или
             * нечётный байт; собираем актуальное слово и эмитим эффект. */
            on_write_ext_dev(bk_read_w(REG_EXT_DEV));
            break;
        default:
            break;
        }
    }
}

void bk_write_w(uint16_t addr, uint16_t value)
{
    addr &= 0xFFFEu;
    bk_mem[addr]     = (uint8_t)(value & 0xFFu);
    bk_mem[addr + 1] = (uint8_t)((value >> 8) & 0xFFu);

    if (addr >= MEM_REGS) {
        switch (addr) {
        case REG_EXT_DEV:
            on_write_ext_dev(value);
            break;
        case REG_TVE_CSR:
        case REG_TVE_LIMIT:
            if (getenv("KLAD2_TIMER_LOG")) {
                fprintf(stderr,
                        "[таймер] запись слово %06o → %s\n",
                        value,
                        addr == REG_TVE_CSR ? "CSR" : "LIMIT");
            }
            break;
        case REG_V_SCROLL:
            fprintf(stderr,
                    "[V_SCROLL] запись слово %06o (ADDRESS=%03o, "
                    "EXT_MEMORY=%u)\n",
                    value, value & 0xff, (value >> 9) & 1);
            break;
        default:
            break;
        }
    }
}

/* ---------------- Загрузка БК .BIN ------------------------------------ */

int bk_load_bin(const char *path, uint16_t *out_load_addr)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bk_load_bin: не могу открыть '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    uint8_t header[4];
    if (fread(header, 1, sizeof header, f) != sizeof header) {
        fprintf(stderr, "bk_load_bin: '%s' короче 4 байт\n", path);
        fclose(f);
        return -1;
    }

    uint16_t load_addr = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
    uint16_t length    = (uint16_t)header[2] | ((uint16_t)header[3] << 8);

    if ((uint32_t)load_addr + length > BK_MEM_SIZE) {
        fprintf(stderr,
                "bk_load_bin: образ '%s' (адрес 0o%o, длина %u) вылезает "
                "за пределы 64 КиБ\n", path, load_addr, length);
        fclose(f);
        return -1;
    }

    size_t n = fread(&bk_mem[load_addr], 1, length, f);
    fclose(f);
    if (n != length) {
        fprintf(stderr,
                "bk_load_bin: ожидалось %u байт образа, прочитано %zu\n",
                length, n);
        return -1;
    }

    if (out_load_addr) *out_load_addr = load_addr;
    return 0;
}
