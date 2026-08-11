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

# test_vm.c (Stage 2's own shadow-diff corpus, docs/BYTECODE_VM_DESIGN.md)
# needs the compiler+VM (compiler.c/bytecode.c/vm.c) alongside the same
# eval.c/parser.c/ast.c/lexer.c/interpreter.c stack TEST_SHADOW_DIFF_TARGET
# already links -- vm.c depends on interpreter.h/eval.h the same way
# eval.c itself does (see vm.h's own comment), so GTK_LIBS is needed here
# too.
TEST_VM_TARGET = build/test_vm
TEST_VM_SRC = tests/test_vm.c src/compiler.c src/bytecode.c src/vm.c src/eval.c src/parser.c src/ast.c src/lexer.c src/interpreter.c
TEST_VM_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
TEST_VM_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

# test_bytecode.c (Stage B of the bytecode save/load/assembler
# initiative, docs/ROADMAP.md) -- bytecode.c/compiler.c are both
# GTK/interpreter-free by design (see bytecode.h's own file comment and
# compiler.c's own), so this needs no pkg-config flags at all, same
# shape as TEST_PARSER_TARGET above.
TEST_BYTECODE_TARGET = build/test_bytecode
TEST_BYTECODE_SRC = tests/test_bytecode.c src/compiler.c src/bytecode.c src/parser.c src/ast.c src/lexer.c
TEST_BYTECODE_CFLAGS = -Wall -Wextra -g -O1 -std=c11

# test_agent.c (Phase 6's own first slice, docs/CONCURRENT_AGENTS_DESIGN.md)
# -- same stack as TEST_VM_TARGET plus agent.c itself. Fully headless
# (agent.c never touches GTK, by design -- see agent.h's own file
# comment), so no window/display needed here either.
TEST_AGENT_TARGET = build/test_agent
TEST_AGENT_SRC = tests/test_agent.c src/agent.c src/compiler.c src/bytecode.c src/vm.c src/eval.c src/parser.c src/ast.c src/lexer.c src/interpreter.c
TEST_AGENT_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
TEST_AGENT_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

# tools/logi_cli.c -- a standalone command-line driver for Stage 1's new
# evaluator (see docs/BYTECODE_VM_DESIGN.md), letting a script be run
# against it directly (or an interactive REPL started with no
# argument) instead of only through the test binaries above. "logi" for
# Logo Interactive. Lives outside src/ (its own main() would collide
# with main.c's if it were wildcarded into $(TARGET) like every other
# src/*.c file), so it needs its own explicit source list, same shape
# as TEST_EVAL_TARGET/TEST_SHADOW_DIFF_TARGET above (eval.c's own
# dependency on interpreter.h means GTK_LIBS and interpreter.c are
# needed here too). Not part of `all`/`test` -- opt-in via `make logi`,
# matching test-eval/test-shadow-diff's own opt-in targets.
LOGI_TARGET = bin/logi
LOGI_SRC = tools/logi_cli.c src/eval.c src/parser.c src/ast.c src/lexer.c src/interpreter.c
LOGI_CFLAGS = -Wall -Wextra -g -O1 -std=c11 $(shell $(PKG_CONFIG) --cflags $(GTK_LIBS))
LOGI_LDFLAGS = $(shell $(PKG_CONFIG) --libs $(GTK_LIBS))

.PHONY: all clean run test test-lexer test-parser test-eval test-shadow-diff test-vm test-bytecode test-agent logi

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
test: $(TEST_TARGET) $(TEST_LEXER_TARGET) $(TEST_PARSER_TARGET) $(TEST_EVAL_TARGET) $(TEST_SHADOW_DIFF_TARGET) $(TEST_VM_TARGET) $(TEST_BYTECODE_TARGET) $(TEST_AGENT_TARGET)
	./$(TEST_TARGET)
	./$(TEST_LEXER_TARGET)
	./$(TEST_PARSER_TARGET)
	./$(TEST_EVAL_TARGET)
	./$(TEST_SHADOW_DIFF_TARGET)
	./$(TEST_VM_TARGET)
	./$(TEST_BYTECODE_TARGET)
	./$(TEST_AGENT_TARGET)

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

test-vm: $(TEST_VM_TARGET)
	./$(TEST_VM_TARGET)

$(TEST_VM_TARGET): $(TEST_VM_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_VM_CFLAGS) $(TEST_VM_SRC) -o $(TEST_VM_TARGET) $(TEST_VM_LDFLAGS)

test-bytecode: $(TEST_BYTECODE_TARGET)
	./$(TEST_BYTECODE_TARGET)

$(TEST_BYTECODE_TARGET): $(TEST_BYTECODE_SRC) src/compiler.h src/bytecode.h src/parser.h src/ast.h src/lexer.h
	@mkdir -p build
	$(CC) $(TEST_BYTECODE_CFLAGS) $(TEST_BYTECODE_SRC) -o $(TEST_BYTECODE_TARGET)

test-agent: $(TEST_AGENT_TARGET)
	./$(TEST_AGENT_TARGET)

$(TEST_AGENT_TARGET): $(TEST_AGENT_SRC) $(HEADERS)
	@mkdir -p build
	$(CC) $(TEST_AGENT_CFLAGS) $(TEST_AGENT_SRC) -o $(TEST_AGENT_TARGET) $(TEST_AGENT_LDFLAGS)

logi: $(LOGI_TARGET)

$(LOGI_TARGET): $(LOGI_SRC) $(HEADERS)
	@mkdir -p bin
	$(CC) $(LOGI_CFLAGS) $(LOGI_SRC) -o $(LOGI_TARGET) $(LOGI_LDFLAGS)

clean:
	rm -rf build bin
