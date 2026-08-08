# Makefile for the GTK4 Logo interpreter

CC = gcc
TARGET = bin/logo
SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))

# sdl2: Phase 4's one genuinely new dependency, joystick/game-controller
# input (JOYSTICK?/JOYSTICKAXIS/JOYSTICKBUTTON? -- see ui.c). Only ui.c
# actually calls any SDL function (interpreter.c stays exactly as
# GTK/Cairo/SDL-free as it already was, reached only through a
# callback) -- kept out of the headless test build entirely, not just
# unused: linking it in there made AddressSanitizer runs of
# test_interpreter hang indefinitely (confirmed 2026-08-07), even though
# a plain (non-ASan) build was fine. TEST_LDFLAGS below stays GTK-only
# for exactly this reason, not merely to trim an unneeded library.
PKG_CONFIG = pkg-config
GTK_LIBS = gtk4
SDL_LIBS = sdl2

# -O1 matters, not just style: eval_logo (src/interpreter.c) is one
# giant function with every command as a branch, so at -O0 every
# branch's locals get their own permanent stack slot even though only
# one branch runs per call -- 200 levels of recursion (MAX_SCOPE_DEPTH)
# reliably overflowed the real stack well before that cap at -O0 (not
# just under AddressSanitizer's own separately-inflated overhead, which
# is independent of optimization level and still overflows here
# regardless). -O1 lets the compiler reuse/eliminate dead branches'
# stack slots, and empirically survives the full 200-level cap cleanly,
# repeatedly, direct invocation and not just through `make test`
# (confirmed 2026-08-07).
CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS) $(SDL_LIBS))
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS) $(SDL_LIBS))

TEST_TARGET = build/test_interpreter
TEST_SRC = tests/test_interpreter.c src/interpreter.c
TEST_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
TEST_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p bin
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

HEADERS = $(wildcard src/*.h)

build/%.o: src/%.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

# Headless tests for the interpreter core (src/interpreter.c) — no GTK
# widgets involved, so no window/display is needed to run these.
test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) $(TEST_SRC) -o $(TEST_TARGET) $(TEST_LDFLAGS)

clean:
	rm -rf build bin
