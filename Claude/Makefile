# Makefile for GTK4 project

CC = gcc
TARGET = myapp
SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

# PKG_CONFIG = pkg-config
PKG_CONFIG = /opt/homebrew/bin/pkg-config
GTK_LIBS = gtk4

CFLAGS = -Wall -Wextra -g -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJ) $(TARGET)

