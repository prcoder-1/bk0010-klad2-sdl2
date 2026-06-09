/*
 * Тесты интерпретатора КР1801ВМ1 (pdp11.c).
 *
 * Каждый тест собирает короткую инструкционную последовательность
 * вручную из 16-битных слов, кладёт её в bk_mem с типичного загрузочного
 * адреса (01000), вызывает pdp11_reset + pdp11_run_n и проверяет
 * регистры/память/PSW после остановки на HALT.
 *
 * Не нужны ни SDL2, ни bk_load_bin — только bk.o, pdp11.o, audio.o
 * (audio.o подтянут потому, что bk.c вызывает audio_speaker_set).
 *
 * Запуск:  make test
 */

#include "bk.h"
#include "pdp11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run    = 0;
static int tests_failed = 0;

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define CHECK_EQ(actual, expected) do {                                     \
    tests_run++;                                                            \
    unsigned long _a = (unsigned long)(actual);                             \
    unsigned long _e = (unsigned long)(expected);                           \
    if (_a != _e) {                                                         \
        tests_failed++;                                                     \
        fprintf(stderr,                                                     \
                "  FAIL %s:%d: ожидалось %s = %lo, получено %lo\n",         \
                __FILE__, __LINE__, #actual, _e, _a);                       \
    }                                                                       \
} while (0)

/* Уложить массив слов в bk_mem[addr..], записать туда же финальный HALT. */
static void load_program(uint16_t addr, const uint16_t *words, size_t n)
{
    bk_reset();
    for (size_t i = 0; i < n; i++) {
        bk_mem[addr + 2*i]     = (uint8_t)(words[i] & 0xff);
        bk_mem[addr + 2*i + 1] = (uint8_t)(words[i] >> 8);
    }
    /* HALT в конце, если автор теста его не положил. */
    if (n == 0 || words[n-1] != 0) {
        bk_mem[addr + 2*n]     = 0;
        bk_mem[addr + 2*n + 1] = 0;
    }
}

static void run_test(const char *name, uint16_t entry,
                     const uint16_t *prog, size_t n, void (*check)(void))
{
    fprintf(stderr, "  %s\n", name);
    load_program(entry, prog, n);
    pdp11_reset(entry, 0177000); /* стек где-нибудь в верхней RAM */
    int steps = pdp11_run_n(10000);
    if (!cpu.halted) {
        tests_failed++; tests_run++;
        fprintf(stderr, "  FAIL: не остановился за %d шагов\n", steps);
        return;
    }
    check();
}

/* ============== Тесты =================================================== */

/* MOV #5, R0 ; HALT  →  R0=5 */
static void check_mov_imm(void) { CHECK_EQ(cpu.r[0], 5); }
static const uint16_t prog_mov_imm[] = {
    0012700, 5,  /* MOV #5, R0  */
    0          /* HALT        */
};

/* MOV #3, R1 ; MOV R1, R0 ; HALT */
static void check_mov_reg(void) { CHECK_EQ(cpu.r[0], 3); CHECK_EQ(cpu.r[1], 3); }
static const uint16_t prog_mov_reg[] = {
    0012701, 3,    /* MOV #3, R1  */
    0010100,       /* MOV R1, R0  */
    0
};

/* CLR R0 ; HALT  →  R0=0, Z=1, N=0, V=0, C=0 */
static void check_clr(void) {
    CHECK_EQ(cpu.r[0], 0);
    CHECK_EQ((cpu.psw & PSW_Z) != 0, 1);
    CHECK_EQ((cpu.psw & PSW_N) != 0, 0);
    CHECK_EQ((cpu.psw & PSW_V) != 0, 0);
    CHECK_EQ((cpu.psw & PSW_C) != 0, 0);
}
static const uint16_t prog_clr[] = { 0012700, 0xffff, 0005000, 0 };

/* ADD #2, R0 (с R0=3)  →  R0=5 */
static void check_add(void) { CHECK_EQ(cpu.r[0], 5); }
static const uint16_t prog_add[] = {
    0012700, 3,       /* MOV #3, R0 */
    0062700, 2,       /* ADD #2, R0 */
    0
};

/* SUB: R0 = 10 - 3 = 7 */
static void check_sub(void) { CHECK_EQ(cpu.r[0], 7); }
static const uint16_t prog_sub[] = {
    0012700, 10,
    0012701, 3,
    0160100,          /* SUB R1, R0 */
    0
};

/* CMP R0, R0 при R0=0  →  Z=1 */
static void check_cmp_z(void) {
    CHECK_EQ((cpu.psw & PSW_Z) != 0, 1);
}
static const uint16_t prog_cmp_z[] = {
    0005000,          /* CLR R0   */
    0020000,          /* CMP R0, R0 */
    0
};

/* BNE: если не равно — прыжок. R0=1, CMP R0, #0 → Z=0 → BNE прыгает.
 * Раскладка с адресами:
 *   01000: MOV #1, R0          ; 2 слова
 *   01004: CMP R0, #0          ; 2 слова  → Z=0
 *   01010: BNE +3              ; 1 слово  → переход на 01020
 *   01012: MOV #0xbb, R1       ; 2 слова  — должно быть пропущено
 *   01016: BR  +2              ; 1 слово  → переход на 01024 (HALT)
 *   01020: MOV #0xaa, R1       ; 2 слова  — целевая ветка
 *   01024: HALT (добавляется загрузчиком) */
static void check_bne(void) { CHECK_EQ(cpu.r[1], 0xaa); }
static const uint16_t prog_bne[] = {
    0012700, 1,
    0022700, 0,
    0001003,          /* BNE +3 */
    0012701, 0xbb,
    0000402,          /* BR  +2 */
    0012701, 0xaa,
    0
};

/* SOB: цикл R5 раз, копит R0. R5=4, R0 += 1 каждый виток → R0=4. */
static void check_sob(void) { CHECK_EQ(cpu.r[0], 4); CHECK_EQ(cpu.r[5], 0); }
static const uint16_t prog_sob[] = {
    0012700, 0,       /* MOV #0, R0 */
    0012705, 4,       /* MOV #4, R5 */
    /* L: */
    0005200,          /* INC R0     */
    0077502,          /* SOB R5, L  (offset 2 слова назад) */
    0
};

/* JSR PC, sub ; в sub: MOV #0x55, R0 ; RTS PC.
 *   01000: JSR PC, @#01006     ; 2 слова
 *   01004: HALT                 ; 1 слово
 *   01006: MOV #0x55, R0        ; 2 слова
 *   01012: RTS PC               ; 1 слово */
static void check_jsr_rts(void) { CHECK_EQ(cpu.r[0], 0x55); }
static const uint16_t prog_jsr_rts[] = {
    0004737, 01006,
    0,
    0012700, 0x55,
    0000207
};

/* BIS / BIC: R0 = 0xF0; BIS #0x0F → 0xFF; BIC #0xF0 → 0x0F. */
static void check_bis_bic(void) { CHECK_EQ(cpu.r[0], 0x0F); }
static const uint16_t prog_bis_bic[] = {
    0012700, 0xf0,
    0052700, 0x0f,    /* BIS #0x0f, R0 */
    0042700, 0xf0,    /* BIC #0xf0, R0 */
    0
};

/* XOR (КР1801ВМ1): R0 ^= R1. R0=0xff, R1=0x0f → R0=0xf0. */
static void check_xor(void) { CHECK_EQ(cpu.r[0], 0xf0); }
static const uint16_t prog_xor[] = {
    0012700, 0xff,
    0012701, 0x0f,
    0074100,          /* XOR R1, R0  (070000 | reg=1<<6 | dst=0) */
    0
};

/* MOV через (R5)+ : пишем 3 слова в память, проверяем. */
static void check_movinc(void) {
    CHECK_EQ(bk_read_w(02000), 0x11);
    CHECK_EQ(bk_read_w(02002), 0x22);
    CHECK_EQ(bk_read_w(02004), 0x33);
    CHECK_EQ(cpu.r[5], 02006);
}
static const uint16_t prog_movinc[] = {
    0012705, 02000,   /* MOV #02000, R5 */
    0012725, 0x11,    /* MOV #0x11, (R5)+ */
    0012725, 0x22,
    0012725, 0x33,
    0
};

/* ASL R0 при R0=0x4000 → R0=0x8000; N=1 (бит 15), C=0 (бит 15 исходного),
 * V = N^C = 1, Z=0. */
static void check_asl(void) {
    CHECK_EQ(cpu.r[0], 0x8000);
    CHECK_EQ((cpu.psw & PSW_N) != 0, 1);
    CHECK_EQ((cpu.psw & PSW_V) != 0, 1);
    CHECK_EQ((cpu.psw & PSW_C) != 0, 0);
    CHECK_EQ((cpu.psw & PSW_Z) != 0, 0);
}
static const uint16_t prog_asl[] = {
    0012700, 0x4000,
    0006300,          /* ASL R0 */
    0
};

/* EMT — обработчик кладёт результат в R0. */
static int g_emt_seen;
static void emt_test_handler(unsigned no) {
    g_emt_seen = (int)no;
    cpu.r[0]   = 0xdead;
}
static void check_emt(void) {
    CHECK_EQ(g_emt_seen, 042);
    CHECK_EQ(cpu.r[0], 0xdead);
}
static const uint16_t prog_emt[] = {
    0104042,          /* EMT 042 */
    0
};

/* ============== Главный драйвер ========================================= */

typedef struct {
    const char       *name;
    const uint16_t   *prog;
    size_t            len;
    void            (*check)(void);
} test_t;

int main(void)
{
    static const test_t tests[] = {
        {"MOV #imm, Rn",                 prog_mov_imm, ARRAY_LEN(prog_mov_imm),  check_mov_imm},
        {"MOV Rsrc, Rdst",               prog_mov_reg, ARRAY_LEN(prog_mov_reg),  check_mov_reg},
        {"CLR + флаги",                  prog_clr,     ARRAY_LEN(prog_clr),      check_clr},
        {"ADD #imm, Rn",                 prog_add,     ARRAY_LEN(prog_add),      check_add},
        {"SUB Rsrc, Rdst",               prog_sub,     ARRAY_LEN(prog_sub),      check_sub},
        {"CMP — Z-флаг",                 prog_cmp_z,   ARRAY_LEN(prog_cmp_z),    check_cmp_z},
        {"BNE — переход по Z=0",         prog_bne,     ARRAY_LEN(prog_bne),      check_bne},
        {"SOB — счётчик в цикле",        prog_sob,     ARRAY_LEN(prog_sob),      check_sob},
        {"JSR PC + RTS PC",              prog_jsr_rts, ARRAY_LEN(prog_jsr_rts),  check_jsr_rts},
        {"BIS / BIC — биты",             prog_bis_bic, ARRAY_LEN(prog_bis_bic),  check_bis_bic},
        {"XOR — расширение КР1801ВМ1",   prog_xor,     ARRAY_LEN(prog_xor),      check_xor},
        {"MOV (R5)+ — авто-инкремент",   prog_movinc,  ARRAY_LEN(prog_movinc),   check_movinc},
        {"ASL — сдвиг и флаги",          prog_asl,     ARRAY_LEN(prog_asl),      check_asl},
        {"EMT — диспетч в обработчик",   prog_emt,     ARRAY_LEN(prog_emt),      check_emt},
    };

    pdp11_set_emt_handler(emt_test_handler);

    fprintf(stderr, "Тесты интерпретатора PDP-11:\n");
    for (size_t i = 0; i < ARRAY_LEN(tests); i++) {
        g_emt_seen = -1;
        run_test(tests[i].name, 01000,
                 tests[i].prog, tests[i].len, tests[i].check);
    }

    fprintf(stderr, "\nПроверок: %d, провалов: %d\n", tests_run, tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
