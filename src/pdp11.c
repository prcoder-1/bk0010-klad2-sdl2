/*
 * Интерпретатор КР1801ВМ1.
 *
 * Реализует базовый набор PDP-11 + расширения КР1801ВМ1 (SOB, XOR).
 * Декодер — иерархический switch по верхним битам слова инструкции;
 * адресация — единая функция resolve(), возвращающая «либо номер
 * регистра, либо эффективный адрес». Доступ к памяти всегда идёт через
 * bk_read_X / bk_write_X из bk.c — там же и перехват регистров периферии.
 *
 * Семантика флагов проверяется по «Архитектуре микропроцессорной системы
 * Электроника БК-0010» (соответствует стандартной PDP-11 — за вычетом
 * MFPI/MFPD/MTPI/MTPD и пары других «больших» инструкций, в БК-0010
 * отсутствующих).
 */

#include "pdp11.h"
#include "bk.h"

#include <stdio.h>

cpu_t cpu;

static emt_handler_fn g_emt_handler;
static emt_handler_fn g_trap_handler;
static int            g_trace;

/* ================== Управление и трасса =============================== */

void pdp11_set_emt_handler(emt_handler_fn fn)  { g_emt_handler = fn; }
void pdp11_set_trap_handler(emt_handler_fn fn) { g_trap_handler = fn; }
void pdp11_set_trace(int on)                   { g_trace = on; }

void pdp11_reset(uint16_t entry_pc, uint16_t stack_top)
{
    for (int i = 0; i < 8; i++) cpu.r[i] = 0;
    cpu.r[7] = entry_pc;
    cpu.r[6] = stack_top;
    cpu.psw  = 0;
    cpu.halted = 0;
}

/* ================== Работа с флагами PSW ============================== */

static inline void psw_set(uint16_t mask, int cond)
{
    if (cond) cpu.psw |= mask;
    else      cpu.psw &= (uint16_t)~mask;
}

static inline int psw(uint16_t mask)
{
    return (cpu.psw & mask) != 0;
}

static inline void set_nz_w(uint16_t v)
{
    psw_set(PSW_N, (v & 0x8000u) != 0);
    psw_set(PSW_Z, v == 0);
}

static inline void set_nz_b(uint8_t v)
{
    psw_set(PSW_N, (v & 0x80u) != 0);
    psw_set(PSW_Z, v == 0);
}

/* ================== Доступ к памяти процессора ======================== */

static inline uint16_t fetch_word(void)
{
    uint16_t w = bk_read_w(cpu.r[7]);
    cpu.r[7] = (uint16_t)(cpu.r[7] + 2);
    return w;
}

/* Распаковка 6-битного поля «mode|reg».
 * Возвращает 1, если операнд — это регистр (out_slot = его номер).
 * Возвращает 0, если операнд по адресу (out_slot = эффективный адрес).
 * step = 2 для словных операций, 1 для байтовых — но для R6/R7
 *        всегда 2 (стек и PC шагают словами).
 */
static int resolve(unsigned spec, int byte_op, unsigned *out_slot)
{
    unsigned mode = (spec >> 3) & 7u;
    unsigned reg  = spec & 7u;
    unsigned step = (byte_op && reg < 6) ? 1u : 2u;

    switch (mode) {
    case 0: /* R */
        *out_slot = reg;
        return 1;
    case 1: /* (R) */
        *out_slot = cpu.r[reg];
        return 0;
    case 2: /* (R)+ */
        *out_slot = cpu.r[reg];
        cpu.r[reg] = (uint16_t)(cpu.r[reg] + step);
        return 0;
    case 3: /* @(R)+ */
        *out_slot = bk_read_w(cpu.r[reg]);
        cpu.r[reg] = (uint16_t)(cpu.r[reg] + 2);
        return 0;
    case 4: /* -(R) */
        cpu.r[reg] = (uint16_t)(cpu.r[reg] - step);
        *out_slot = cpu.r[reg];
        return 0;
    case 5: /* @-(R) */
        cpu.r[reg] = (uint16_t)(cpu.r[reg] - 2);
        *out_slot = bk_read_w(cpu.r[reg]);
        return 0;
    case 6: { /* X(R) */
        uint16_t x = fetch_word();
        *out_slot = (uint16_t)(x + cpu.r[reg]);
        return 0;
    }
    case 7: { /* @X(R) */
        uint16_t x = fetch_word();
        *out_slot = bk_read_w((uint16_t)(x + cpu.r[reg]));
        return 0;
    }
    }
    *out_slot = 0;
    return 0;
}

static inline uint16_t read_op(int is_reg, unsigned slot, int byte_op)
{
    if (is_reg) {
        return byte_op ? (uint16_t)(cpu.r[slot] & 0xffu) : cpu.r[slot];
    }
    return byte_op ? (uint16_t)bk_read_b((uint16_t)slot)
                   : bk_read_w((uint16_t)slot);
}

/* Запись «как все»: байт в регистр трогает только младший байт.
 * Исключение — MOVB и MFPS, у них своя процедура (write_movb_to_dst). */
static inline void write_op(int is_reg, unsigned slot, uint16_t v, int byte_op)
{
    if (is_reg) {
        if (byte_op) cpu.r[slot] = (uint16_t)((cpu.r[slot] & 0xff00u)
                                              | (v & 0xffu));
        else         cpu.r[slot] = v;
    } else {
        if (byte_op) bk_write_b((uint16_t)slot, (uint8_t)v);
        else         bk_write_w((uint16_t)slot, v);
    }
}

/* MOVB в регистр: байт расширяется со знаком до 16 бит.
 * MOVB в память: пишется только байт. */
static inline void write_movb_to_dst(int is_reg, unsigned slot, uint8_t v)
{
    if (is_reg) cpu.r[slot] = (uint16_t)(int16_t)(int8_t)v;
    else        bk_write_b((uint16_t)slot, v);
}

/* ================== Команды: 2-операндные ============================= */

static void exec_two_op(uint16_t op)
{
    int byte_op   = (op & 0100000u) != 0;
    unsigned src  = (op >> 6) & 077u;
    unsigned dst  = op & 077u;
    unsigned opc  = (op >> 12) & 7u;

    unsigned ssrc, sdst;
    int src_is_reg = resolve(src, byte_op, &ssrc);
    uint16_t s_val = read_op(src_is_reg, ssrc, byte_op);
    int dst_is_reg = resolve(dst, byte_op, &sdst);

    switch (opc) {
    case 1: /* MOV / MOVB */
        if (byte_op) write_movb_to_dst(dst_is_reg, sdst, (uint8_t)s_val);
        else         write_op(dst_is_reg, sdst, s_val, 0);
        if (byte_op) set_nz_b((uint8_t)s_val); else set_nz_w(s_val);
        psw_set(PSW_V, 0);
        break;
    case 2: { /* CMP / CMPB — флаги как при src - dst */
        uint16_t d_val = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t a = (uint8_t)s_val, b = (uint8_t)d_val;
            uint8_t r = (uint8_t)(a - b);
            set_nz_b(r);
            psw_set(PSW_V, ((a ^ b) & (a ^ r) & 0x80u) != 0);
            psw_set(PSW_C, a < b);
        } else {
            uint16_t r = (uint16_t)(s_val - d_val);
            set_nz_w(r);
            psw_set(PSW_V, ((s_val ^ d_val) & (s_val ^ r) & 0x8000u) != 0);
            psw_set(PSW_C, s_val < d_val);
        }
        break;
    }
    case 3: { /* BIT / BITB — N/Z от src & dst */
        uint16_t d_val = read_op(dst_is_reg, sdst, byte_op);
        uint16_t r = (uint16_t)(s_val & d_val);
        if (byte_op) set_nz_b((uint8_t)r); else set_nz_w(r);
        psw_set(PSW_V, 0);
        break;
    }
    case 4: { /* BIC / BICB — dst &= ~src */
        uint16_t d_val = read_op(dst_is_reg, sdst, byte_op);
        uint16_t r = (uint16_t)(d_val & ~s_val);
        write_op(dst_is_reg, sdst, r, byte_op);
        if (byte_op) set_nz_b((uint8_t)r); else set_nz_w(r);
        psw_set(PSW_V, 0);
        break;
    }
    case 5: { /* BIS / BISB — dst |= src */
        uint16_t d_val = read_op(dst_is_reg, sdst, byte_op);
        uint16_t r = (uint16_t)(d_val | s_val);
        write_op(dst_is_reg, sdst, r, byte_op);
        if (byte_op) set_nz_b((uint8_t)r); else set_nz_w(r);
        psw_set(PSW_V, 0);
        break;
    }
    case 6: { /* ADD (word) / SUB (если byte_op — это уже опкод 16, см. ниже) */
        uint16_t d_val = read_op(dst_is_reg, sdst, 0);
        uint16_t r = (uint16_t)(s_val + d_val);
        write_op(dst_is_reg, sdst, r, 0);
        set_nz_w(r);
        psw_set(PSW_V, (~(s_val ^ d_val) & (s_val ^ r) & 0x8000u) != 0);
        psw_set(PSW_C, r < s_val);
        break;
    }
    default:
        fprintf(stderr, "pdp11: неизвестная 2-операндная инструкция %06o "
                "по адресу %06o\n", op, (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        break;
    }
}

/* SUB — отдельный опкод 16xxxx (1110_xxx_xxx_xxx_xxx), word-only. */
static void exec_sub(uint16_t op)
{
    unsigned src = (op >> 6) & 077u;
    unsigned dst = op & 077u;
    unsigned ssrc, sdst;
    int src_is_reg = resolve(src, 0, &ssrc);
    uint16_t s_val = read_op(src_is_reg, ssrc, 0);
    int dst_is_reg = resolve(dst, 0, &sdst);
    uint16_t d_val = read_op(dst_is_reg, sdst, 0);
    uint16_t r = (uint16_t)(d_val - s_val);
    write_op(dst_is_reg, sdst, r, 0);
    set_nz_w(r);
    psw_set(PSW_V, ((d_val ^ s_val) & (d_val ^ r) & 0x8000u) != 0);
    psw_set(PSW_C, d_val < s_val);
}

/* ================== Команды: 1-операндные ============================= */

static void exec_one_op(uint16_t op)
{
    int byte_op = (op & 0100000u) != 0;
    unsigned opc = (op >> 6) & 0777u; /* 9 бит: верхушка опкода */
    unsigned dst = op & 077u;

    unsigned sdst;
    int dst_is_reg = resolve(dst, byte_op, &sdst);
    uint16_t v;

    /* opc лежит в диапазоне 0050..0067 — это блок CLR..SXT,
     * для байтовых вариантов добавляется 01000 (бит 15). */
    unsigned sub = opc & 077u;

    switch (sub) {
    case 050: /* CLR(B) */
        write_op(dst_is_reg, sdst, 0, byte_op);
        psw_set(PSW_N, 0); psw_set(PSW_Z, 1);
        psw_set(PSW_V, 0); psw_set(PSW_C, 0);
        break;
    case 051: /* COM(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        v = byte_op ? (uint16_t)((~v) & 0xffu) : (uint16_t)~v;
        write_op(dst_is_reg, sdst, v, byte_op);
        if (byte_op) set_nz_b((uint8_t)v); else set_nz_w(v);
        psw_set(PSW_V, 0); psw_set(PSW_C, 1);
        break;
    case 052: /* INC(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t r = (uint8_t)(v + 1);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_V, v == 0x7fu);
        } else {
            uint16_t r = (uint16_t)(v + 1);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_V, v == 0x7fffu);
        }
        break;
    case 053: /* DEC(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t r = (uint8_t)(v - 1);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_V, v == 0x80u);
        } else {
            uint16_t r = (uint16_t)(v - 1);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_V, v == 0x8000u);
        }
        break;
    case 054: /* NEG(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t r = (uint8_t)(-(int8_t)v);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_V, r == 0x80u);
            psw_set(PSW_C, r != 0);
        } else {
            uint16_t r = (uint16_t)(-(int16_t)v);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_V, r == 0x8000u);
            psw_set(PSW_C, r != 0);
        }
        break;
    case 055: /* ADC(B) — добавить C */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t c = (uint8_t)psw(PSW_C);
            uint8_t r = (uint8_t)(v + c);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_V, v == 0x7fu && c);
            psw_set(PSW_C, v == 0xffu && c);
        } else {
            uint16_t c = (uint16_t)psw(PSW_C);
            uint16_t r = (uint16_t)(v + c);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_V, v == 0x7fffu && c);
            psw_set(PSW_C, v == 0xffffu && c);
        }
        break;
    case 056: /* SBC(B) — вычесть C */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t c = (uint8_t)psw(PSW_C);
            uint8_t r = (uint8_t)(v - c);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_V, v == 0x80u);
            psw_set(PSW_C, v == 0 && c);
        } else {
            uint16_t c = (uint16_t)psw(PSW_C);
            uint16_t r = (uint16_t)(v - c);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_V, v == 0x8000u);
            psw_set(PSW_C, v == 0 && c);
        }
        break;
    case 057: /* TST(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) set_nz_b((uint8_t)v); else set_nz_w(v);
        psw_set(PSW_V, 0); psw_set(PSW_C, 0);
        break;
    case 060: /* ROR(B) — циклический сдвиг через C */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t old = (uint8_t)v;
            uint8_t r = (uint8_t)((old >> 1) | (psw(PSW_C) ? 0x80u : 0u));
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_C, (old & 1) != 0);
        } else {
            uint16_t old = v;
            uint16_t r = (uint16_t)((old >> 1) | (psw(PSW_C) ? 0x8000u : 0u));
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_C, (old & 1) != 0);
        }
        psw_set(PSW_V, psw(PSW_N) ^ psw(PSW_C));
        break;
    case 061: /* ROL(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t old = (uint8_t)v;
            uint8_t r = (uint8_t)((old << 1) | (psw(PSW_C) ? 1u : 0u));
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_C, (old & 0x80u) != 0);
        } else {
            uint16_t old = v;
            uint16_t r = (uint16_t)((old << 1) | (psw(PSW_C) ? 1u : 0u));
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_C, (old & 0x8000u) != 0);
        }
        psw_set(PSW_V, psw(PSW_N) ^ psw(PSW_C));
        break;
    case 062: /* ASR(B) — арифметический сдвиг вправо */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t old = (uint8_t)v;
            uint8_t r = (uint8_t)(((int8_t)old) >> 1);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_C, (old & 1) != 0);
        } else {
            uint16_t old = v;
            uint16_t r = (uint16_t)(((int16_t)old) >> 1);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_C, (old & 1) != 0);
        }
        psw_set(PSW_V, psw(PSW_N) ^ psw(PSW_C));
        break;
    case 063: /* ASL(B) */
        v = read_op(dst_is_reg, sdst, byte_op);
        if (byte_op) {
            uint8_t old = (uint8_t)v;
            uint8_t r = (uint8_t)(old << 1);
            write_op(dst_is_reg, sdst, r, 1);
            set_nz_b(r);
            psw_set(PSW_C, (old & 0x80u) != 0);
        } else {
            uint16_t old = v;
            uint16_t r = (uint16_t)(old << 1);
            write_op(dst_is_reg, sdst, r, 0);
            set_nz_w(r);
            psw_set(PSW_C, (old & 0x8000u) != 0);
        }
        psw_set(PSW_V, psw(PSW_N) ^ psw(PSW_C));
        break;
    case 067: /* SXT — знакорасширение от N */
        v = psw(PSW_N) ? 0xffffu : 0u;
        write_op(dst_is_reg, sdst, v, 0);
        psw_set(PSW_Z, !psw(PSW_N));
        psw_set(PSW_V, 0);
        break;
    default:
        fprintf(stderr, "pdp11: неизвестная 1-операндная инструкция %06o "
                "по адресу %06o\n", op, (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        break;
    }
}

/* SWAB — обмен младшего и старшего байтов, флаги N/Z по младшему байту. */
static void exec_swab(uint16_t op)
{
    unsigned dst = op & 077u;
    unsigned sdst;
    int dst_is_reg = resolve(dst, 0, &sdst);
    uint16_t v = read_op(dst_is_reg, sdst, 0);
    uint16_t r = (uint16_t)((v >> 8) | (v << 8));
    write_op(dst_is_reg, sdst, r, 0);
    set_nz_b((uint8_t)(r & 0xffu));
    psw_set(PSW_V, 0);
    psw_set(PSW_C, 0);
}

/* ================== Команды: 1.5-операндные =========================== */

static void exec_mul_etc(uint16_t op)
{
    unsigned opc = (op >> 9) & 7u;   /* 070..074 */
    unsigned reg = (op >> 6) & 7u;
    unsigned src = op & 077u;
    unsigned ssrc;
    int s_is_reg = resolve(src, 0, &ssrc);
    uint16_t s_val = read_op(s_is_reg, ssrc, 0);

    switch (opc) {
    case 0: { /* MUL — R, R|1 <- R * src (32 бит знаковое) */
        int32_t prod = (int32_t)(int16_t)cpu.r[reg] * (int32_t)(int16_t)s_val;
        cpu.r[reg]       = (uint16_t)((uint32_t)prod >> 16);
        cpu.r[reg | 1u]  = (uint16_t)(prod & 0xffffu);
        set_nz_w((uint16_t)((uint32_t)prod >> 16));
        if (prod == 0) psw_set(PSW_Z, 1);
        if (prod < 0)  psw_set(PSW_N, 1); else psw_set(PSW_N, 0);
        psw_set(PSW_V, 0);
        psw_set(PSW_C, prod < -32768 || prod > 32767);
        break;
    }
    case 1: { /* DIV */
        if (s_val == 0) {
            psw_set(PSW_V, 1); psw_set(PSW_C, 1);
            return;
        }
        int32_t dividend = (int32_t)(((uint32_t)cpu.r[reg] << 16)
                                    | (uint32_t)cpu.r[reg | 1u]);
        int16_t divisor = (int16_t)s_val;
        int32_t quot = dividend / divisor;
        int32_t rem  = dividend % divisor;
        if (quot > 32767 || quot < -32768) {
            psw_set(PSW_V, 1);
        } else {
            cpu.r[reg]       = (uint16_t)quot;
            cpu.r[reg | 1u]  = (uint16_t)rem;
            set_nz_w((uint16_t)quot);
            psw_set(PSW_V, 0);
        }
        psw_set(PSW_C, 0);
        break;
    }
    case 2: { /* ASH — арифм. сдвиг R на src (младшие 6 бит знаковое) */
        int sh = (int)(int8_t)((s_val & 077u) | ((s_val & 040u) ? 0xc0 : 0));
        int16_t v0 = (int16_t)cpu.r[reg];
        int32_t r;
        if (sh >= 0) r = (int32_t)v0 << sh;
        else         r = (int32_t)v0 >> -sh;
        cpu.r[reg] = (uint16_t)r;
        set_nz_w((uint16_t)r);
        psw_set(PSW_V, ((int16_t)cpu.r[reg] < 0) != (v0 < 0));
        if (sh > 0)      psw_set(PSW_C, (v0 << (sh - 1)) & 0x8000u);
        else if (sh < 0) psw_set(PSW_C, (v0 >> (-sh - 1)) & 1u);
        else             psw_set(PSW_C, 0);
        break;
    }
    case 3: { /* ASHC — сдвиг {R, R|1} как 32-бит */
        int sh = (int)(int8_t)((s_val & 077u) | ((s_val & 040u) ? 0xc0 : 0));
        int32_t v0 = (int32_t)(((uint32_t)cpu.r[reg] << 16)
                              | (uint32_t)cpu.r[reg | 1u]);
        int64_t r;
        if (sh >= 0) r = (int64_t)v0 << sh;
        else         r = (int64_t)v0 >> -sh;
        cpu.r[reg]      = (uint16_t)((uint64_t)r >> 16);
        cpu.r[reg | 1u] = (uint16_t)(r & 0xffffu);
        set_nz_w((uint16_t)((uint64_t)r >> 16));
        psw_set(PSW_V, (r < 0) != (v0 < 0));
        psw_set(PSW_C, 0); /* грубо — игра, скорее всего, ASHC не использует */
        break;
    }
    case 4: { /* XOR — dst ^= R[reg]. У XOR в op поле «src» — это и есть
                 спецификация dst; s_val уже содержит старое значение dst,
                 второй resolve приведёт к двойному auto-inc/dec на режимах
                 2/4 — здесь его быть не должно. */
        uint16_t r = (uint16_t)(s_val ^ cpu.r[reg]);
        write_op(s_is_reg, ssrc, r, 0);
        set_nz_w(r);
        psw_set(PSW_V, 0);
        break;
    }
    default:
        fprintf(stderr, "pdp11: неизвестная 1.5-операндная инструкция %06o "
                "по адресу %06o\n", op, (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        break;
    }
}

/* SOB — Subtract One and Branch (КР1801ВМ1, тоже PDP-11/40+).
 * SOB Rn, off: --Rn; если Rn != 0, PC -= 2*off (off — 6 бит беззн.). */
static void exec_sob(uint16_t op)
{
    unsigned reg = (op >> 6) & 7u;
    unsigned off = op & 077u;
    cpu.r[reg] = (uint16_t)(cpu.r[reg] - 1);
    if (cpu.r[reg] != 0) {
        cpu.r[7] = (uint16_t)(cpu.r[7] - 2u * off);
    }
}

/* ================== Команды: ветвления ================================ */

static void do_branch(int taken, int8_t off)
{
    if (taken) cpu.r[7] = (uint16_t)(cpu.r[7] + 2 * (int16_t)off);
}

static void exec_branch(uint16_t op)
{
    int8_t off = (int8_t)(op & 0377u);
    unsigned cond = (op >> 8) & 0377u;

    switch (cond) {
    case 0001: do_branch(1, off);                              break; /* BR  */
    case 0002: do_branch(!psw(PSW_Z), off);                    break; /* BNE */
    case 0003: do_branch(psw(PSW_Z), off);                     break; /* BEQ */
    case 0004: do_branch(!(psw(PSW_N) ^ psw(PSW_V)), off);     break; /* BGE */
    case 0005: do_branch(psw(PSW_N) ^ psw(PSW_V), off);        break; /* BLT */
    case 0006: do_branch(!(psw(PSW_Z) || (psw(PSW_N) ^ psw(PSW_V))), off); break; /* BGT */
    case 0007: do_branch(psw(PSW_Z) || (psw(PSW_N) ^ psw(PSW_V)), off); break; /* BLE */
    case 0200: do_branch(!psw(PSW_N), off);                    break; /* BPL */
    case 0201: do_branch(psw(PSW_N), off);                     break; /* BMI */
    case 0202: do_branch(!(psw(PSW_C) || psw(PSW_Z)), off);    break; /* BHI */
    case 0203: do_branch(psw(PSW_C) || psw(PSW_Z), off);       break; /* BLOS*/
    case 0204: do_branch(!psw(PSW_V), off);                    break; /* BVC */
    case 0205: do_branch(psw(PSW_V), off);                     break; /* BVS */
    case 0206: do_branch(!psw(PSW_C), off);                    break; /* BCC */
    case 0207: do_branch(psw(PSW_C), off);                     break; /* BCS */
    default:
        fprintf(stderr, "pdp11: неизвестное ветвление %06o "
                "по адресу %06o\n", op, (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        break;
    }
}

/* ================== Команды: переходы и стек ========================== */

static void exec_jmp(uint16_t op)
{
    unsigned dst = op & 077u;
    unsigned mode = (dst >> 3) & 7u;
    /* JMP с mode 0 (регистр) — бессмыслица; на железе trap. Мы тормозим. */
    if (mode == 0) {
        fprintf(stderr, "pdp11: JMP с режимом 0 (на регистр) по %06o\n",
                (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        return;
    }
    unsigned sdst;
    resolve(dst, 0, &sdst);
    cpu.r[7] = (uint16_t)sdst;
}

static void exec_jsr(uint16_t op)
{
    unsigned reg = (op >> 6) & 7u;
    unsigned dst = op & 077u;
    unsigned mode = (dst >> 3) & 7u;
    if (mode == 0) {
        fprintf(stderr, "pdp11: JSR с режимом 0 (на регистр) по %06o\n",
                (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        return;
    }
    unsigned sdst;
    resolve(dst, 0, &sdst);
    /* push reg, R7=sdst, reg=PC_сохр */
    cpu.r[6] = (uint16_t)(cpu.r[6] - 2);
    bk_write_w(cpu.r[6], cpu.r[reg]);
    cpu.r[reg] = cpu.r[7];
    cpu.r[7]   = (uint16_t)sdst;
}

static void exec_rts(uint16_t op)
{
    unsigned reg = op & 7u;
    cpu.r[7] = cpu.r[reg];
    cpu.r[reg] = bk_read_w(cpu.r[6]);
    cpu.r[6] = (uint16_t)(cpu.r[6] + 2);
}

/* MARK — деаллокация фрейма (редко, оставим как nop с корректной семантикой). */
static void exec_mark(uint16_t op)
{
    unsigned n = op & 077u;
    cpu.r[6] = (uint16_t)(cpu.r[7] + 2u * n);
    cpu.r[7] = cpu.r[5];
    cpu.r[5] = bk_read_w(cpu.r[6]);
    cpu.r[6] = (uint16_t)(cpu.r[6] + 2);
}

/* ================== Команды: операции с кодом условий ================= */

static void exec_cc(uint16_t op)
{
    /* op в диапазоне 0240..0277. Бит 4 = set/clear, биты 0..3 = маска NZVC. */
    int set = (op & 0020u) != 0;
    uint16_t mask = 0;
    if (op & 0001u) mask |= PSW_C;
    if (op & 0002u) mask |= PSW_V;
    if (op & 0004u) mask |= PSW_Z;
    if (op & 0010u) mask |= PSW_N;
    if (set) cpu.psw |= mask;
    else     cpu.psw &= (uint16_t)~mask;
}

/* ================== Команды: RTI / RTT ================================ */

static void exec_rti(void)
{
    cpu.r[7] = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2);
    cpu.psw  = bk_read_w(cpu.r[6]); cpu.r[6] = (uint16_t)(cpu.r[6] + 2);
}

/* ================== Декодер ============================================ */

int pdp11_step(void)
{
    if (cpu.halted) return -1;

    if (g_trace) {
        fprintf(stderr,
                "PC=%06o op=%06o "
                "R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o "
                "SP=%06o PSW=%03o\n",
                cpu.r[7], bk_read_w(cpu.r[7]),
                cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5],
                cpu.r[6], cpu.psw);
    }

    uint16_t op = fetch_word();

    /* Ветвления: 0o000400..0o003777 (bit15=0) и 0o100000..0o103777 (bit15=1). */
    unsigned top4  = (op >> 12) & 017u;
    unsigned top7  = (op >> 9)  & 0177u;
    unsigned top8  = (op >> 8)  & 0377u;
    unsigned top10 = (op >> 6)  & 01777u;

    /* 2-операндные опкоды 1..6 (word) и 11..15 (byte) и 16 (SUB). */
    if (top4 >= 1 && top4 <= 6) { exec_two_op(op); return 0; }
    if (top4 >= 011 && top4 <= 015) { exec_two_op(op); return 0; }
    if (top4 == 016) { exec_sub(op); return 0; }

    /* 1.5-операндные: 070000..074777. */
    if (top7 >= 070 && top7 <= 074) { exec_mul_etc(op); return 0; }

    /* SOB: 077000..077777. */
    if (top7 == 077) { exec_sob(op); return 0; }

    /* Ветвления: top8 в {01..07, 0200..0207}. */
    if ((top8 >= 001 && top8 <= 007) ||
        (top8 >= 0200 && top8 <= 0207))
    {
        exec_branch(op); return 0;
    }

    /* EMT: 104000..104377 (top8 = 0o210). */
    if (top8 == 0210) {
        if (g_emt_handler) g_emt_handler(op & 0377u);
        else { fprintf(stderr, "pdp11: EMT %03o без обработчика\n",
                       op & 0377u); cpu.halted = 1; }
        return 0;
    }
    /* TRAP: 104400..104777 (top8 = 0o211). */
    if (top8 == 0211) {
        if (g_trap_handler) g_trap_handler(op & 0377u);
        else { fprintf(stderr, "pdp11: TRAP %03o без обработчика\n",
                       op & 0377u); cpu.halted = 1; }
        return 0;
    }

    /* JSR: 004xxx (bits 15..9 == 0o0040..0047 уже учли как top7=04). */
    if (top7 == 004) { exec_jsr(op); return 0; }

    /* 1-операндные: 0050xx..0067xx (word) и 1050xx..1067xx (byte). */
    {
        unsigned top9 = (op >> 6) & 01777u;
        unsigned base = top9 & 0777u;
        if ((base >= 0050 && base <= 0063) || base == 0067) {
            exec_one_op(op); return 0;
        }
    }

    /* JMP: 0001xx. */
    if (top10 == 0001) { exec_jmp(op); return 0; }

    /* SWAB: 0003xx. */
    if (top10 == 0003) { exec_swab(op); return 0; }

    /* RTS: 000020x. */
    if ((op & 0177770u) == 0000200u) { exec_rts(op); return 0; }

    /* MARK: 0064xx. */
    if (top10 == 0064) { exec_mark(op); return 0; }

    /* MFPS/MTPS: 1067xx (already routed to one_op as case 067 — для MFPS)
     * и 1064xx — MTPS. Для КЛАД-2 не используется, пока пропустим. */

    /* Прочие нульоперандные и CCC/SCC: opcode = 0240..0277. */
    if ((op & 0177760u) == 0000240u) { exec_cc(op); return 0; }

    switch (op) {
    case 0000000: cpu.halted = 1;                              return 0; /* HALT */
    case 0000001: /* WAIT */                                   return 0;
    case 0000002: exec_rti();                                  return 0; /* RTI */
    case 0000003: /* BPT */
        if (g_trap_handler) g_trap_handler(0);
        else { fprintf(stderr, "pdp11: BPT без обработчика\n"); cpu.halted = 1; }
        return 0;
    case 0000004: /* IOT — БК-0010 не использует, тормозим */
        fprintf(stderr, "pdp11: IOT без обработчика\n"); cpu.halted = 1; return 0;
    case 0000005: /* RESET — игнорируем */                     return 0;
    case 0000006: exec_rti();                                  return 0; /* RTT */
    case 0000240: /* NOP */                                    return 0;
    default:
        fprintf(stderr, "pdp11: неизвестная инструкция %06o "
                "по адресу %06o\n", op, (unsigned)cpu.r[7] - 2);
        cpu.halted = 1;
        return -1;
    }
}

int pdp11_run_n(int max_steps)
{
    int n = 0;
    while (n < max_steps && !cpu.halted) {
        if (pdp11_step() < 0) break;
        n++;
    }
    return n;
}
