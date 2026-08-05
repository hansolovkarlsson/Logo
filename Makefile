# Makefile for the GTK4 Logo interpreter

CC = gcc
TARGET = bin/logo
SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))

PKG_CONFIG = pkg-config
GTK_LIBS = gtk4

CFLAGS = -Wall -Wextra -g -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

TEST_TARGET = build/test_interpreter
TEST_SRC = tests/test_interpreter.c src/interpreter.c

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
	$(CC) $(CFLAGS) $(TEST_SRC) -o $(TEST_TARGET) $(LDFLAGS)

clean:
	rm -rf build bin
