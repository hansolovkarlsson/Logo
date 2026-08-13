#!/bin/bash
# One-shot alternative to `make` -- everything in a single gcc call, no
# incremental object files in build/.
#
# The flags here deliberately mirror the Makefile's rather than being a
# minimal "just compile it" line, because each one is load-bearing and
# this script previously drifted out of sync on two of them (it passed
# neither sdl2 nor -lm, so it had been failing at link since Phase 4).
# See the Makefile's own comments for the full reasoning:
#   -O1 + -fconserve-stack  keep exec_call/eval_logo's frames small
#                           enough that MAX_SCOPE_DEPTH's 200-level
#                           recursion cap is reachable without blowing
#                           the real stack (gcc-only flag, hence probed)
#   -std=gnu11              glibc hides open_memstream/usleep/
#                           clock_gettime under strict -std=c11
#   sdl2                    ui.c's joystick input
#   -lm                     libm is a separate DSO on Linux
set -e
mkdir -p bin

CONSERVE_STACK=""
if gcc -fconserve-stack -Werror -E -x c /dev/null >/dev/null 2>&1; then
    CONSERVE_STACK="-fconserve-stack"
fi

gcc -Wall -Wextra -g -O1 $CONSERVE_STACK -std=gnu11 \
    $(pkg-config --cflags gtk4 sdl2) \
    -o bin/logomotive src/*.c \
    $(pkg-config --libs gtk4 sdl2) -lm
