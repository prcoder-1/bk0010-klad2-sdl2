# Сборка Linux/SDL2-порта КЛАД-2.
#
# Подробности по фазам — в плане ~/.claude/plans/curried-stargazing-cloud.md
# Архитектура — в CLAUDE.md.

CC      ?= cc
CSTD    ?= -std=c11
WARN    ?= -Wall -Wextra -Wpedantic -Wno-unused-parameter
OPT     ?= -O2 -g
SDL2    := $(shell pkg-config --cflags --libs sdl2)

CFLAGS  := $(CSTD) $(WARN) $(OPT) $(shell pkg-config --cflags sdl2)
LDLIBS  := $(shell pkg-config --libs sdl2) -lm

SRC_DIR := src
BUILD   := build

# Все .c в src/, кроме файлов test_*.c (у них своя main()).
ALL_SRCS  := $(wildcard $(SRC_DIR)/*.c)
TEST_SRCS := $(wildcard $(SRC_DIR)/test_*.c)
APP_SRCS  := $(filter-out $(TEST_SRCS),$(ALL_SRCS))

APP_OBJS  := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(APP_SRCS))
TEST_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(TEST_SRCS))

DEPS      := $(APP_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

BIN       := klad2

# Объекты для тест-бинарника — только не-SDL2-модули.
TEST_DEPS := $(BUILD)/bk.o $(BUILD)/pdp11.o $(BUILD)/audio.o $(BUILD)/routines.o $(BUILD)/emt.o $(BUILD)/klad2_font.o $(BUILD)/klad2_mon_rom.o $(BUILD)/klad2_initial.o $(BUILD)/input.o

.PHONY: all run test clean screenshots verify

all: $(BIN)

$(BIN): $(APP_OBJS)
	$(CC) $(APP_OBJS) -o $@ $(LDLIBS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(BIN)
	./$(BIN)

# Бинарник тестов: один на каждый test_*.c. audio.o тащит за собой
# SDL2-зависимости (SDL_OpenAudioDevice и т.п.), а g_steps_per_frame
# живёт в main.c которого тут нет — оба эти случая закрываем стабом
# через -D test-флаг и -lSDL2 в линковке.
$(BUILD)/test_%: $(BUILD)/test_%.o $(TEST_DEPS)
	$(CC) $^ -o $@ -lm $(shell pkg-config --libs sdl2)

test: $(patsubst $(BUILD)/%.o,$(BUILD)/%,$(TEST_OBJS))
	@set -e; for t in $^; do echo "=== $$t ==="; ./$$t; done

clean:
	rm -rf $(BUILD) $(BIN)

# Полная AB-верификация всех C-замен на gameplay уровня 1.
# См. docs/verification.md для интерпретации результата.
verify: $(BIN)
	@echo "=== AB всех C-замен на gameplay уровня 1 (12 сек) ==="
	@KLAD2_AB=all KLAD2_AB_REGS=1 timeout -s INT 15 ./$(BIN) KLAD2.BIN \
	    --auto-keys "60:Enter,180:1" --speed 30000 2>&1 | \
	    awk '/AB-MEM/{m++} /AB-REGS/{r++} /AB-PSW/{p++} \
	         /AB:.*вызов/{print; total=$$2} \
	         END{printf "Расхождений: память=%d, регистры=%d, PSW=%d\n", \
	             m+0, r+0, p+0}'

# Воспроизвести три референсных скриншота через --auto-keys + дамп +
# конвертер. Полезно для регрессионной проверки после изменений.
SCREENS_DIR := docs/screens
screenshots: $(BIN)
	@mkdir -p $(SCREENS_DIR)
	@echo "=== Заставка ==="
	timeout -s INT 8 ./$(BIN) --max-steps 200000 \
	    --dump-memory /tmp/_title.bin >/dev/null 2>&1 || true
	./tools/bk_dump_to_ppm.py /tmp/_title.bin /tmp/_title.ppm
	convert /tmp/_title.ppm -scale 512x512 $(SCREENS_DIR)/title_screen.png
	@echo "=== Меню правил ==="
	timeout -s INT 10 ./$(BIN) --auto-keys "60:Enter" --max-steps 400000 \
	    --dump-memory /tmp/_menu.bin >/dev/null 2>&1 || true
	./tools/bk_dump_to_ppm.py /tmp/_menu.bin /tmp/_menu.ppm
	convert /tmp/_menu.ppm -scale 512x512 $(SCREENS_DIR)/menu_rules.png
	@echo "=== Уровень 1 ==="
	timeout -s INT 15 ./$(BIN) --auto-keys "60:Enter,180:1" \
	    --max-steps 800000 --dump-memory /tmp/_game.bin >/dev/null 2>&1 || true
	./tools/bk_dump_to_ppm.py /tmp/_game.bin /tmp/_game.ppm
	convert /tmp/_game.ppm -scale 512x512 $(SCREENS_DIR)/gameplay_level1.png
	@rm -f /tmp/_title.* /tmp/_menu.* /tmp/_game.*
	@echo "Скриншоты обновлены в $(SCREENS_DIR)/"

-include $(DEPS)
