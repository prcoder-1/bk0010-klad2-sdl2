# Сборка Linux/SDL2-порта КЛАД-2.
#
# Описание порта — в README.md, A/B-верификация — в docs/verification.md.

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
TEST_DEPS := $(BUILD)/bk.o $(BUILD)/pdp11.o $(BUILD)/audio.o $(BUILD)/routines.o $(BUILD)/emt.o $(BUILD)/klad2_font.o $(BUILD)/klad2_mon_rom.o $(BUILD)/klad2_bin.o $(BUILD)/klad2_relocator.o $(BUILD)/input.o

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

# Картинки для README: один прогон без окна с записью кадров (--record)
# по сценарию DEMO_KEYS, из него — три скриншота и GIF геймплея. Номера
# кадров — при скорости по умолчанию (200 инстр/кадр). Нужен Pillow.
# Сценарий: Enter → правила, «1» → уровень, шаг вправо на лестницу, подъём,
# выстрел влево, проход по верхнему ярусу с кладами, падение в воду.
SCREENS_DIR := docs/screens
DEMO_KEYS   := 400:Enter,700:1,750:Right:30,790:Up:190,985:LShift,1030:Left:240
DEMO_FRAMES := 1520
DEMO_SHOTS  := title_screen:390 menu_rules:690 gameplay_level1:760
GIF_RANGE   := --from 720 --to $(DEMO_FRAMES) --step 3

screenshots: $(BIN)
	@mkdir -p $(SCREENS_DIR)
	@set -e; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	echo "=== Запись $(DEMO_FRAMES) кадров (~25 с) ==="; \
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./$(BIN) --record "$$tmp" \
	    --frames $(DEMO_FRAMES) --auto-keys "$(DEMO_KEYS)" >/dev/null 2>&1; \
	for s in $(DEMO_SHOTS); do \
	    ./tools/bk_dump_to_ppm.py "$$tmp/$$(printf %06d $${s#*:}).bin" \
	        $(SCREENS_DIR)/$${s%%:*}.png --scale 2; \
	done; \
	./tools/frames_to_gif.py "$$tmp" $(SCREENS_DIR)/gameplay.gif $(GIF_RANGE)
	@echo "Картинки обновлены в $(SCREENS_DIR)/"

-include $(DEPS)
