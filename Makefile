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

# lexer.c (docs/BYTECODE_VM_DESIGN.md's Stage 1) has zero dependency on
# GTK/GLib/interpreter.h by design -- its own test binary needs no
# pkg-config flags at all, unlike every other target in this Makefile.
TEST_LEXER_TARGET = build/test_lexer
TEST_LEXER_SRC = tests/test_lexer.c src/lexer.c
TEST_LEXER_CFLAGS = -Wall -Wextra -g -O1 -std=c11

# ast.c/parser.c (Stage 1's AST + recursive-descent parser) are the
# same story -- no GTK/GLib/interpreter.h dependency, no pkg-config
# flags needed.
TEST_PARSER_TARGET = build/test_parser
TEST_PARSER_SRC = tests/test_parser.c src/parser.c src/ast.c src/lexer.c
TEST_PARSER_CFLAGS = -Wall -Wextra -g -O1 -std=c11

# eval.c (Stage 1's tree-walking evaluator) is different from
# lexer.c/ast.c/parser.c above: it deliberately DOES depend on
# interpreter.h, sharing LogoApp/turtle/variable state directly with
# eval_logo (see eval.h's own comment) -- so its test binary needs
# interpreter.c linked in and GTK_LIBS, same as TEST_TARGET above.
TEST_EVAL_TARGET = build/test_eval
TEST_EVAL_SRC = tests/test_eval.c src/eval.c src/parser.c src/ast.c src/lexer.c src/interpreter.c
TEST_EVAL_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
TEST_EVAL_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

# test_shadow_diff.c (the migration strategy from
# docs/BYTECODE_VM_DESIGN.md) needs both engines in one binary --
# eval_logo (interpreter.c) and the new lexer/parser/ast/eval stack --
# so it's the same shape as TEST_EVAL_TARGET above, just with its own
# source file.
TEST_SHADOW_DIFF_TARGET = build/test_shadow_diff
TEST_SHADOW_DIFF_SRC = tests/test_shadow_diff.c src/eval.c src/parser.c src/ast.c src/lexer.c src/interpreter.c
TEST_SHADOW_DIFF_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
TEST_SHADOW_DIFF_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

# tools/logo_new_cli.c -- a standalone command-line driver for Stage 1's
# new evaluator (see docs/BYTECODE_VM_DESIGN.md), letting a script be
# run against it directly instead of only through the test binaries
# above. Lives outside src/ (its own main() would collide with
# main.c's if it were wildcarded into $(TARGET) like every other
# src/*.c file), so it needs its own explicit source list, same shape
# as TEST_EVAL_TARGET/TEST_SHADOW_DIFF_TARGET above (eval.c's own
# dependency on interpreter.h means GTK_LIBS and interpreter.c are
# needed here too). Not part of `all`/`test` -- opt-in via `make
# logo-new`, matching test-eval/test-shadow-diff's own opt-in targets.
LOGO_NEW_CLI_TARGET = bin/logo_new
LOGO_NEW_CLI_SRC = tools/logo_new_cli.c src/eval.c src/parser.c src/ast.c src/lexer.c src/interpreter.c
LOGO_NEW_CLI_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
LOGO_NEW_CLI_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

.PHONY: all clean run test test-lexer test-parser test-eval test-shadow-diff logo-new

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
# widgets involved, so no window/display is needed to run these — plus
# lexer.c's, parser.c's, eval.c's, and the shadow-diff corpus's own
# tests, so `make test` remains the one command that verifies
# everything still works.
test: $(TEST_TARGET) $(TEST_LEXER_TARGET) $(TEST_PARSER_TARGET) $(TEST_EVAL_TARGET) $(TEST_SHADOW_DIFF_TARGET)
	./$(TEST_TARGET)
	./$(TEST_LEXER_TARGET)
	./$(TEST_PARSER_TARGET)
	./$(TEST_EVAL_TARGET)
	./$(TEST_SHADOW_DIFF_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_CFLAGS) $(TEST_SRC) -o $(TEST_TARGET) $(TEST_LDFLAGS)

test-lexer: $(TEST_LEXER_TARGET)
	./$(TEST_LEXER_TARGET)

$(TEST_LEXER_TARGET): $(TEST_LEXER_SRC) src/lexer.h
	@mkdir -p build
	$(CC) $(TEST_LEXER_CFLAGS) $(TEST_LEXER_SRC) -o $(TEST_LEXER_TARGET)

test-parser: $(TEST_PARSER_TARGET)
	./$(TEST_PARSER_TARGET)

$(TEST_PARSER_TARGET): $(TEST_PARSER_SRC) src/parser.h src/ast.h src/lexer.h
	@mkdir -p build
	$(CC) $(TEST_PARSER_CFLAGS) $(TEST_PARSER_SRC) -o $(TEST_PARSER_TARGET)

test-eval: $(TEST_EVAL_TARGET)
	./$(TEST_EVAL_TARGET)

$(TEST_EVAL_TARGET): $(TEST_EVAL_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_EVAL_CFLAGS) $(TEST_EVAL_SRC) -o $(TEST_EVAL_TARGET) $(TEST_EVAL_LDFLAGS)

test-shadow-diff: $(TEST_SHADOW_DIFF_TARGET)
	./$(TEST_SHADOW_DIFF_TARGET)

$(TEST_SHADOW_DIFF_TARGET): $(TEST_SHADOW_DIFF_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_SHADOW_DIFF_CFLAGS) $(TEST_SHADOW_DIFF_SRC) -o $(TEST_SHADOW_DIFF_TARGET) $(TEST_SHADOW_DIFF_LDFLAGS)

logo-new: $(LOGO_NEW_CLI_TARGET)

$(LOGO_NEW_CLI_TARGET): $(LOGO_NEW_CLI_SRC) $(HEADERS)
	@mkdir -p bin
	$(CC) $(LOGO_NEW_CLI_CFLAGS) $(LOGO_NEW_CLI_SRC) -o $(LOGO_NEW_CLI_TARGET) $(LOGO_NEW_CLI_LDFLAGS)

clean:
	rm -rf build bin
