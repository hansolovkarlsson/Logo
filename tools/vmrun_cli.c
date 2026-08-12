// vmrun_cli.c
//
// Standalone headless command-line driver for the bytecode VM (see
// docs/BYTECODE_VM_DESIGN.md) -- `bin/vmrun script.logo` compiles the
// script (logo_lex -> logo_parse -> compile_program) and runs it on
// vm_run/vm_resume*, printing PRINT/error output. Same purpose as
// tools/logi_cli.c, but against the VM (compiler.c/bytecode.c/vm.c)
// instead of the tree-walker (eval.c) -- needed because bin/logomotive itself
// never exits (it's a GTK event loop) and has no headless mode, so
// there was previously no way to run a VM-only builtin (REPCOUNT,
// EVAL, bitwise ops, etc.) from a shell and see its output, the way
// bin/logi already does for the tree-walker. Built for verifying
// docs/TUTORIAL.md's own examples against the real VM before they ship
// -- same discipline as docs/COMMAND_REFERENCE.md's "every example
// actually runs" rule.
//
// No REPL (unlike logi_cli.c) -- this exists to run one script file
// and exit, not for interactive use. Not wired into bin/logomotive at all.
//
// Suspending opcodes are auto-resumed rather than left hanging, since
// there's no real clock/keyboard/window here: WAIT/PAUSE/MOTION_DELAY
// resume immediately (no actual delay), WAITKEY/INPUT resume with a
// fixed canned value. AWAIT/YIELD/LAUNCH/ANIMATESPRITE (concurrent
// agents, sprite animation) are out of scope for this tool -- it
// reports the suspension and stops rather than faking scheduler/frame
// state.
//
// Usage: bin/vmrun script.logo

#include "../src/ast.h"
#include "../src/bytecode.h"
#include "../src/compiler.h"
#include "../src/interpreter.h"
#include "../src/lexer.h"
#include "../src/parser.h"
#include "../src/vm.h"

#include <stdio.h>
#include <stdlib.h>

#define MAX_CLI_TOKENS 16384

static void print_sink(LogoApp *app, const char *text) {
    (void)app;
    fputs(text, stdout);
}

// Matches test_vm.c's own new_app() -- the same default state a
// script expects (turtle 0 at canvas center, pen down implicitly via
// init_turtle, white background).
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

static const char *status_name(VmRunResult status) {
    switch (status) {
        case VM_RUN_HALTED: return "HALTED";
        case VM_RUN_SUSPENDED_WAIT: return "SUSPENDED_WAIT";
        case VM_RUN_SUSPENDED_WAITKEY: return "SUSPENDED_WAITKEY";
        case VM_RUN_SUSPENDED_INPUT: return "SUSPENDED_INPUT";
        case VM_RUN_SUSPENDED_PAUSE: return "SUSPENDED_PAUSE";
        case VM_RUN_SUSPENDED_ANIMATESPRITE: return "SUSPENDED_ANIMATESPRITE";
        case VM_RUN_SUSPENDED_LAUNCH: return "SUSPENDED_LAUNCH";
        case VM_RUN_SUSPENDED_AWAIT: return "SUSPENDED_AWAIT";
        case VM_RUN_SUSPENDED_YIELD: return "SUSPENDED_YIELD";
        case VM_RUN_SUSPENDED_MOTION_DELAY: return "SUSPENDED_MOTION_DELAY";
    }
    return "UNKNOWN";
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

    // Heap, not stack -- same rule this project applies everywhere
    // else (see logi_cli.c's own comment on this).
    LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_CLI_TOKENS);
    int n = logo_lex(source, tokens, MAX_CLI_TOKENS);
    if (n < 0) {
        fprintf(stderr, "%s: script needs more than %d tokens\n", prog, MAX_CLI_TOKENS);
        free(tokens);
        free(source);
        return 1;
    }

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
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    int suspend_guard = 0;
    while (status != VM_RUN_HALTED && suspend_guard++ < 100000) {
        switch (status) {
            case VM_RUN_SUSPENDED_WAIT:
            case VM_RUN_SUSPENDED_PAUSE:
            case VM_RUN_SUSPENDED_MOTION_DELAY:
                status = vm_resume(vm, app, &result->pool, chunk);
                break;
            case VM_RUN_SUSPENDED_WAITKEY:
                status = vm_resume_with_key(vm, app, &result->pool, chunk, "space");
                break;
            case VM_RUN_SUSPENDED_INPUT:
                status = vm_resume_with_input(vm, app, &result->pool, chunk, "");
                break;
            default:
                fprintf(stderr, "%s: stopped at unsupported suspension %s (concurrent agents/sprite animation aren't simulated by this tool)\n", prog, status_name(status));
                free(vm);
                free(chunk);
                free(app);
                parse_result_destroy(result);
                free(tokens);
                free(source);
                return 1;
        }
    }

    free(vm);
    free(chunk);
    free(app);
    parse_result_destroy(result);
    free(tokens);
    free(source);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s script.logo\n", argv[0]);
        return 1;
    }
    return run_file(argv[0], argv[1]);
}
