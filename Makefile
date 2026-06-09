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

SRCS    := $(wildcard $(SRC_DIR)/*.c)
OBJS    := $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

BIN     := klad2

.PHONY: all run clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDLIBS)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD) $(BIN)

-include $(DEPS)
