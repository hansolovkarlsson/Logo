// headless.c
//
// bin/logomotive --headless script.logo (see main.c's own
// consume_headless_flag): runs one script with no GTK window/event
// loop at all, then exits -- for scripting/automation use rather than
// interactive use (docs/ROADMAP.md's own former "--headless CLI flag"
// entry, now shipped -- see docs/CHANGELOG.md). Promotes
// tools/vmrun_cli.c's same headless-VM-driver approach (built
// 2026-08-12 for doc verification) into the real app, but more
// complete: WAITKEY/INPUT read genuine stdin instead of a canned
// value (see read_stdin_line below), ANIMATESPRITE resolves every
// frame in a tight loop instead of stopping at "unsupported", and
// LAUNCH hands off to agent.c's own scheduler_run exactly the way
// ui.c's handle_vm_result does -- concurrent agents need no real GTK
// timer at all (see agent.h's own file comment), so that path was
// already fully headless-safe with no changes needed.
//
// Every suspend point resolves instantly, with no real delay --
// honoring SETSPEED/WAIT's real timing would mean pausing a process
// with no window to watch a drawing unfold in, and headless mode
// implies batch/automation use in the first place (the design question
// docs/ROADMAP.md's own entry raised, resolved this way).

#include "agent.h"
#include "ast.h"
#include "bytecode.h"
#include "compiler.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HEADLESS_TOKENS 16384

static void print_sink(LogoApp *app, const char *text) {
    (void)app;
    fputs(text, stdout);
}

// Matches every other headless entry point's own default state (see
// tests/test_vm.c's new_app, tools/vmrun_cli.c's make_app): turtle 0 at
// canvas center, pen down, white background. Every GTK-only callback
// (request_redraw, load_sprite_image, ...) stays NULL, same as any
// other headless run -- there's no window/file dialog/GdkPixbuf loader
// to back them with here either, and every builtin that uses one
// already tolerates NULL (see e.g. CLEARTEXT/LOADSPRITE's own comments
// in vm.c).
static LogoApp *make_headless_app(void) {
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

// A real stdin line for WAITKEY/INPUT -- unlike vmrun_cli.c's canned
// values (that tool exists purely to verify doc examples, never meant
// for real use), headless mode is meant for genuine automation, so
// both read real input: INPUT gets the line's own text, matching its
// ordinary meaning exactly; WAITKEY gets the same line's text used as
// its "key name" (e.g. piping in "space\n" satisfies `WAITKEY =
// "space`) -- a deliberate, documented simplification of real
// single-keypress capture, which would need raw terminal mode and
// GDK-style key-name mapping (see ui.c's own gdk_keyval_name calls)
// that headless mode's own batch-use case doesn't warrant. Returns ""
// (not NULL) at EOF, so a script that hits WAITKEY/INPUT with no more
// input left resolves instead of crashing.
static const char *read_stdin_line(char *buf, size_t buf_size) {
    if (fgets(buf, (int)buf_size, stdin) == NULL) {
        buf[0] = '\0';
        return buf;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return buf;
}

// Wraps `vm` (already suspended on its own first LAUNCH, exactly as
// vm_run left it) as agent.c's own "initial" Agent and hands off to
// scheduler_run -- the same construction ui.c's handle_vm_result uses
// for its own VM_RUN_SUSPENDED_LAUNCH case, minus the GTK redraw at the
// end (there's no canvas to redraw here). Frees `vm` itself (folded
// into the Agent it becomes), matching handle_vm_result's own comment
// on this exact point.
static void run_as_agent(LogoApp *app, AstPool *pool, BytecodeChunk *chunk, Vm *vm) {
    Agent *initial_agent = calloc(1, sizeof(Agent));
    initial_agent->vm = *vm;
    free(vm);
    initial_agent->turtle_index = app->current_turtle;
    initial_agent->throw_requested = app->throw_requested;
    snprintf(initial_agent->throw_tag, sizeof(initial_agent->throw_tag), "%s", app->throw_tag);
    initial_agent->run_depth = app->run_depth;
    initial_agent->state = AGENT_READY;
    initial_agent->started = TRUE;
    scheduler_run(app, pool, chunk, initial_agent);
}

int run_headless_script(const char *prog, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "%s: cannot open %s\n", prog, path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *source = malloc((size_t)size + 1);
    size_t bytes_read = fread(source, 1, (size_t)size, f);
    source[bytes_read] = '\0';
    fclose(f);
    logo_normalize_newlines(source); // a CRLF script must behave as an LF one

    // Heap, not stack -- same rule this project applies everywhere else
    // (see vmrun_cli.c's own comment on this).
    LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_HEADLESS_TOKENS);
    int n = logo_lex(source, tokens, MAX_HEADLESS_TOKENS);
    if (n < 0) {
        fprintf(stderr, "%s: script needs more than %d tokens\n", prog, MAX_HEADLESS_TOKENS);
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

    LogoApp *app = make_headless_app();
    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));

    char line_buf[4096];
    int exit_code = 0;
    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);
    for (;;) {
        switch (status) {
            case VM_RUN_HALTED:
                free(vm);
                goto done;
            case VM_RUN_SUSPENDED_WAIT:
            case VM_RUN_SUSPENDED_PAUSE:
            case VM_RUN_SUSPENDED_MOTION_DELAY:
                status = vm_resume(vm, app, &result->pool, chunk);
                break;
            case VM_RUN_SUSPENDED_WAITKEY:
                status = vm_resume_with_key(vm, app, &result->pool, chunk,
                                             read_stdin_line(line_buf, sizeof(line_buf)));
                break;
            case VM_RUN_SUSPENDED_INPUT:
                status = vm_resume_with_input(vm, app, &result->pool, chunk,
                                               read_stdin_line(line_buf, sizeof(line_buf)));
                break;
            case VM_RUN_SUSPENDED_ANIMATESPRITE:
                status = vm_resume_animatesprite(vm, app, &result->pool, chunk);
                break;
            case VM_RUN_SUSPENDED_LAUNCH:
                run_as_agent(app, &result->pool, chunk, vm); // frees vm itself
                goto done;
            default:
                // VM_RUN_SUSPENDED_AWAIT/YIELD reached with no LAUNCH
                // ever having run first -- same bare-AWAIT/YIELD case
                // ui.c's own handle_vm_result reports (see its comment
                // there for why neither means anything outside
                // agent.c's own scheduler).
                fprintf(stderr, "%s: AWAIT/YIELD outside a concurrent-agent run (started by LAUNCH) are not supported\n", prog);
                free(vm);
                exit_code = 1;
                goto done;
        }
    }

done:
    free(chunk);
    free(app);
    parse_result_destroy(result);
    free(tokens);
    free(source);
    return exit_code;
}
