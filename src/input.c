/*
 * Ввод с клавиатуры — заглушка Фазы 1.
 *
 * SDL-события превращаются в простейшие БК-коды и кладутся в
 * bk_mem[REG_KEY_DATA] / SYS_LAST_KEY, в REG_KEY_STATE поднимается бит
 * KEY_STATE_STATE. Без БК-прерывания клавиатуры — оно будет в Фазе 3,
 * когда появится интерпретатор и таблица векторов.
 *
 * Раскладка пока чисто символическая: латинские буквы и цифры
 * передаются как KOI-7-подобные коды; стрелки/Esc/Enter — общеупотребимые
 * БК-коды (Enter=0o12, Esc=0o33). После реверса соответствующего кода
 * КЛАД-2 (Фаза 5+) здесь будет настоящая раскладка.
 */

#include "input.h"
#include "bk.h"

/* Маппинг SDL-клавиш на биты порта параллельного интерфейса БК
 * (REG_PAR_INTERF = 0o177714). По разведке КЛАД-2 (PC=0o4724:
 *   mov @#177714, r3; ror r3; bcs found
 * ) порт **активно-низкий**: idle = 0xFF (все биты = 1 = «отпущено»),
 * нажатие сбрасывает соответствующий бит. КЛАД-2 при idle интерпрети-
 * рует первую попавшуюся «нажатую» клавишу из таблицы (UP) как «стоять
 * на месте», что и нужно когда никакая клавиша не нажата.
 *
 * (В digger-bk0010 порт активно-высокий — но там используется ДЖОЙСТИК
 * через другую схему. КЛАД-2 использует БК-клавиатуру через параллель-
 * ный порт, где логика инверсная.) */
static int par_bit_for_key(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_UP:     return PAR_INTERF_UP;
    case SDLK_RIGHT:  return PAR_INTERF_RIGHT;
    case SDLK_DOWN:   return PAR_INTERF_DOWN;
    case SDLK_LEFT:   return PAR_INTERF_LEFT;
    /* Shift/Ctrl (стрельба) идут через REG_KEY_DATA как РУС/ЛАТ
     * (коды 0o16/0o17), а не через PAR_INTERF. На реальной БК биты
     * LEFT_BUTTON/RIGHT_BUTTON в порту относятся к джойстику
     * «Электроника», который КЛАД-2 не использует. */
    default: return -1;
    }
}

int input_init(void)
{
    bk_mem[REG_KEY_STATE]     = 0;
    bk_mem[REG_KEY_STATE + 1] = 0;
    bk_mem[REG_KEY_DATA]      = 0;
    bk_mem[REG_KEY_DATA + 1]  = 0;
    bk_mem[SYS_LAST_KEY]      = 0;
    bk_mem[SYS_LAST_KEY_FLAG] = 0;
    /* Порт активно-высокий (откатано). */
    bk_mem[REG_PAR_INTERF]     = 0;
    bk_mem[REG_PAR_INTERF + 1] = 0;
    /* MAG_KEY (бит 6 в REG_EXT_DEV) активно-низкий: 1 = клавиша
     * НЕ нажата (исходное состояние БК). Без этого КЛАД-2 видит
     * «всегда нажата какая-то клавиша» и зацикливается на reading
     * REG_KEY_DATA = 0, интерпретируя как «продолжить движение». */
    bk_mem[REG_EXT_DEV] = (uint8_t)(1u << EXT_DEV_MAG_KEY);
    return 0;
}

/* Считаем, сколько SDL-клавиш сейчас удерживается — нужно для
 * корректного отслеживания состояния MAG_KEY. */
static int g_keys_held;

void input_shutdown(void) {}

static uint8_t sdl_to_bk_code(SDL_Keycode sym, uint16_t mod)
{
    (void)mod;
    /* Базовые управляющие. */
    switch (sym) {
    case SDLK_RETURN:    return 012; /* ВК */
    case SDLK_ESCAPE:    return 003; /* СТОП — соответствует БК-кодировке */
    case SDLK_BACKSPACE: return 030; /* ЗБ */
    /* Стрелки БК-0010 по таблице раскладки клавиатуры:
     *   ← (влево)  = 010
     *   ↑ (вверх)  = 032
     *   ↓ (вниз)   = 033
     *   → (вправо) = 031 */
    case SDLK_LEFT:      return 010;
    case SDLK_RIGHT:     return 031;
    case SDLK_UP:        return 032;
    case SDLK_DOWN:      return 033;
    case SDLK_SPACE:     return 040;
    /* РУС / ЛАТ — переключатели регистра БК; КЛАД-2 в меню правил
     * использует их как «стрельба влево» (РУС) и «стрельба вправо»
     * (ЛАТ). По соглашению ASCII control-codes РУС соответствует SO
     * (Shift Out, 0o16), ЛАТ — SI (Shift In, 0o17). Маплю на оба
     * Shift и оба Ctrl: чем больше клавиш, тем удобнее. */
    case SDLK_LSHIFT:    return 016; /* РУС → стрельба влево */
    case SDLK_RSHIFT:    return 017; /* ЛАТ → стрельба вправо */
    case SDLK_LCTRL:     return 016; /* дубль РУС */
    case SDLK_RCTRL:     return 017; /* дубль ЛАТ */
    default: break;
    }

    /* Печатные ASCII-символы пропустим как есть (плейсхолдер до реверса
     * настоящей раскладки в Фазе 5+). */
    if (sym >= 0x20 && sym < 0x7F) {
        return (uint8_t)sym;
    }
    return 0;
}

int input_handle_sdl_event(const SDL_Event *ev)
{
    if (ev->type != SDL_KEYDOWN && ev->type != SDL_KEYUP) return 0;

    SDL_Keycode sym = ev->key.keysym.sym;
    int pressed = (ev->type == SDL_KEYDOWN);

    /* 0. Счётчик удерживаемых клавиш + MAG_KEY-бит:
     *    - есть хоть одна нажатая → MAG_KEY = 0 (активно-низкий)
     *    - все отпущены           → MAG_KEY = 1
     *    Игнорируем SDL2-repeat — это всё та же удерживаемая клавиша. */
    if (pressed && !ev->key.repeat) g_keys_held++;
    else if (!pressed && g_keys_held > 0) g_keys_held--;

    if (g_keys_held > 0) bk_mem[REG_EXT_DEV] &= (uint8_t)~(1u << EXT_DEV_MAG_KEY);
    else                 bk_mem[REG_EXT_DEV] |= (uint8_t)(1u << EXT_DEV_MAG_KEY);

    /* 1. Порт активно-высокий: нажатие ставит бит, отпускание сбрасывает. */
    int bit = par_bit_for_key(sym);
    if (bit >= 0) {
        if (pressed) bk_mem[REG_PAR_INTERF] |= (uint8_t)(1u << bit);
        else         bk_mem[REG_PAR_INTERF] &= (uint8_t)~(1u << bit);
    }

    /* 2. Регистр клавиатуры (REG_KEY_DATA) и SYS_LAST_KEY: на БК-0010
     *    контроллер клавиатуры при нажатии защёлкивает скан-код в
     *    KEY_DATA и поднимает KEY_STATE.STATE. Чтение KEY_DATA
     *    аппаратно сбрасывает STATE (см. bk.c::on_read_key_data); сам
     *    код в KEY_DATA остаётся, пока не придёт следующая клавиша.
     *
     *    При KEYUP мы НЕ трогаем KEY_DATA и KEY_STATE — иначе КЛАД-2
     *    видел «пришла новая клавиша 0» и интерпретировал stale-код в
     *    своих буферах как «продолжать движение вправо». Это и было
     *    видимым багом «сполз с лестницы» (project_input_bug.md).
     *    Игнорируем SDL2 auto-repeat — в БК клавиатура не повторяет код
     *    без клавиши ПОВТ. */
    if (pressed && !ev->key.repeat) {
        uint8_t code = sdl_to_bk_code(sym, ev->key.keysym.mod);
        if (code) {
            bk_mem[REG_KEY_DATA]      = code;
            bk_mem[SYS_LAST_KEY]      = code;
            bk_mem[SYS_LAST_KEY_FLAG] = 1;
            bk_mem[REG_KEY_STATE]    |= (uint8_t)(1u << KEY_STATE_STATE);
        }
    }

    return 0;
}
