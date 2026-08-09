// logo_new_cli.c
//
// Standalone command-line driver for Stage 1's new evaluator (see
// docs/BYTECODE_VM_DESIGN.md) -- runs a .logo script through
// logo_lex -> logo_parse -> ast_eval and prints its output to stdout.
// Deliberately lives outside src/ (whose Makefile target wildcards
// every src/*.c file wholesale into bin/logo): this file has its own
// main(), and living in src/ would collide with main.c's.
//
// Not wired into the real GTK app at all -- bin/logo still runs
// exclusively on eval_logo (see docs/BYTECODE_VM_DESIGN.md's own note
// on this); this is purely a way to try the new engine directly from
// the command line, against the same examples/*.logo scripts that
// already exercise eval_logo. Turtle motion/drawing commands still run
// (the shared LogoApp state updates normally), but there's no canvas
// to actually draw on here -- headless by design, same as the test
// binaries -- so only PRINT/error output is visible.
//
// Usage: bin/logo_new script.logo

#include "../src/eval.h"
#include "../src/interpreter.h"
#include "../src/lexer.h"
#include "../src/parser.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_CLI_TOKENS 8192

static void print_sink(LogoApp *app, const char *text) {
    (void)app;
    fputs(text, stdout);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s script.logo\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "%s: cannot open %s\n", argv[0], argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc((size_t)size + 1);
    size_t read = fread(source, 1, (size_t)size, f);
    source[read] = '\0';
    fclose(f);

    // Same "heap, not stack" rule this project applies to every
    // multi-KB-or-larger buffer (see LogoApp/ParseResult below) --
    // MAX_CLI_TOKENS * sizeof(LogoToken) is small enough to be safe on
    // the stack today, but there's no reason for this one-shot CLI to
    // be the exception to that habit.
    LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_CLI_TOKENS);
    int n = logo_lex(source, tokens, MAX_CLI_TOKENS);
    if (n < 0) {
        fprintf(stderr, "%s: script needs more than %d tokens\n", argv[0], MAX_CLI_TOKENS);
        free(tokens);
        free(source);
        return 1;
    }

    // ParseResult/LogoApp are heap-allocated, not stack locals -- both
    // are multi-MB (AstPool's fixed node array, LogoApp's list-node
    // pool), the same lesson recorded throughout this project's tests.
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        for (int i = 0; i < result->error_count; i++) {
            fprintf(stderr, "%s:%d:%d: %s\n", argv[1], result->errors[i].line, result->errors[i].col, result->errors[i].message);
        }
        free(result);
        free(tokens);
        free(source);
        return 1;
    }

    // Matches new_app() in tests/test_eval.c exactly -- the same
    // default state a script expects (turtle 0 at canvas center, pen
    // down implicitly via init_turtle, white background).
    LogoApp *app = calloc(1, sizeof(LogoApp));
    app->canvas_width = DEFAULT_CANVAS_WIDTH;
    app->canvas_height = DEFAULT_CANVAS_HEIGHT;
    init_turtle(app, &app->turtles[0]);
    app->turtle_count = 1;
    app->current_turtle = 0;
    app->bg_r = app->bg_g = app->bg_b = 1.0;
    app->output_sink = print_sink;

    ast_eval(app, &result->pool, result->program);

    free(app);
    free(result);
    free(tokens);
    free(source);
    return 0;
}
