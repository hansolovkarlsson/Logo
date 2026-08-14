// logi_cli.c
//
// Standalone command-line driver for Stage 1's new evaluator (see
// docs/BYTECODE_VM_DESIGN.md): `bin/logi script.logo` runs a script
// through logo_lex -> logo_parse -> ast_eval and prints its output;
// `bin/logi` with no argument instead starts an interactive REPL --
// "logi" for Logo Interactive (see run_repl below). Deliberately lives
// outside src/ (whose Makefile target wildcards every src/*.c file
// wholesale into bin/logomotive): this file has its own main(), and living
// in src/ would collide with main.c's.
//
// Not wired into the real GTK app at all -- bin/logomotive still runs
// exclusively on eval_logo (see docs/BYTECODE_VM_DESIGN.md's own note
// on this); this is purely a way to try the new engine directly,
// against the same examples/*.logo scripts that already exercise
// eval_logo. Turtle motion/drawing commands still run (the shared
// LogoApp state updates normally), but there's no canvas to actually
// draw on here -- headless by design, same as the test binaries -- so
// only PRINT/error output is visible.
//
// Usage: bin/logi [script.logo]

#include "../src/eval.h"
#include "../src/interpreter.h"
#include "../src/lexer.h"
#include "../src/parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLI_TOKENS 16384

static void print_sink(LogoApp *app, const char *text) {
    (void)app;
    fputs(text, stdout);
}

// Matches new_app() in tests/test_eval.c exactly -- the same default
// state a script expects (turtle 0 at canvas center, pen down
// implicitly via init_turtle, white background).
static LogoApp *make_app(void) {
    LogoApp *app = calloc(1, sizeof(LogoApp));
    app->canvas_width = DEFAULT_CANVAS_WIDTH;
    app->canvas_height = DEFAULT_CANVAS_HEIGHT;
    init_turtle(app, &app->turtles[0]);
    app->turtle_count = 1;
    app->current_turtle = 0;
    app->bg_r = app->bg_g = app->bg_b = 1.0;
    app->output_sink = print_sink;
    return app;
}

static int run_file(const char *prog, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "%s: cannot open %s\n", prog, path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc((size_t)size + 1);
    size_t read = fread(source, 1, (size_t)size, f);
    source[read] = '\0';
    fclose(f);
    logo_normalize_newlines(source); // see lexer.h

    // Same "heap, not stack" rule this project applies to every
    // multi-KB-or-larger buffer (see LogoApp/ParseResult below) --
    // MAX_CLI_TOKENS * sizeof(LogoToken) is small enough to be safe on
    // the stack today, but there's no reason for this one-shot CLI to
    // be the exception to that habit.
    LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_CLI_TOKENS);
    int n = logo_lex(source, tokens, MAX_CLI_TOKENS);
    if (n < 0) {
        fprintf(stderr, "%s: script needs more than %d tokens\n", prog, MAX_CLI_TOKENS);
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
            fprintf(stderr, "%s:%d:%d: %s\n", path, result->errors[i].line, result->errors[i].col, result->errors[i].message);
        }
        parse_result_destroy(result);
        free(tokens);
        free(source);
        return 1;
    }

    LogoApp *app = make_app();
    ast_eval(app, &result->pool, result->program);

    free(app);
    parse_result_destroy(result);
    free(tokens);
    free(source);
    return 0;
}

// --- REPL -------------------------------------------------------------
//
// A plain growable byte buffer -- used both for the whole session's
// source text so far and for the current not-yet-submitted input.
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} DynStr;

static void dynstr_init(DynStr *s) {
    s->cap = 256;
    s->data = malloc(s->cap);
    s->data[0] = '\0';
    s->len = 0;
}

static void dynstr_clear(DynStr *s) {
    s->len = 0;
    s->data[0] = '\0';
}

static void dynstr_append(DynStr *s, const char *text) {
    size_t add = strlen(text);
    if (s->len + add + 1 > s->cap) {
        while (s->len + add + 1 > s->cap) s->cap *= 2;
        s->data = realloc(s->data, s->cap);
    }
    memcpy(s->data + s->len, text, add + 1);
    s->len += add;
}

static void dynstr_free(DynStr *s) {
    free(s->data);
    s->data = NULL;
    s->len = s->cap = 0;
}

// True if `line` is exactly "bye" (case-insensitive, ignoring
// surrounding whitespace/the trailing newline) -- a REPL-only meta-
// command to exit, not a real Logo builtin: there's no BYE in
// interpreter.c/docs/LANGUAGE.md to port, so this is handled directly
// in run_repl below rather than added to parser.c/eval.c, and doesn't
// exist inside a script run via run_file.
static int line_is_bye(const char *line) {
    while (isspace((unsigned char)*line)) line++;
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) len--;
    return len == 3 && strncasecmp(line, "bye", 3) == 0;
}

// Scans one line, updating *bracket_depth (net '[' minus ']' count so
// far) and *to_open (whether an unclosed TO is pending) -- a simple,
// non-lexer heuristic (word-boundary matching for TO/END, raw bracket
// counting) good enough to know when a REPL "turn" -- one full
// statement or TO...END definition, however many physical lines it
// spans -- is complete and ready to submit. Not used for real parsing
// (logo_lex/logo_parse still do that, on the accumulated session
// text); this only decides when to stop prompting for continuation
// lines. Doesn't account for a literal [ or ] inside a 'raw text'/
// "quoted word (rare in practice, and interpreter.c's own
// extract_block has the same simplification).
static void scan_line_for_continuation(const char *line, int *bracket_depth, int *to_open) {
    const char *p = line;
    while (*p != '\0') {
        if (*p == '[') {
            (*bracket_depth)++;
            p++;
        } else if (*p == ']') {
            (*bracket_depth)--;
            p++;
        } else if (isalpha((unsigned char)*p)) {
            const char *start = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            size_t word_len = (size_t)(p - start);
            if (word_len == 2 && strncasecmp(start, "TO", 2) == 0) *to_open = 1;
            else if (word_len == 3 && strncasecmp(start, "END", 3) == 0) *to_open = 0;
        } else {
            p++;
        }
    }
}

// Interactive REPL: accumulates the whole session's source text (see
// DynStr session below) and re-lexes/re-parses it from scratch on
// every submitted turn, rather than trying to parse each turn in
// isolation. This is a real architectural constraint, not extra
// caution -- this engine's TO definitions are AST_PROC_DEF nodes
// living in whatever AstPool got parsed (see docs/BYTECODE_VM_DESIGN.md
// and the PROCEDURES milestone), with no separate persistent procedure
// table the way eval_logo's app->procedures[] gives the old engine;
// a procedure defined in an earlier turn is only callable from a later
// one if that later turn's own parse still contains its AST_PROC_DEF,
// which means re-parsing the whole growing session text every time.
// Only the newly added tail statements are actually run each turn
// (via ast_eval_from), so earlier turns' PRINT/turtle-motion side
// effects don't repeat -- variables/turtle state persist naturally
// anyway, since `app` itself is one long-lived object across turns.
// A turn that fails to parse is never folded into the session buffer,
// so a mistyped line doesn't haunt every later turn's diagnostics.
static void run_repl(void) {
    LogoApp *app = make_app();
    DynStr session, pending;
    dynstr_init(&session);
    dynstr_init(&pending);
    int bracket_depth = 0;
    int to_open = 0;
    int session_child_count = 0;

    printf("Logo (Stage 1 new evaluator) REPL -- type BYE or Ctrl+D to quit\n");

    char line[4096];
    for (;;) {
        printf(pending.len == 0 ? "? " : "  ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }

        if (pending.len == 0 && line_is_bye(line)) {
            printf("Bye!\n");
            break;
        }

        scan_line_for_continuation(line, &bracket_depth, &to_open);
        dynstr_append(&pending, line);

        if (bracket_depth < 0) {
            fprintf(stderr, "logi: unmatched ]\n");
            bracket_depth = 0;
            dynstr_clear(&pending);
            continue;
        }
        if (bracket_depth > 0 || to_open) continue; // still accumulating a block

        int has_content = 0;
        for (size_t i = 0; i < pending.len; i++) {
            if (!isspace((unsigned char)pending.data[i])) { has_content = 1; break; }
        }
        if (!has_content) {
            dynstr_clear(&pending);
            continue;
        }

        DynStr candidate;
        dynstr_init(&candidate);
        dynstr_append(&candidate, session.data);
        dynstr_append(&candidate, pending.data);

        LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_CLI_TOKENS);
        int n = logo_lex(candidate.data, tokens, MAX_CLI_TOKENS);
        if (n < 0) {
            fprintf(stderr, "logi: session grew too large for this REPL -- restart it\n");
        } else {
            ParseResult *result = calloc(1, sizeof(ParseResult));
            logo_parse(tokens, n, result);
            if (result->error_count > 0) {
                for (int i = 0; i < result->error_count; i++) {
                    fprintf(stderr, "%d:%d: %s\n", result->errors[i].line, result->errors[i].col, result->errors[i].message);
                }
            } else {
                AstNode *program_node = &result->pool.nodes[result->program];
                int start = program_node->first_child;
                for (int i = 0; i < session_child_count && start >= 0; i++) {
                    start = result->pool.nodes[start].next_sibling;
                }
                ast_eval_from(app, &result->pool, start);

                int total = 0;
                for (int c = program_node->first_child; c >= 0; c = result->pool.nodes[c].next_sibling) total++;
                session_child_count = total;

                dynstr_free(&session);
                session = candidate;
                candidate.data = NULL; // ownership moved into session; don't double-free below
            }
            parse_result_destroy(result);
        }
        free(tokens);
        dynstr_free(&candidate); // no-op if ownership was moved above
        dynstr_clear(&pending);
    }

    dynstr_free(&session);
    dynstr_free(&pending);
    free(app);
}

int main(int argc, char **argv) {
    if (argc == 1) {
        run_repl();
        return 0;
    }
    if (argc == 2) {
        return run_file(argv[0], argv[1]);
    }
    fprintf(stderr, "usage: %s [script.logo]\n", argv[0]);
    return 1;
}
