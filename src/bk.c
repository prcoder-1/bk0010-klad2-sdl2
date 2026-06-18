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
#include "input.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t bk_mem[BK_MEM_SIZE];

/* ----- Шрифт БК-Monitor 017 ------------------------------------------ *
 *
 * Извлечён в `src/klad2_font.c` как `klad2_font_data[2080]` (208 глифов
 * × 10 байт). По-умолчанию используется встроенный массив — внешний
 * ROM-файл не нужен. `bk_load_rom_font(path)` оставлен для совмести-
 * мости и для случаев, когда нужно подгрузить ДРУГОЙ шрифт. */
#define BK_FONT_ROM_OFFSET  0x141Eu
#define BK_FONT_GLYPH_SIZE  10u
#define BK_FONT_NUM_GLYPHS  208u

extern const uint8_t  klad2_font_data[];           /* в klad2_font.c */
extern const unsigned KLAD2_FONT_NUM_GLYPHS;
extern const unsigned KLAD2_FONT_GLYPH_SIZE;

/* g_font_ptr указывает либо на встроенный массив (по-умолчанию),
 * либо на g_font_loaded_buf (после bk_load_rom_font). */
static uint8_t        g_font_loaded_buf[BK_FONT_NUM_GLYPHS * BK_FONT_GLYPH_SIZE];
static const uint8_t *g_font_ptr = klad2_font_data;

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
    size_t n = fread(g_font_loaded_buf, 1, sizeof g_font_loaded_buf, f);
    fclose(f);
    if (n != sizeof g_font_loaded_buf) {
        fprintf(stderr,
                "bk_load_rom_font: прочитано %zu байт вместо %zu из '%s'\n",
                n, sizeof g_font_loaded_buf, path);
        return -1;
    }
    g_font_ptr = g_font_loaded_buf;
    return 0;
}

extern const uint8_t  klad2_mon_rom_data[];        /* в klad2_mon_rom.c */
extern const unsigned KLAD2_MON_ROM_SIZE;

/* Загружает встроенный 8KB образ Monitor-ROM в bk_mem[0o100000].
 * Вызывается из bk_reset(). Внешний path-вариант оставлен для
 * совместимости и подмены альтернативного ROM (см. --rom). */
void bk_load_embedded_rom(void)
{
    memcpy(&bk_mem[MEM_ROM_MON], klad2_mon_rom_data, KLAD2_MON_ROM_SIZE);
}

int bk_load_rom_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "bk_load_rom_image: не могу открыть '%s': %s\n",
                path, strerror(errno));
        return -1;
    }
    /* Образ Monitor-ПЗУ БК-0010 = 8 КиБ, грузится в bk_mem[0o100000]. */
    size_t n = fread(&bk_mem[MEM_ROM_MON], 1, 0x2000, f);
    fclose(f);
    if (n != 0x2000) {
        fprintf(stderr,
                "bk_load_rom_image: прочитано %zu байт из '%s' "
                "(ожидалось 8192)\n", n, path);
        return -1;
    }
    return 0;
}

const uint8_t *bk_font_glyph(uint8_t code)
{
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
    return &g_font_ptr[idx * BK_FONT_GLYPH_SIZE];
}

void bk_reset(void)
{
    memset(bk_mem, 0, sizeof bk_mem);
    /* Загрузить встроенный Monitor-ROM в bk_mem[0o100000..]. КЛАД-2
     * вызывает ROM-функции (звук BEL «плик») через прямые JSR на
     * адреса 0o102xxx. Без ROM игра запустится, но без этих звуков. */
    bk_load_embedded_rom();
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

extern struct { uint16_t r[8]; uint16_t psw; int halted; } cpu;
static void on_write_ext_dev(uint16_t value)
{
    if (getenv("KLAD2_EXTDEV_TRACE")) {
        static unsigned cnt;
        if (cnt < 30) {
            fprintf(stderr, "[EXT_DEV] PC=%06o value=%06o\n",
                    cpu.r[7], value);
            cnt++;
        }
    }
    /* На реальной БК любая запись в REG_EXT_DEV генерирует короткий
     * импульс (write-strobe) на пьезодинамике, независимо от значения
     * бита MAG_KEY. КЛАД-2 этим пользуется: пишет CLR @#0o177716 в
     * звуковых циклах, получая серию щелчков с заданной частотой.
     * Поэтому переключаем уровень на КАЖДУЮ запись, а не только при
     * смене бита 6 (исходный приближённый алгоритм). */
    static int toggle_state;
    toggle_state ^= 1;
    audio_speaker_set(toggle_state);

    /* Диагностика: лог любой первой и каждой 50-й записи + бит-стат. */
    if (getenv("KLAD2_AUDIO_LOG")) {
        static unsigned total;
        static unsigned nonzero_total;
        static unsigned bit_writes[8];
        for (int b = 0; b < 8; b++) {
            if (value & (1u << b)) bit_writes[b]++;
        }
        if (value != 0) nonzero_total++;

        if (total < 4 || (total < 100 && value != 0)
                      || total % 50 == 0) {
            fprintf(stderr,
                    "[EXT_DEV] #%u значение %06o (нонзеро всего: %u)\n",
                    total, value, nonzero_total);
        }
        if (total > 0 && total % 200 == 0) {
            fprintf(stderr,
                    "[EXT_DEV] бит-статистика (всего %u, не-нулей %u): "
                    "b0=%u b1=%u b2=%u b3=%u b4=%u b5=%u b6=%u b7=%u\n",
                    total, nonzero_total,
                    bit_writes[0], bit_writes[1], bit_writes[2],
                    bit_writes[3], bit_writes[4], bit_writes[5],
                    bit_writes[6], bit_writes[7]);
        }
        total++;
    }
}

/* ---------------- Чтение ---------------------------------------------- */

static void log_ext_dev_read(uint16_t addr)
{
    if (!getenv("KLAD2_AUDIO_LOG")) return;
    if ((addr & 0xFFFEu) != REG_EXT_DEV) return;
    static unsigned reads;
    reads++;
    if (reads < 4 || reads % 200 == 0) {
        fprintf(stderr, "[EXT_DEV] чтение #%u (значение %06o)\n",
                reads, bk_mem[REG_EXT_DEV] | (bk_mem[REG_EXT_DEV+1] << 8));
    }
}

/* На реальной БК-0010 чтение REG_KEY_DATA аппаратно сбрасывает бит
 * KEY_STATE_STATE регистра REG_KEY_STATE — это и есть acknowledge
 * принятой клавиши. Сам код в KEY_DATA остаётся, пока контроллер не
 * получит следующее нажатие. КЛАД-2 опирается именно на эту семантику:
 * обработчик читает KEY_DATA один раз, дальше крутит цикл по PAR_INTERF
 * и видит «нет новых клавиш» по очищенному STATE. */
static void on_read_key_data(uint16_t addr)
{
    if ((addr & 0xFFFEu) == REG_KEY_DATA) {
        bk_mem[REG_KEY_STATE] &= (uint8_t)~(1u << KEY_STATE_STATE);
        /* На реальной БК клавиатурный контроллер при отсутствии новой
         * клавиши возвращает 0 в KEY_DATA (либо обновляет байт scan'ом).
         * КЛАД-2 на 0x2ec читает KEY_DATA НЕ проверяя KEY_STATE.STATE
         * и ожидает что stale code очистится — иначе при stale '2'
         * (выбор уровня) герой каждый кадр получает «нет валидной
         * клавиши» → анимация падения = соскальзывание с лестницы.
         * Эмулируем: сбрасываем KEY_DATA после чтения, кодовый байт
         * восстановится при следующем KEYDOWN. */
        /* Сбрасываем в 0o377 (а не 0), потому что КЛАД-2 на 0x306
         * ищет код в таблице направлений 0x1732 через `cmp (r5)+, r0`
         * — там есть НУЛИ в позициях 2/3/6/7/8, и сброс в 0 даёт
         * ложное совпадение → герой получает «направление» 0o22
         * (движение вправо) без нажатия. 0o377 точно не встречается:
         * максимум в таблице 0o33 (↓), cmp #50 (= 50 dec) для 0o377
         * (= 255) даёт C=1 → bcc не сработает → loop без direction. */
        bk_mem[REG_KEY_DATA]     = 0377;
        bk_mem[REG_KEY_DATA + 1] = 0;
        /* Если пользователь УДЕРЖИВАЕТ клавишу, эмулируем scan-based
         * auto-repeat: восстанавливаем код в KEY_DATA и STATE сразу
         * после сброса. КЛАД-2 при следующем чтении снова получит код,
         * как будто кнопка нажалась повторно. При отпущенной клавише
         * KEY_DATA остаётся 0o377, КЛАД-2 видит «нет ввода». */
        input_repeat_held();
    }
}

uint8_t bk_read_b(uint16_t addr)
{
    log_ext_dev_read(addr);
    on_read_key_data(addr);
    return bk_mem[addr];
}

uint16_t bk_read_w(uint16_t addr)
{
    /* PDP-11 — little-endian; нечётное выравнивание словного доступа
     * округляем вниз (на железе был бы trap; контроль будет в Фазе 2). */
    addr &= 0xFFFEu;
    log_ext_dev_read(addr);
    on_read_key_data(addr);
    return (uint16_t)bk_mem[addr] | ((uint16_t)bk_mem[addr + 1] << 8);
}

/* ---------------- Запись ---------------------------------------------- */

bk_write_hook_fn bk_write_hook;


void bk_write_b(uint16_t addr, uint8_t value)
{
    bk_mem[addr] = value;
    if (bk_write_hook) bk_write_hook(addr, value, 0);

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
    if (bk_write_hook) bk_write_hook(addr, value, 1);

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
