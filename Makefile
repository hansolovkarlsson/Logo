# Makefile for the GTK4 Logo interpreter

CC = gcc
TARGET = bin/logo
SRC = $(wildcard src/*.c)
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))

PKG_CONFIG = pkg-config
GTK_LIBS = gtk4

CFLAGS = -Wall -Wextra -g -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

.PHONY: all clean run

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

clean:
	rm -rf build bin
