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

int input_init(void)
{
    bk_mem[REG_KEY_STATE]     = 0;
    bk_mem[REG_KEY_STATE + 1] = 0;
    bk_mem[REG_KEY_DATA]      = 0;
    bk_mem[REG_KEY_DATA + 1]  = 0;
    bk_mem[SYS_LAST_KEY]      = 0;
    bk_mem[SYS_LAST_KEY_FLAG] = 0;
    return 0;
}

void input_shutdown(void) {}

static uint8_t sdl_to_bk_code(SDL_Keycode sym, uint16_t mod)
{
    (void)mod;
    /* Базовые управляющие. */
    switch (sym) {
    case SDLK_RETURN:    return 012; /* ВК */
    case SDLK_ESCAPE:    return 033; /* СТОП-эквивалент (не настоящий СТОП) */
    case SDLK_BACKSPACE: return 030; /* ЗБ */
    case SDLK_LEFT:      return 010;
    case SDLK_RIGHT:     return 031;
    case SDLK_UP:        return 032;
    case SDLK_DOWN:      return 027;
    case SDLK_SPACE:     return 040;
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
    if (ev->type != SDL_KEYDOWN) return 0;

    uint8_t code = sdl_to_bk_code(ev->key.keysym.sym, ev->key.keysym.mod);
    if (!code) return 0;

    bk_mem[REG_KEY_DATA]      = code;
    bk_mem[SYS_LAST_KEY]      = code;
    bk_mem[SYS_LAST_KEY_FLAG] = 1;
    bk_mem[REG_KEY_STATE]    |= (uint8_t)(1u << KEY_STATE_STATE);

    return 0;
}
