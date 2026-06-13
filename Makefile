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
TEST_DEPS := $(BUILD)/bk.o $(BUILD)/pdp11.o $(BUILD)/audio.o $(BUILD)/routines.o $(BUILD)/emt.o

.PHONY: all run test clean

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

-include $(DEPS)
