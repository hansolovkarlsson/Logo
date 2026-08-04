#!/bin/bash
set -e
mkdir -p bin
gcc $(pkg-config --cflags gtk4) -o bin/logo src/main.c $(pkg-config --libs gtk4)
