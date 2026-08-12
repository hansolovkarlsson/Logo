// vm.c
//
// See vm.h for the frame-layout decision and rationale. The dispatch
// loop below is deliberately a flat switch over an explicit program
// counter (`pc`), not recursive C function calls the way eval.c's own
// exec_block/eval_expr are -- that's the entire point of Stage 2 (see
// docs/BYTECODE_VM_DESIGN.md's own "Why"): a Logo-level call becomes a
// VmFrame push and a `pc` jump, not a new C stack frame, so Logo-level
// recursion depth stops being coupled to C stack depth -- and, since
// Vm.scopes/Vm.scope_depth own their own storage (see MAX_VM_SCOPE_DEPTH's
// own comment in vm.h), no longer coupled to eval_logo/ast_eval's own
// app->scopes[]/MAX_SCOPE_DEPTH ceiling either.
//
// Every opcode handler below is a deliberate, checked-against-the-
// source-code replica of the matching eval.c logic (eval_expr's
// AST_VARREF/AST_BINOP/AST_NEG cases, eval_condition's AST_COMPARE
// non-numeric fallback, exec_call's *result-defaults-to-num_val(0)
// convention, call_ast_procedure/do_user_procedure_call's OUTPUT/STOP
// handling) -- see this project's shadow-diff strategy
// (tests/test_vm.c): the VM's whole job is to reach byte-identical
// output to ast_eval, not just "plausible" output.

#include "vm.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static void push(Vm *vm, EvalValue v) {
    if (vm->stack_top >= MAX_VM_STACK) return; // can't happen for this vertical slice's own test scripts; see this file's own note on MAX_INSTRUCTIONS overflow handling in compiler.c for the same "not yet robustly reported" tradeoff
    vm->stack[vm->stack_top++] = v;
}

static EvalValue pop(Vm *vm) {
    if (vm->stack_top <= 0) return num_val(0);
    return vm->stack[--vm->stack_top];
}

// See bytecode.h's own comment on OP_PEEK: reads a copy of the value
// `depth` slots below the current top (0 = the top itself) without
// removing it.
static EvalValue peek(Vm *vm, int depth) {
    int idx = vm->stack_top - 1 - depth;
    if (idx < 0 || idx >= vm->stack_top) return num_val(0);
    return vm->stack[idx];
}

// See bytecode.h's own comment on OP_POKE: pops the top value, then
// overwrites the persistent slot `depth` below the NEW (post-pop) top
// with it.
static void poke(Vm *vm, int depth, EvalValue v) {
    int idx = vm->stack_top - 1 - depth;
    if (idx < 0 || idx >= vm->stack_top) return;
    vm->stack[idx] = v;
}

// `vm`'s own scope stack, wrapped as a ScopeStack -- what every
// opcode/helper below passes to find_var/set_var*/eval_local_declare/
// eval_push_scope_for_call, in place of interpreter.h's own
// app_scope_stack(app) (which eval_logo/ast_eval still use). See
// ScopeStack's own comment in logo_types.h and MAX_VM_SCOPE_DEPTH's own
// comment in vm.h.
static ScopeStack vm_scope_stack(Vm *vm) {
    return (ScopeStack){vm->scopes, &vm->scope_depth, MAX_VM_SCOPE_DEPTH};
}

// Binary arithmetic (OP_ADD/OP_SUB/OP_MUL/OP_DIV): pop right then left
// (right was pushed last), matching eval_expr's own AST_BINOP
// left-then-right evaluation order and AST_OP_DIV's own "divide by
// zero reads as 0" fallback.
static void exec_arith(Vm *vm, OpCode op) {
    double right = eval_to_number(pop(vm));
    double left = eval_to_number(pop(vm));
    double result = 0;
    switch (op) {
        case OP_ADD: result = left + right; break;
        case OP_SUB: result = left - right; break;
        case OP_MUL: result = left * right; break;
        case OP_DIV: result = (right != 0) ? left / right : 0; break;
        default: break;
    }
    push(vm, num_val(result));
}

// OP_CMP_* -- mirrors eval_condition's own AST_COMPARE case exactly,
// including its non-numeric fallback (eval_values_equal for EQ/NE, 0
// for any other ordering comparison on non-numeric operands).
static void exec_compare(LogoApp *app, Vm *vm, OpCode op) {
    EvalValue right = pop(vm);
    EvalValue left = pop(vm);
    int result;
    if (left.type != VALUE_NUMBER || right.type != VALUE_NUMBER) {
        int equal = eval_values_equal(app, left, right);
        if (op == OP_CMP_EQ) result = equal;
        else if (op == OP_CMP_NE) result = !equal;
        else result = 0;
    } else {
        switch (op) {
            case OP_CMP_LT: result = left.number < right.number; break;
            case OP_CMP_GT: result = left.number > right.number; break;
            case OP_CMP_EQ: result = left.number == right.number; break;
            case OP_CMP_LE: result = left.number <= right.number; break;
            case OP_CMP_GE: result = left.number >= right.number; break;
            default: result = left.number != right.number; break;
        }
    }
    push(vm, num_val(result));
}

// OP_CALL_BUILTIN's own dispatch -- one branch per builtin this batch
// recognizes, each just forwarding to eval.c's own exposed value-taking
// core (see eval.h's own note: this is deliberately the same function
// the corresponding do_* wrapper calls after its own eval_expr, so
// there's no parallel reimplementation to drift from it). `*produced`
// is 1 for a real value-returning builtin, 0 for a void one (SETITEM/
// FILLARRAY) -- read by OP_CHECK_OUTPUT for a call used in expression
// position, matching eval_expr's own AST_CALL wrapper (`resolved &&
// !produced` -> "didn't output a value"). `args` already holds exactly
// this builtin's own arity worth of values, in argument order -- the
// parser guarantees that arity at parse time, so no argc parameter is
// needed here.
// CONTINUE/CO -- the other half of PAUSE (see OP_PAUSE), but not itself
// a suspend point: it's an ordinary command that just decrements the
// shared app->pause_depth (interpreter.c's own do_continue, unported
// until now since it had no PAUSE to pair with in this pipeline).
// Whether that decrement actually satisfies some suspended run's own
// captured pause_level is entirely ui.c's concern (see
// maybe_resume_paused_runs there) -- this function has no opinion on
// who, if anyone, is waiting.
static void exec_continue(LogoApp *app) {
    if (app->pause_depth > 0) app->pause_depth--;
    else append_output(app, "CONTINUE: nothing is paused\n");
}

// BACKTRACE/BT -- prints the current call stack, innermost first.
// Direct port of interpreter.c's own version, ported 2026-08-12
// (docs/ROADMAP.md's "Remaining old-engine builtins" entry), but
// reading vm->scope_depth/vm->scopes[] (VM-owned scope storage, see
// vm.h's own comment) rather than app->scope_depth/app->scopes[] --
// the two are separate arrays, so this couldn't just reuse the old
// engine's own version unchanged.
static void exec_backtrace(Vm *vm, LogoApp *app) {
    append_output(app, "BACKTRACE:\n");
    for (int s = vm->scope_depth - 1; s >= 0; s--) {
        char line[64];
        snprintf(line, sizeof(line), "  %s\n", vm->scopes[s].proc_name);
        append_output(app, line);
    }
    append_output(app, "  (top level)\n");
}

// The five ordinary (non-suspending) sprite commands -- direct ports of
// interpreter.c's own do_loadsprite/do_loadspritesheet/do_setsprite/
// do_setspriteframe/do_stampsprite, deliberately vm.c-only (not also
// added to eval.c/ast_eval, the same scope the user chose for
// WAIT/WAITKEY/INPUT/PAUSE): bin/logomotive no longer runs on ast_eval at
// all, so porting there too would only buy extra shadow-diff test
// infrastructure for a subsystem that only ever executes through the
// live VM. All five reuse already-public LogoApp/Turtle fields
// (sprite_names/sprite_images/sprite_frame_cols/rows/sprite_count,
// Turtle.sprite_index/sprite_frame) and app->load_sprite_image (a
// GTK-side callback-pointer seam, same shape as request_redraw --
// silently a no-op when NULL, e.g. headless tests, matching
// interpreter.c's own convention exactly).
//
// Unlike interpreter.c's own versions, none of these re-validate their
// own quoted-word arguments for a literal leading '"' ("expected a
// \"name" style errors): that check exists there only because
// interpreter.c parses raw text with sscanf; this pipeline's real
// grammar-based parser (ARG_QUOTED_WORD) already guarantees a
// syntactically valid name at parse time, the same reason ERASE/MAKE/
// DELETEFILE never emit that class of error here either.
// LOADPIC/SAVEPIC -- canvas background image load/export, ported from
// interpreter.c 2026-08-12 (docs/ROADMAP.md's "Remaining old-engine
// builtins" entry). Same shape as exec_loadsprite below:
// app->load_background_image/save_canvas_image were already wired up
// in ui.c for the old engine's own use of them, NULL in headless
// tests (no real GUI to load/export an image with), same convention.
static void exec_loadpic(LogoApp *app, EvalValue path_val) {
    char path_buf[512];
    eval_value_to_text(app, path_val, path_buf, sizeof(path_buf));
    if (app->load_background_image != NULL && !app->load_background_image(app, path_buf)) {
        append_output(app, "LOADPIC: could not load \"");
        append_output(app, path_buf);
        append_output(app, "\n");
    }
}

static void exec_savepic(LogoApp *app, EvalValue path_val) {
    char path_buf[512];
    eval_value_to_text(app, path_val, path_buf, sizeof(path_buf));
    if (app->save_canvas_image != NULL && !app->save_canvas_image(app, path_buf)) {
        append_output(app, "SAVEPIC: could not save \"");
        append_output(app, path_buf);
        append_output(app, "\n");
    }
}

// TONE frequency seconds -- fire-and-forget, matching interpreter.c's
// own exact behavior: no error reported if play_tone fails (bad
// frequency/seconds, or no audio device), unlike PLAYSOUND below. Not
// this port's place to change that asymmetry.
static void exec_tone(LogoApp *app, EvalValue freq_val, EvalValue seconds_val) {
    double frequency = eval_to_number(freq_val);
    double seconds = eval_to_number(seconds_val);
    if (app->play_tone != NULL) {
        app->play_tone(app, frequency, seconds);
    }
}

static void exec_playsound(LogoApp *app, EvalValue path_val) {
    char path_buf[512];
    eval_value_to_text(app, path_val, path_buf, sizeof(path_buf));
    if (app->play_sound_file != NULL && !app->play_sound_file(app, path_buf)) {
        append_output(app, "PLAYSOUND: could not load \"");
        append_output(app, path_buf);
        append_output(app, "\n");
    }
}

static void exec_loadsprite(LogoApp *app, EvalValue name_val, EvalValue path_val) {
    char name_buf[64], path_buf[512];
    eval_value_to_text(app, name_val, name_buf, sizeof(name_buf));
    eval_value_to_text(app, path_val, path_buf, sizeof(path_buf));
    if (app->load_sprite_image != NULL && !app->load_sprite_image(app, name_buf, path_buf, 1, 1)) {
        append_output(app, "LOADSPRITE: could not load \"");
        append_output(app, path_buf);
        append_output(app, "\n");
    }
}

static void exec_loadspritesheet(LogoApp *app, EvalValue name_val, EvalValue path_val, EvalValue cols_val, EvalValue rows_val) {
    char name_buf[64], path_buf[512];
    eval_value_to_text(app, name_val, name_buf, sizeof(name_buf));
    eval_value_to_text(app, path_val, path_buf, sizeof(path_buf));
    double cols = eval_to_number(cols_val);
    double rows = eval_to_number(rows_val);
    if (cols < 1 || rows < 1) {
        append_output(app, "LOADSPRITESHEET: cols and rows must be at least 1\n");
    } else if (app->load_sprite_image != NULL &&
               !app->load_sprite_image(app, name_buf, path_buf, (int)cols, (int)rows)) {
        append_output(app, "LOADSPRITESHEET: could not load \"");
        append_output(app, path_buf);
        append_output(app, "\n");
    }
}

static void exec_setsprite(LogoApp *app, EvalValue name_val) {
    char name_buf[64];
    eval_value_to_text(app, name_val, name_buf, sizeof(name_buf));
    Turtle *t = current_turtle(app);
    if (strcasecmp(name_buf, "NONE") == 0) {
        t->sprite_index = -1;
        t->sprite_frame = 0;
        return;
    }
    int idx = -1;
    for (int i = 0; i < app->sprite_count; i++) {
        if (strcasecmp(app->sprite_names[i], name_buf) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        append_output(app, "SETSPRITE: no such sprite \"");
        append_output(app, name_buf);
        append_output(app, "\n");
    } else {
        t->sprite_index = idx;
        t->sprite_frame = 0;
    }
}

static void exec_setspriteframe(LogoApp *app, EvalValue n_val) {
    double n = eval_to_number(n_val);
    Turtle *t = current_turtle(app);
    if (t->sprite_index < 0) {
        append_output(app, "SETSPRITEFRAME: no sprite set (use SETSPRITE first)\n");
        return;
    }
    int frame_count = app->sprite_frame_cols[t->sprite_index] * app->sprite_frame_rows[t->sprite_index];
    if ((int)n < 0 || (int)n >= frame_count) {
        append_output(app, "SETSPRITEFRAME: frame out of range\n");
    } else {
        t->sprite_frame = (int)n;
    }
}

static void exec_stampsprite(LogoApp *app) {
    if (app->raster_op_count < MAX_RASTER_OPS) {
        Turtle *t = current_turtle(app);
        RasterOp *op = &app->raster_ops[app->raster_op_count++];
        op->kind = RASTER_OP_STAMP;
        op->x = t->x;
        op->y = t->y;
        op->angle = t->angle;
        op->sprite_index = t->sprite_index;
        op->sprite_frame = t->sprite_frame;
        op->line_count_at_call = app->line_count;
    }
}

// SAVEBYTECODE "path -- Stage D of the bytecode save/load/assembler
// initiative (docs/ROADMAP.md's own "Bytecode save/load/assembler"
// section): disassembles `chunk` (the chunk THIS OP_CALL_BUILTIN
// instruction itself lives in -- see call_builtin's own new `chunk`
// parameter and vm_run's OP_CALL_BUILTIN case, the only call site) into
// text via bytecode_disassemble, written to `path` via
// g_file_set_contents. Same success/failure messaging shape as SAVE
// (eval_save_value): "Saved <path>\n" on success, "SAVEBYTECODE: could
// not write file\n" on failure.
static void exec_savebytecode(LogoApp *app, BytecodeChunk *chunk, const char *path) {
    char *text = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&text, &size);
    bytecode_disassemble(chunk, f);
    fclose(f);
    GError *error = NULL;
    if (g_file_set_contents(path, text, (gssize)size, &error)) {
        append_output(app, "Saved ");
        append_output(app, path);
        append_output(app, "\n");
    } else {
        append_output(app, "SAVEBYTECODE: could not write file\n");
        g_error_free(error);
    }
    free(text);
}

// LOADBYTECODE "path -- the reverse of SAVEBYTECODE above: reads
// `path`, parses it via bytecode_assemble into a fresh scratch
// BytecodeChunk, then runs it via a RECURSIVE vm_run call sharing this
// same Vm's own stack/frames -- the same recursive-call mechanism
// exec_run/exec_load below already use for RUN/LOAD, but with no real
// AstPool actually needed: the assembled chunk is already fully self-
// contained (see docs/BYTECODE_VM_DESIGN.md's "Self-contained
// BytecodeChunk" entry), so an empty one (node_count=0, never actually
// read) is passed just to satisfy vm_run's own signature. Unlike LOAD,
// there's no separate "hoist TO...END, then run the rest" split to
// preserve -- an assembled chunk already IS one complete compiled
// program (every procedure body AND every top-level statement already
// baked into a single instruction stream), so this recursive vm_run
// call's only job is to run the whole thing, starting at the file's
// own recorded entry point (chunk->start_pc, recovered from its own
// "START:" line -- see BytecodeChunk.start_pc's own comment for why
// this can't just be 0). Same frame_floor OUTPUT/STOP-escape guard
// exec_run/exec_load already use, for the same reason.
static void exec_loadbytecode(Vm *vm, LogoApp *app, const char *path) {
    char *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        append_output(app, "LOADBYTECODE: could not read file\n");
        g_error_free(error);
        return;
    }
    BytecodeChunk *scratch_chunk = calloc(1, sizeof(BytecodeChunk));
    char asm_err[256];
    if (!bytecode_assemble(contents, scratch_chunk, asm_err, sizeof(asm_err))) {
        append_output(app, "LOADBYTECODE: ");
        append_output(app, asm_err);
        append_output(app, "\n");
    } else {
        AstPool *empty_pool = calloc(1, sizeof(AstPool));
        int frame_floor = vm->frame_count;
        vm_run(vm, app, empty_pool, scratch_chunk, scratch_chunk->start_pc);
        if (vm->frame_count < frame_floor) {
            append_output(app, "LOADBYTECODE: OUTPUT/STOP escaping the loaded program's own top level is not fully supported\n");
        }
        free(empty_pool);
    }
    free(scratch_chunk);
    g_free(contents);
}

// ONKEY "procname -- registers procname (must take exactly one input,
// bound to the pressed key's name -- same gdk_keyval_name convention
// WAITKEY's own output already uses) to fire as a fresh background
// invocation on every keypress (see ui.c's own fire_onkey/handle_vm_
// result). Validated eagerly here (exists, exactly 1 param) so a typo
// is reported immediately rather than silently doing nothing the first
// time a key is pressed; a validation failure leaves any previously-
// registered handler untouched, not cleared. The actual firing --
// resolving app->onkey_handler against a *retained* copy of this
// chunk, since this run's own chunk is normally freed the moment it
// finishes -- is ui.c's job, not this engine's: vm.c stays headless/
// GTK-free, same as everywhere else in this file.
static void exec_onkey(LogoApp *app, BytecodeChunk *chunk, const char *proc_name) {
    const ProcAddr *def = bytecode_find_proc_entry(chunk, proc_name);
    if (def == NULL) {
        append_output(app, "ONKEY: no such procedure \"");
        append_output(app, proc_name);
        append_output(app, "\n");
        return;
    }
    if (def->param_count != 1) {
        append_output(app, "ONKEY: procedure \"");
        append_output(app, proc_name);
        append_output(app, "\" must take exactly one input (the pressed key's name)\n");
        return;
    }
    snprintf(app->onkey_handler, sizeof(app->onkey_handler), "%s", proc_name);
}

// ONCLICK "procname -- same shape as exec_onkey above, but for mouse
// clicks on the canvas: procname must take exactly three inputs, bound
// to the click's x, y (canvas-relative pixels, the same coordinate
// space SETXY/POS already use) and button number (1/2/3, GDK's own
// left/middle/right convention).
static void exec_onclick(LogoApp *app, BytecodeChunk *chunk, const char *proc_name) {
    const ProcAddr *def = bytecode_find_proc_entry(chunk, proc_name);
    if (def == NULL) {
        append_output(app, "ONCLICK: no such procedure \"");
        append_output(app, proc_name);
        append_output(app, "\n");
        return;
    }
    if (def->param_count != 3) {
        append_output(app, "ONCLICK: procedure \"");
        append_output(app, proc_name);
        append_output(app, "\" must take exactly three inputs (x, y, button)\n");
        return;
    }
    snprintf(app->onclick_handler, sizeof(app->onclick_handler), "%s", proc_name);
}

// ONMOUSEMOVE "procname -- same shape as exec_onclick above, but for
// pointer motion: procname must take exactly two inputs, bound to the
// pointer's x, y (canvas-relative pixels, same coordinate space
// SETXY/POS/ONCLICK already use). Motion events can fire 60+/sec --
// ui.c's own fire_handler already declines to fire at all while the
// interpreter isn't idle (see its own comment), which is what actually
// keeps a slow/suspending handler from piling up invocations; nothing
// extra needed here at registration time.
static void exec_onmousemove(LogoApp *app, BytecodeChunk *chunk, const char *proc_name) {
    const ProcAddr *def = bytecode_find_proc_entry(chunk, proc_name);
    if (def == NULL) {
        append_output(app, "ONMOUSEMOVE: no such procedure \"");
        append_output(app, proc_name);
        append_output(app, "\n");
        return;
    }
    if (def->param_count != 2) {
        append_output(app, "ONMOUSEMOVE: procedure \"");
        append_output(app, proc_name);
        append_output(app, "\" must take exactly two inputs (x, y)\n");
        return;
    }
    snprintf(app->onmousemove_handler, sizeof(app->onmousemove_handler), "%s", proc_name);
}

// ONKEYUP "procname -- same shape as exec_onkey above, but for key
// *release* rather than press: procname must take exactly one input
// (:KEY, same gdk_keyval_name convention).
static void exec_onkeyup(LogoApp *app, BytecodeChunk *chunk, const char *proc_name) {
    const ProcAddr *def = bytecode_find_proc_entry(chunk, proc_name);
    if (def == NULL) {
        append_output(app, "ONKEYUP: no such procedure \"");
        append_output(app, proc_name);
        append_output(app, "\n");
        return;
    }
    if (def->param_count != 1) {
        append_output(app, "ONKEYUP: procedure \"");
        append_output(app, proc_name);
        append_output(app, "\" must take exactly one input (the released key's name)\n");
        return;
    }
    snprintf(app->onkeyup_handler, sizeof(app->onkeyup_handler), "%s", proc_name);
}

// ONRELEASE "procname -- same shape as exec_onclick above, but for
// button *release* rather than press: procname must take exactly three
// inputs (:X :Y :BUTTON, same convention as ONCLICK).
static void exec_onrelease(LogoApp *app, BytecodeChunk *chunk, const char *proc_name) {
    const ProcAddr *def = bytecode_find_proc_entry(chunk, proc_name);
    if (def == NULL) {
        append_output(app, "ONRELEASE: no such procedure \"");
        append_output(app, proc_name);
        append_output(app, "\n");
        return;
    }
    if (def->param_count != 3) {
        append_output(app, "ONRELEASE: procedure \"");
        append_output(app, proc_name);
        append_output(app, "\" must take exactly three inputs (x, y, button)\n");
        return;
    }
    snprintf(app->onrelease_handler, sizeof(app->onrelease_handler), "%s", proc_name);
}

// 2026-08-11 Terrapin Logo comparison (docs/ROADMAP.md's "Language
// completeness") -- 24 small general-purpose operators/commands found
// genuinely missing, all VM-only (see parser.c's own note on this
// batch), grouped below by category.

// ASCII/CHAR: single-character <-> code-point conversion. Plain C char
// (0-255), not full Unicode -- this project's word/text handling is
// byte-oriented throughout (EvalValue.word/AST_MAX_TEXT), so this
// deliberately doesn't attempt UTF-8 codepoint synthesis.
static EvalValue eval_ascii_value(LogoApp *app, EvalValue v) {
    char text[512];
    eval_value_to_text(app, v, text, sizeof(text));
    if (text[0] == '\0' || text[1] != '\0') {
        append_output(app, "ASCII: expected a single character\n");
        return num_val(0);
    }
    return num_val((unsigned char)text[0]);
}
static EvalValue eval_char_value(EvalValue v) {
    char text[2] = { (char)(int)eval_to_number(v), '\0' };
    return word_val(text);
}

static EvalValue eval_uppercase_value(LogoApp *app, EvalValue v) {
    char text[512];
    eval_value_to_text(app, v, text, sizeof(text));
    for (char *p = text; *p != '\0'; p++) *p = (char)toupper((unsigned char)*p);
    return word_val(text);
}
static EvalValue eval_lowercase_value(LogoApp *app, EvalValue v) {
    char text[512];
    eval_value_to_text(app, v, text, sizeof(text));
    for (char *p = text; *p != '\0'; p++) *p = (char)tolower((unsigned char)*p);
    return word_val(text);
}

// BITAND/BITOR/BITXOR/BITNOT/LSHIFT/RSHIFT: integer bitwise ops,
// truncating through a 64-bit integer (wide enough for real bit
// patterns, unlike INT's own fmod-precision-oriented trunc). Named
// BIT*/*SHIFT rather than Terrapin's own LOGAND/LOGOR/LOGXOR/LOGNOT/LSH
// (still the underlying convention these follow) for a more
// self-explanatory, less cryptic name -- a deliberate user preference,
// not a Terrapin-compatibility choice like most of the rest of this
// batch. LSHIFT/RSHIFT are two separate, explicitly-directional
// operators (Terrapin's own LSH instead overloads a single operator's
// sign to mean direction) -- each still tolerates a negative shift
// count by flipping direction rather than invoking undefined C
// behavior, but that's a robustness fallback, not the intended way to
// ask for the other direction; use the other operator instead.
static EvalValue eval_bitand_value(EvalValue a, EvalValue b) {
    return num_val((double)((long long)eval_to_number(a) & (long long)eval_to_number(b)));
}
static EvalValue eval_bitor_value(EvalValue a, EvalValue b) {
    return num_val((double)((long long)eval_to_number(a) | (long long)eval_to_number(b)));
}
static EvalValue eval_bitxor_value(EvalValue a, EvalValue b) {
    return num_val((double)((long long)eval_to_number(a) ^ (long long)eval_to_number(b)));
}
static EvalValue eval_bitnot_value(EvalValue a) {
    return num_val((double)(~(long long)eval_to_number(a)));
}
static EvalValue eval_lshift_value(EvalValue a, EvalValue n) {
    long long val = (long long)eval_to_number(a);
    int shift = (int)eval_to_number(n);
    return num_val((double)(shift >= 0 ? (val << shift) : (val >> -shift)));
}
static EvalValue eval_rshift_value(EvalValue a, EvalValue n) {
    long long val = (long long)eval_to_number(a);
    int shift = (int)eval_to_number(n);
    return num_val((double)(shift >= 0 ? (val >> shift) : (val << -shift)));
}

// SEC/CSC/COT take degrees (like SIN/COS/TAN); ASEC/ACSC/ACOT/ARCTAN2
// return degrees (like ASIN/ACOS/ARCTAN) -- see eval.c's own comment on
// this project's degrees-not-radians convention throughout.
static EvalValue eval_sec_value(EvalValue v) {
    return num_val(1.0 / cos(eval_to_number(v) * M_PI / 180.0));
}
static EvalValue eval_csc_value(EvalValue v) {
    return num_val(1.0 / sin(eval_to_number(v) * M_PI / 180.0));
}
static EvalValue eval_cot_value(EvalValue v) {
    return num_val(1.0 / tan(eval_to_number(v) * M_PI / 180.0));
}
static EvalValue eval_asec_value(EvalValue v) {
    return num_val(acos(1.0 / eval_to_number(v)) * 180.0 / M_PI);
}
static EvalValue eval_acsc_value(EvalValue v) {
    return num_val(asin(1.0 / eval_to_number(v)) * 180.0 / M_PI);
}
static EvalValue eval_acot_value(EvalValue v) {
    return num_val(atan(1.0 / eval_to_number(v)) * 180.0 / M_PI);
}
static EvalValue eval_arctan2_value(EvalValue y, EvalValue x) {
    return num_val(atan2(eval_to_number(y), eval_to_number(x)) * 180.0 / M_PI);
}

// TIME/DATE: current wall-clock time/date as a word, localtime-
// formatted. MILLISECONDS: raw epoch milliseconds as a number.
static EvalValue eval_time_value(void) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return word_val(buf);
}
static EvalValue eval_date_value(void) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
    return word_val(buf);
}
static EvalValue eval_milliseconds_value(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    return num_val(ms);
}

// DEFINED? -- looks the word up in the CURRENTLY EXECUTING chunk's own
// procs[] table, same lookup exec_onkey/exec_onclick already use to
// validate a handler at registration time.
static EvalValue eval_definedp_value(LogoApp *app, BytecodeChunk *chunk, EvalValue v) {
    char text[512];
    eval_value_to_text(app, v, text, sizeof(text));
    return word_val(bytecode_find_proc_entry(chunk, text) != NULL ? "TRUE" : "FALSE");
}

static EvalValue eval_turtles_value(LogoApp *app) {
    return num_val(app->turtle_count);
}

// REPCOUNT: the innermost currently-running REPEAT's 1-indexed pass
// number (pushed/incremented/popped by OP_REPCOUNT_PUSH/INCR/POP, see
// bytecode.h's own comment), or -1 (Terrapin's own documented value)
// outside any REPEAT. Reads vm->repcount_stack directly rather than
// app state -- unlike every other builtin in this file, which only
// ever needs `app` -- since this is genuinely per-Vm (per concurrent
// agent) state, same as vm->scopes.
static EvalValue eval_repcount_value(Vm *vm) {
    return num_val(vm->repcount_depth > 0 ? vm->repcount_stack[vm->repcount_depth - 1] : -1);
}

// READWORD/READCHAR channel -- finer-grained reads than READLINE
// (eval.c's own eval_readline_value, shared with the old tree-walker;
// these two are VM-only, like every other builtin added this session
// -- no existing convention anywhere in this codebase for word/char
// boundaries to match, so both are a fresh, deliberately simple
// design, not ported from anywhere).
//
// READWORD skips any leading whitespace (space/tab/CR/LF), then reads
// up to the next whitespace or EOF, advancing past the word itself but
// not past whatever whitespace follows it (so a second READWORD call
// starts right after, ready to skip that leftover whitespace itself).
// READCHAR returns the single next raw byte, whitespace included, with
// no skipping at all -- the more primitive of the two. Both share
// READLINE's own EOF/bad-channel sentinel (an empty word, checkable
// via EOF? separately) rather than a distinct error path.
static EvalValue eval_readword_value(LogoApp *app, EvalValue idx_val) {
    int idx = (int)eval_to_number(idx_val);
    if (idx < 0 || idx >= MAX_OPEN_FILES || app->file_channels[idx].mode != FILE_CHANNEL_READ) {
        return word_val("");
    }
    FileChannel *fc = &app->file_channels[idx];
    size_t len = strlen(fc->read_buffer);
    size_t pos = fc->read_pos;
    while (pos < len && isspace((unsigned char)fc->read_buffer[pos])) pos++;
    if (pos >= len) {
        fc->read_pos = pos;
        return word_val("");
    }
    size_t start = pos;
    while (pos < len && !isspace((unsigned char)fc->read_buffer[pos])) pos++;
    char word[512];
    size_t word_len = pos - start;
    if (word_len >= sizeof(word)) word_len = sizeof(word) - 1;
    memcpy(word, fc->read_buffer + start, word_len);
    word[word_len] = '\0';
    fc->read_pos = pos;
    return word_val(word);
}

static EvalValue eval_readchar_value(LogoApp *app, EvalValue idx_val) {
    int idx = (int)eval_to_number(idx_val);
    if (idx < 0 || idx >= MAX_OPEN_FILES || app->file_channels[idx].mode != FILE_CHANNEL_READ) {
        return word_val("");
    }
    FileChannel *fc = &app->file_channels[idx];
    size_t len = strlen(fc->read_buffer);
    if (fc->read_pos >= len) return word_val("");
    char ch[2] = { fc->read_buffer[fc->read_pos], '\0' };
    fc->read_pos++;
    return word_val(ch);
}

// RANGE from to: integers from FROM to TO inclusive, counting by 1 if
// FROM <= TO, otherwise by -1 -- standard Logo ISEQ convention, under a
// more self-explanatory name (a deliberate user preference, same as
// BITAND/etc above).
static EvalValue eval_range_value(LogoApp *app, EvalValue from_val, EvalValue to_val) {
    int from = (int)eval_to_number(from_val);
    int to = (int)eval_to_number(to_val);
    int step = (from <= to) ? 1 : -1;
    int head = -1, tail = -1;
    for (int i = from; step > 0 ? i <= to : i >= to; i += step) {
        int node = value_to_node(app, num_val(i));
        if (node < 0) return list_pool_exhausted(app);
        if (head < 0) head = node; else app->list_pool[tail].next = node;
        tail = node;
    }
    return list_val(head);
}

// SPACEDRANGE from to count: COUNT equally spaced rational numbers
// between FROM and TO inclusive (COUNT-1 equal intervals) -- standard
// Logo RSEQ convention, under a more self-explanatory name.
static EvalValue eval_spacedrange_value(LogoApp *app, EvalValue from_val, EvalValue to_val, EvalValue count_val) {
    double from = eval_to_number(from_val);
    double to = eval_to_number(to_val);
    int count = (int)eval_to_number(count_val);
    if (count < 1) return list_val(-1);
    int head = -1, tail = -1;
    for (int i = 0; i < count; i++) {
        double v = (count == 1) ? from : from + (to - from) * i / (count - 1);
        int node = value_to_node(app, num_val(v));
        if (node < 0) return list_pool_exhausted(app);
        if (head < 0) head = node; else app->list_pool[tail].next = node;
        tail = node;
    }
    return list_val(head);
}

static EvalValue call_builtin(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const char *name, EvalValue *args, int *produced) {
    *produced = 1;
    if (strcasecmp(name, "PRINT") == 0 || strcasecmp(name, "PR") == 0) {
        eval_print_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "TYPE") == 0) {
        eval_type_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "CONTINUE") == 0 || strcasecmp(name, "CO") == 0) {
        exec_continue(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "CLEARTEXT") == 0 || strcasecmp(name, "CT") == 0) {
        if (app->clear_history != NULL) app->clear_history(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "BACKTRACE") == 0 || strcasecmp(name, "BT") == 0) {
        exec_backtrace(vm, app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "LOADPIC") == 0) {
        exec_loadpic(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SAVEPIC") == 0) {
        exec_savepic(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "TONE") == 0) {
        exec_tone(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "PLAYSOUND") == 0) {
        exec_playsound(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "STOPSOUND") == 0) {
        if (app->stop_sound != NULL) app->stop_sound(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "LOADSPRITE") == 0) {
        exec_loadsprite(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "LOADSPRITESHEET") == 0) {
        exec_loadspritesheet(app, args[0], args[1], args[2], args[3]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETSPRITE") == 0) {
        exec_setsprite(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETSPRITEFRAME") == 0) {
        exec_setspriteframe(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "STAMPSPRITE") == 0) {
        exec_stampsprite(app);
        *produced = 0;
        return num_val(0);
    }
    // TEXT/SHOW/SAVE and general file I/O (see eval.h's own note --
    // LOAD is deliberately not here, it has its own dedicated OP_LOAD).
    if (strcasecmp(name, "TEXT") == 0) return eval_text_value(app, pool, args[0]);
    if (strcasecmp(name, "SHOW") == 0) {
        eval_show_value(app, pool, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SAVE") == 0) {
        eval_save_value(app, pool, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "DELETEFILE") == 0) {
        eval_deletefile_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SAVEBYTECODE") == 0) {
        exec_savebytecode(app, chunk, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "LOADBYTECODE") == 0) {
        exec_loadbytecode(vm, app, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ONKEY") == 0) {
        exec_onkey(app, chunk, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "OFFKEY") == 0) {
        app->onkey_handler[0] = '\0';
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ONCLICK") == 0) {
        exec_onclick(app, chunk, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "OFFCLICK") == 0) {
        app->onclick_handler[0] = '\0';
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ONMOUSEMOVE") == 0) {
        exec_onmousemove(app, chunk, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "OFFMOUSEMOVE") == 0) {
        app->onmousemove_handler[0] = '\0';
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ONKEYUP") == 0) {
        exec_onkeyup(app, chunk, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "OFFKEYUP") == 0) {
        app->onkeyup_handler[0] = '\0';
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ONRELEASE") == 0) {
        exec_onrelease(app, chunk, args[0].word);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "OFFRELEASE") == 0) {
        app->onrelease_handler[0] = '\0';
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "PI") == 0) return num_val(M_PI);
    if (strcasecmp(name, "RERANDOM") == 0) {
        logo_rerandom();
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ASCII") == 0) return eval_ascii_value(app, args[0]);
    if (strcasecmp(name, "CHAR") == 0) return eval_char_value(args[0]);
    if (strcasecmp(name, "UPPERCASE") == 0) return eval_uppercase_value(app, args[0]);
    if (strcasecmp(name, "LOWERCASE") == 0) return eval_lowercase_value(app, args[0]);
    if (strcasecmp(name, "BITAND") == 0) return eval_bitand_value(args[0], args[1]);
    if (strcasecmp(name, "BITOR") == 0) return eval_bitor_value(args[0], args[1]);
    if (strcasecmp(name, "BITXOR") == 0) return eval_bitxor_value(args[0], args[1]);
    if (strcasecmp(name, "BITNOT") == 0) return eval_bitnot_value(args[0]);
    if (strcasecmp(name, "LSHIFT") == 0) return eval_lshift_value(args[0], args[1]);
    if (strcasecmp(name, "RSHIFT") == 0) return eval_rshift_value(args[0], args[1]);
    if (strcasecmp(name, "ARCTAN2") == 0) return eval_arctan2_value(args[0], args[1]);
    if (strcasecmp(name, "SEC") == 0) return eval_sec_value(args[0]);
    if (strcasecmp(name, "CSC") == 0) return eval_csc_value(args[0]);
    if (strcasecmp(name, "COT") == 0) return eval_cot_value(args[0]);
    if (strcasecmp(name, "ASEC") == 0) return eval_asec_value(args[0]);
    if (strcasecmp(name, "ACSC") == 0) return eval_acsc_value(args[0]);
    if (strcasecmp(name, "ACOT") == 0) return eval_acot_value(args[0]);
    if (strcasecmp(name, "TIME") == 0) return eval_time_value();
    if (strcasecmp(name, "DATE") == 0) return eval_date_value();
    if (strcasecmp(name, "MILLISECONDS") == 0) return eval_milliseconds_value();
    if (strcasecmp(name, "DEFINED?") == 0) return eval_definedp_value(app, chunk, args[0]);
    if (strcasecmp(name, "RANGE") == 0) return eval_range_value(app, args[0], args[1]);
    if (strcasecmp(name, "SPACEDRANGE") == 0) return eval_spacedrange_value(app, args[0], args[1], args[2]);
    if (strcasecmp(name, "TURTLES") == 0) return eval_turtles_value(app);
    if (strcasecmp(name, "REPCOUNT") == 0) return eval_repcount_value(vm);
    if (strcasecmp(name, "EVAL") == 0) return eval_eval_value(app, args[0]);
    if (strcasecmp(name, "OPENREAD") == 0) return eval_openread_value(app, args[0]);
    if (strcasecmp(name, "OPENWRITE") == 0) return eval_openwrite_value(app, args[0]);
    if (strcasecmp(name, "OPENAPPEND") == 0) return eval_openappend_value(app, args[0]);
    if (strcasecmp(name, "READLINE") == 0) return eval_readline_value(app, args[0]);
    if (strcasecmp(name, "READWORD") == 0) return eval_readword_value(app, args[0]);
    if (strcasecmp(name, "READCHAR") == 0) return eval_readchar_value(app, args[0]);
    if (strcasecmp(name, "EOF?") == 0) return eval_eofp_value(app, args[0]);
    if (strcasecmp(name, "DIRECTORY") == 0) return eval_directory_value(app);
    if (strcasecmp(name, "CLOSE") == 0) {
        eval_close_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "FILEPRINT") == 0) {
        eval_fileprint_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "THING") == 0) return eval_thing_value(app, vm_scope_stack(vm), args[0]);
    if (strcasecmp(name, "FIRST") == 0) return eval_first_value(app, args[0]);
    if (strcasecmp(name, "BUTFIRST") == 0) return eval_butfirst_value(app, args[0]);
    if (strcasecmp(name, "LAST") == 0) return eval_last_value(app, args[0]);
    if (strcasecmp(name, "BUTLAST") == 0) return eval_butlast_value(app, args[0]);
    if (strcasecmp(name, "COUNT") == 0) return eval_count_value(app, args[0]);
    if (strcasecmp(name, "EMPTY?") == 0) return eval_empty_value(args[0]);
    if (strcasecmp(name, "FPUT") == 0) return eval_list_fput(app, args[0], args[1]);
    if (strcasecmp(name, "LPUT") == 0) return eval_list_lput(app, args[0], args[1]);
    if (strcasecmp(name, "WORD") == 0) return eval_word_concat(app, args[0], args[1]);
    if (strcasecmp(name, "SENTENCE") == 0 || strcasecmp(name, "SE") == 0) return eval_list_sentence(app, args[0], args[1]);
    if (strcasecmp(name, "LIST") == 0) return eval_list_wrap_pair(app, args[0], args[1]);
    if (strcasecmp(name, "ARRAY") == 0) return eval_array_value(app, args[0]);
    if (strcasecmp(name, "ITEM") == 0) return eval_item_value(app, args[0], args[1]);
    if (strcasecmp(name, "SETITEM") == 0) {
        eval_setitem_value(app, args[0], args[1], args[2]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "FILLARRAY") == 0) {
        eval_fillarray_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "NAMES") == 0) return eval_names_value(app);
    if (strcasecmp(name, "GETPROP") == 0) return eval_getprop(app, args[0], args[1]);
    if (strcasecmp(name, "SETPROP") == 0) {
        eval_setprop(app, args[0], args[1], args[2]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "REMOVEPROP") == 0) {
        eval_removeprop(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "PROPLIST") == 0) return eval_proplist(app, args[0]);
    if (strcasecmp(name, "NEW") == 0) {
        eval_new_declare(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "FD") == 0 || strcasecmp(name, "FORWARD") == 0) {
        eval_fd_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "BK") == 0 || strcasecmp(name, "BACK") == 0) {
        eval_bk_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "RT") == 0 || strcasecmp(name, "RIGHT") == 0) {
        eval_rt_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "LT") == 0 || strcasecmp(name, "LEFT") == 0) {
        eval_lt_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETXY") == 0) {
        eval_setxy_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETHEADING") == 0 || strcasecmp(name, "SETH") == 0) {
        eval_setheading_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETX") == 0) {
        eval_setx_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETY") == 0) {
        eval_sety_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "GETX") == 0) return do_getx(app);
    if (strcasecmp(name, "GETY") == 0) return do_gety(app);
    if (strcasecmp(name, "HEADING") == 0) return do_heading(app);
    if (strcasecmp(name, "POS") == 0) return do_pos(app);
    if (strcasecmp(name, "CANVASSIZE") == 0) return do_canvassize(app);
    if (strcasecmp(name, "MOUSEPOS") == 0) {
        refresh_before_read(app);
        return eval_list_wrap_pair(app, num_val(app->mouse_x), num_val(app->mouse_y));
    }
    if (strcasecmp(name, "MOUSEX") == 0) {
        refresh_before_read(app);
        return num_val(app->mouse_x);
    }
    if (strcasecmp(name, "MOUSEY") == 0) {
        refresh_before_read(app);
        return num_val(app->mouse_y);
    }
    if (strcasecmp(name, "BUTTON?") == 0) {
        refresh_before_read(app);
        return app->mouse_button_down ? word_val("TRUE") : word_val("FALSE");
    }
    if (strcasecmp(name, "DISTANCE") == 0) return eval_distance_value(app, args[0], args[1]);
    if (strcasecmp(name, "TOWARDS") == 0) return eval_towards_value(app, args[0]);
    if (strcasecmp(name, "PENUP") == 0 || strcasecmp(name, "PU") == 0) {
        do_penup(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "PENDOWN") == 0 || strcasecmp(name, "PD") == 0) {
        do_pendown(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "HOME") == 0) {
        do_home(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETSPEED") == 0) {
        // <= 0 is stored as-is (no clamping to exactly 0), but
        // OP_MOTION_DELAY's own "if delay <= 0, no-op" check treats it
        // identically to 0 either way -- same "don't bother clamping
        // what a later guard already handles" convention as WAIT's own
        // seconds argument.
        app->turtle_speed_delay = eval_to_number(args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SPEED") == 0) return num_val(app->turtle_speed_delay);
    if (strcasecmp(name, "TELL") == 0) {
        eval_tell_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "WHO") == 0) return do_who(app);
    if (strcasecmp(name, "CLEAR") == 0 || strcasecmp(name, "CS") == 0) {
        do_clear(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ARC") == 0) {
        eval_arc_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "CLEAN") == 0) {
        do_clean(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "HIDETURTLE") == 0 || strcasecmp(name, "HT") == 0) {
        do_hideturtle(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SHOWTURTLE") == 0 || strcasecmp(name, "ST") == 0) {
        do_showturtle(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "WRAP") == 0) {
        do_wrap(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "FENCE") == 0) {
        do_fence(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "WINDOW") == 0) {
        do_window(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETPENCOLOR") == 0 || strcasecmp(name, "SETPC") == 0) {
        eval_setpencolor_value(app, args[0], args[1], args[2]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETPENWIDTH") == 0 || strcasecmp(name, "SETPW") == 0) {
        eval_setpenwidth_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETBACKGROUND") == 0 || strcasecmp(name, "SETBG") == 0) {
        eval_setbackground_value(app, args[0], args[1], args[2]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETCANVASSIZE") == 0) {
        eval_setcanvassize_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "LABEL") == 0) {
        eval_label_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "FILL") == 0) {
        do_fill(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "PLOT") == 0) {
        do_plot(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "ERASERECT") == 0) {
        eval_eraserect_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "PROCEDURES") == 0) return do_procedures(app, pool);
    if (strcasecmp(name, "THROW") == 0) {
        eval_throw_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "INT") == 0) return eval_int_value(args[0]);
    if (strcasecmp(name, "ABS") == 0) return eval_abs_value(args[0]);
    if (strcasecmp(name, "SQRT") == 0) return eval_sqrt_value(args[0]);
    if (strcasecmp(name, "POWER") == 0) return eval_power_value(args[0], args[1]);
    if (strcasecmp(name, "RANDOM") == 0) return eval_random_value(args[0]);
    if (strcasecmp(name, "ROUND") == 0) return eval_round_value(args[0]);
    if (strcasecmp(name, "MOD") == 0) return eval_mod_value(args[0], args[1]);
    if (strcasecmp(name, "SIN") == 0) return eval_sin_value(args[0]);
    if (strcasecmp(name, "COS") == 0) return eval_cos_value(args[0]);
    if (strcasecmp(name, "TAN") == 0) return eval_tan_value(args[0]);
    if (strcasecmp(name, "ASIN") == 0) return eval_asin_value(args[0]);
    if (strcasecmp(name, "ACOS") == 0) return eval_acos_value(args[0]);
    if (strcasecmp(name, "ARCTAN") == 0) return eval_arctan_value(args[0]);
    if (strcasecmp(name, "LN") == 0) return eval_ln_value(args[0]);
    if (strcasecmp(name, "LOG") == 0) return eval_log_value(args[0]);
    if (strcasecmp(name, "EXP") == 0) return eval_exp_value(args[0]);
    if (strcasecmp(name, "WORD?") == 0) return eval_wordp_value(args[0]);
    if (strcasecmp(name, "LIST?") == 0) return eval_listp_value(args[0]);
    if (strcasecmp(name, "NUMBER?") == 0) return eval_numberp_value(args[0]);
    if (strcasecmp(name, "ARRAY?") == 0) return eval_arrayp_value(args[0]);
    if (strcasecmp(name, "PICK") == 0) return eval_pick_value(app, args[0]);
    if (strcasecmp(name, "FLATTEN") == 0) return eval_flatten_value(app, args[0]);
    if (strcasecmp(name, "PARSE") == 0) return eval_parse_value(app, args[0]);
    if (strcasecmp(name, "SUBST") == 0) return eval_subst_value(app, args[0], args[1], args[2]);
    if (strcasecmp(name, "DOT") == 0) return eval_dot_value(app, args[0], args[1]);
    if (strcasecmp(name, "CROSS") == 0) return eval_cross_value(app, args[0], args[1]);
    if (strcasecmp(name, "MEMBER?") == 0) return eval_memberp_value(app, args[0], args[1]);
    // MAP/FILTER/REDUCE/FOREACH reach here only when their own
    // template argument wasn't a literal `[...]` visible at compile
    // time (a runtime-computed template) or didn't parse cleanly when
    // compile_template_call tried it -- see compiler.c's own comment.
    // Either way, this is the exact same eval_map_value/eval_filter_value/
    // eval_reduce_value/eval_foreach_value ast_eval itself calls (via
    // do_map/do_filter/do_reduce/do_foreach), so it reproduces every
    // one of their own per-iteration behaviors (including a broken
    // template's own defensive per-element fallback) without this file
    // needing to reimplement any of it.
    if (strcasecmp(name, "MAP") == 0) return eval_map_value(app, args[0], args[1]);
    if (strcasecmp(name, "FILTER") == 0) return eval_filter_value(app, args[0], args[1]);
    if (strcasecmp(name, "REDUCE") == 0) return eval_reduce_value(app, args[0], args[1]);
    if (strcasecmp(name, "FOREACH") == 0) {
        eval_foreach_value(app, args[0], args[1]);
        *produced = 0;
        return num_val(0);
    }
    // Unreachable for a well-formed compiled program -- compile_call
    // only ever emits OP_CALL_BUILTIN for a name find_proc_def couldn't
    // resolve to a user procedure, and the parser itself already
    // guarantees every such name is one of parser.c's own
    // BUILTIN_SIGNATURES entries (see compiler.c's own note). Stay
    // defensive rather than reading uninitialized args, same as
    // compile_expr's own default case.
    *produced = 0;
    return num_val(0);
}

// OP_CALL_PROC -- pops instr->b args (in argument order), pushes a new
// VmFrame + this Vm's own scope (via eval_push_scope_for_call, the
// same setup call_ast_procedure itself uses), and jumps `*pc` to the
// procedure's own compiled body. `chunk->procs[]` is re-resolved here
// at runtime, not trusted from compile time: compile_call only ever
// emits OP_CALL_PROC because `name` resolved to a real AST_PROC_DEF
// *when the program was compiled* -- but ERASE (see
// bytecode_erase_proc) can blank that same entry's own name before
// this instruction actually runs, so a call compiled as "this is a
// procedure" can still resolve to nothing by the time it executes,
// exactly as do_user_procedure_call itself has to handle for the same
// reason (a script can ERASE a procedure then still try to call it).
static void exec_call_proc(Vm *vm, LogoApp *app, BytecodeChunk *chunk, const Instr *instr, int *pc) {
    int argc = instr->b;
    EvalValue args[AST_MAX_PARAMS];
    for (int i = argc - 1; i >= 0; i--) {
        args[i] = (i < AST_MAX_PARAMS) ? pop(vm) : (pop(vm), num_val(0));
    }
    // *pc must always advance past this OP_CALL_PROC on any failure
    // path below -- leaving it unchanged would re-execute the very
    // same call forever (fatal for the recursion-too-deep case, where
    // the caller keeps retrying the same now-permanently-failing
    // call).
    //
    // Looked up by name against chunk->procs[] (self-contained --
    // see docs/BYTECODE_VM_DESIGN.md's "Self-contained BytecodeChunk"
    // entry) rather than find_proc_def(pool, ...) the way this used to:
    // instr->a already holds the backpatched target pc from compile
    // time (this lookup is only for param_count/param_names, needed to
    // bind arguments into scope).
    const ProcAddr *def = bytecode_find_proc_entry(chunk, instr->text);
    if (def == NULL) {
        // Matches do_user_procedure_call's own "unknown procedure"
        // path exactly, including *resolved=0 (here,
        // last_call_resolved) -- this message is already specific
        // enough that OP_CHECK_OUTPUT's own generic one must stay
        // suppressed, not print on top of it.
        append_output(app, "I don't know how to ");
        append_output(app, instr->text);
        append_output(app, "\n");
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    if (vm->frame_count >= MAX_VM_FRAMES) {
        // A VM-internal safety net, not something eval_logo/ast_eval
        // have an analogous case for -- MAX_VM_FRAMES is deliberately
        // larger than MAX_VM_SCOPE_DEPTH (same relationship as the
        // original 256-vs-200 design, just both numbers raised), so
        // eval_push_scope_for_call's own check below always fires
        // first in practice, giving the user its own reported message
        // ("Recursion too deep, call ignored") -- this branch's own
        // silent placeholder-and-continue is a true last-resort only,
        // not the normal way recursion-too-deep gets reported. resolved
        // stays 1 (no more "unresolved" than a real call that happens
        // to recurse very deep).
        vm->last_call_resolved = 1;
        vm->last_call_produced_output = 0;
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    if (!eval_push_scope_for_call(app, vm_scope_stack(vm), def->name, def->param_count, def->param_names, args, argc < AST_MAX_PARAMS ? argc : AST_MAX_PARAMS)) {
        // Recursion too deep -- eval_push_scope_for_call already
        // printed its own message. resolved stays 1 here: unlike the
        // unknown-procedure case, do_user_procedure_call never touches
        // *resolved on this path, so ast_eval genuinely prints BOTH
        // "Recursion too deep, call ignored" AND (in expression
        // position) "X: didn't output a value" -- a real double
        // message this VM has to reproduce, not avoid.
        vm->last_call_resolved = 1;
        vm->last_call_produced_output = 0;
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    VmFrame *frame = &vm->frames[vm->frame_count++];
    frame->return_pc = *pc + 1;
    frame->value_stack_base = vm->stack_top;
    *pc = instr->a;
}

// OP_SEND -- unlike every other call construct in this VM, SEND's own
// callee isn't known until runtime (resolved through obj's own
// prototype chain, via eval_resolve_method -- the exact same function
// do_send itself calls), so it can't be backpatched into a static
// target the way OP_CALL_PROC's own targets are. Once resolved, the
// target procedure's own compiled address is looked up by name in
// `chunk`'s own persistent proc table (bytecode_find_proc -- the same
// {name, start_pc} pairs compiler.c's own backpatching already
// computed while compiling, just kept around instead of discarded),
// and a VmFrame + scope is pushed exactly like OP_CALL_PROC's own
// success path.
//
// Every one of SEND's own error cases (resolution failure, arity
// mismatch, recursion too deep) is handled eagerly here, mirroring
// do_send's own exact wording and control flow -- confirmed directly
// against do_send's own code, not assumed, including the one place
// SEND's own error-suppression genuinely differs from an ordinary
// call's: do_send sets *resolved=0 on EVERY "didn't produce a value"
// outcome, including recursion-too-deep (unlike do_user_procedure_call,
// which leaves *resolved untouched -- and therefore 1 -- on that same
// condition). last_call_resolved/last_send_message are still set even
// on these eager paths (not just the deferred success path below) so
// the OP_CHECK_SEND_OUTPUT that always immediately follows this
// instruction sees consistent state regardless of which path was
// taken.
static void exec_send(Vm *vm, LogoApp *app, BytecodeChunk *chunk, int *pc) {
    EvalValue arglist_val = pop(vm);
    EvalValue msg_val = pop(vm);
    EvalValue obj_val = pop(vm);

    char obj_text[32], msg_text[32];
    eval_value_to_text(app, obj_val, obj_text, sizeof(obj_text));
    eval_value_to_text(app, msg_val, msg_text, sizeof(msg_text));
    snprintf(vm->last_send_message, sizeof(vm->last_send_message), "%s", msg_text);

    // The prototype-chain half (eval_resolve_message) is pure
    // app->plist_entries state, no pool needed -- this replaces
    // eval_resolve_method's own pool-dependent resolution (which went
    // straight to an AstPool node) with the same self-contained
    // chunk->procs[] lookup every other call opcode uses, reproducing
    // eval_resolve_method's own three specific error messages locally
    // (see docs/BYTECODE_VM_DESIGN.md's "Self-contained BytecodeChunk"
    // entry).
    PlistEntry *e = eval_resolve_message(app, obj_text, msg_text);
    if (e == NULL) {
        append_output(app, "SEND: ");
        append_output(app, obj_text);
        append_output(app, " does not understand ");
        append_output(app, msg_text);
        append_output(app, "\n");
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }
    const ProcAddr *def = (e->type == VALUE_WORD) ? bytecode_find_proc_entry(chunk, e->word) : NULL;
    if (def == NULL) {
        append_output(app, "SEND: ");
        append_output(app, msg_text);
        append_output(app, " is not a method on ");
        append_output(app, obj_text);
        append_output(app, "\n");
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }
    if (def->param_count < 1) {
        append_output(app, "SEND: ");
        append_output(app, def->name);
        append_output(app, " must take :self as its first input\n");
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }

    EvalValue arg_vals[AST_MAX_PARAMS];
    int n = eval_send_unpack_args(app, obj_text, arglist_val, arg_vals, AST_MAX_PARAMS);

    if (n != def->param_count) {
        append_output(app, "SEND: wrong number of inputs for message \"");
        append_output(app, msg_text);
        append_output(app, "\n");
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }

    int target_pc = def->start_pc;
    if (vm->frame_count >= MAX_VM_FRAMES) {
        // Can't happen for a well-formed compiled program -- def was
        // just found in chunk->procs[], and compile_program's own pass
        // 1 compiles every AST_PROC_DEF into it. Stay defensive rather
        // than corrupting VM state.
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }
    if (!eval_push_scope_for_call(app, vm_scope_stack(vm), def->name, def->param_count, def->param_names, arg_vals, n)) {
        // Recursion too deep -- eval_push_scope_for_call already
        // printed its own message. resolved is 0 here (not 1, unlike
        // OP_CALL_PROC's own equivalent branch): do_send's own code
        // unconditionally treats "the call didn't produce a value",
        // for any reason including this one, as its own resolved=0
        // case.
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }
    VmFrame *frame = &vm->frames[vm->frame_count++];
    frame->return_pc = *pc + 1;
    frame->value_stack_base = vm->stack_top;
    *pc = target_pc;
}

// OP_APPLY -- direct port of do_apply's own resolution/unpacking logic
// (find_proc_def, not SEND's own prototype-chain eval_resolve_method),
// but pushing a VmFrame + jumping on success exactly like OP_SEND's own
// success path, since this is a real procedure call once resolved.
// Every failure path pushes a throwaway value and advances past this
// instruction instead of jumping -- see bytecode.h's own OP_APPLY
// comment for why the exact pushed value doesn't matter (the
// OP_VOID_DISCARD that always immediately follows this instruction
// throws it away regardless, on every path).
static void exec_apply(Vm *vm, LogoApp *app, BytecodeChunk *chunk, int *pc) {
    EvalValue list_val = pop(vm);
    EvalValue name_val = pop(vm);
    char name_text[64];
    eval_value_to_text(app, name_val, name_text, sizeof(name_text));

    // One lookup instead of the find_proc_def(pool, ...) + separate
    // bytecode_find_proc(chunk, ...) pair this used to need -- see
    // docs/BYTECODE_VM_DESIGN.md's "Self-contained BytecodeChunk" entry.
    const ProcAddr *def = bytecode_find_proc_entry(chunk, name_text);
    if (def == NULL) {
        append_output(app, "APPLY: no such procedure \"");
        append_output(app, name_text);
        append_output(app, "\n");
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }

    EvalValue arg_vals[AST_MAX_PARAMS];
    int n = 0;
    if (list_val.type == VALUE_LIST) {
        for (int idx = list_val.list_head; idx != -1 && n < AST_MAX_PARAMS; idx = app->list_pool[idx].next) {
            arg_vals[n++] = node_to_value(&app->list_pool[idx]);
        }
    } else if (n < AST_MAX_PARAMS) {
        arg_vals[n++] = list_val;
    }

    if (n != def->param_count) {
        append_output(app, "APPLY: wrong number of inputs for procedure \"");
        append_output(app, name_text);
        append_output(app, "\n");
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }

    int target_pc = def->start_pc;
    if (vm->frame_count >= MAX_VM_FRAMES) {
        // Can't happen for a well-formed compiled program -- same
        // defensive reasoning as OP_SEND's own equivalent check.
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    if (!eval_push_scope_for_call(app, vm_scope_stack(vm), def->name, def->param_count, def->param_names, arg_vals, n)) {
        // Recursion too deep -- already printed its own message.
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    VmFrame *frame = &vm->frames[vm->frame_count++];
    frame->return_pc = *pc + 1;
    frame->value_stack_base = vm->stack_top;
    *pc = target_pc;
}

// OP_LAUNCH -- resolves its own procedure name and unpacks its own
// arglist exactly like exec_apply above (bytecode_find_proc_entry, then
// the same list-unpacks-positionally/scalar-becomes-one-arg logic),
// including its own eager "no such procedure"/
// "wrong number of inputs" failure paths (push a throwaway value,
// advance pc, return FALSE: not suspended). Unlike APPLY, a
// *successful* resolution doesn't push a VmFrame and jump itself -- it
// builds the resolved call's own bound Scope directly (via
// eval_push_scope_for_call, the same function OP_CALL_PROC itself uses,
// against a scratch one-slot ScopeStack whose own scope_depth is a
// throwaway local -- this Vm's own real scope_depth is untouched, since
// this scope belongs to the AGENT THIS WILL BECOME, not this call's
// own caller) and returns TRUE (with vm->launch_target_pc/vm->launch_scope/
// vm->pc already set), leaving vm_run's own OP_LAUNCH case to do the
// actual suspend, since only vm_run itself can return a VmRunResult.
// The vm_run_depth guard lives here, first thing after popping
// (matching WAIT's own pop-then-check order), not as a separate check
// in vm_run's case.
static gboolean exec_launch(Vm *vm, LogoApp *app, BytecodeChunk *chunk, int *pc) {
    EvalValue arglist_val = pop(vm);
    EvalValue name_val = pop(vm);
    if (vm->vm_run_depth > 1) {
        append_output(app, "LAUNCH: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
        push(vm, num_val(0));
        *pc = *pc + 1;
        return FALSE;
    }
    char name_text[64];
    eval_value_to_text(app, name_val, name_text, sizeof(name_text));

    // One lookup instead of find_proc_def(pool, ...) + a separate
    // bytecode_find_proc(chunk, ...) -- see docs/BYTECODE_VM_DESIGN.md's
    // "Self-contained BytecodeChunk" entry.
    const ProcAddr *def = bytecode_find_proc_entry(chunk, name_text);
    if (def == NULL) {
        append_output(app, "LAUNCH: no such procedure \"");
        append_output(app, name_text);
        append_output(app, "\n");
        push(vm, num_val(0));
        *pc = *pc + 1;
        return FALSE;
    }

    EvalValue arg_vals[AST_MAX_PARAMS];
    int n = 0;
    if (arglist_val.type == VALUE_LIST) {
        for (int idx = arglist_val.list_head; idx != -1 && n < AST_MAX_PARAMS; idx = app->list_pool[idx].next) {
            arg_vals[n++] = node_to_value(&app->list_pool[idx]);
        }
    } else if (n < AST_MAX_PARAMS) {
        arg_vals[n++] = arglist_val;
    }

    if (n != def->param_count) {
        append_output(app, "LAUNCH: wrong number of inputs for procedure \"");
        append_output(app, name_text);
        append_output(app, "\n");
        push(vm, num_val(0));
        *pc = *pc + 1;
        return FALSE;
    }
    int target_pc = def->start_pc;
    int scratch_depth = 0;
    ScopeStack launch_ss = {&vm->launch_scope, &scratch_depth, 1};
    eval_push_scope_for_call(app, launch_ss, def->name, def->param_count, def->param_names, arg_vals, n); // always succeeds -- capacity 1, scratch_depth starts at 0
    vm->launch_target_pc = target_pc;
    vm->pc = *pc + 1;
    return TRUE;
}

// RUN's own short in-memory snippet budget -- matches eval.c's own
// private MAX_TEMPLATE_TOKENS (not accessible from here) exactly, same
// reasoning: a RUN'd value is ordinarily a short expression/template,
// not a whole external file (see VM_LOAD_MAX_TOKENS below for that).
#define VM_RUN_MAX_TOKENS 128
// LOAD's own whole-file budget -- matches eval.c's own private
// MAX_LOAD_TOKENS exactly.
#define VM_LOAD_MAX_TOKENS 8192

// OP_RUN -- direct port of do_run's own re-entrant lex/parse/exec
// machinery (see eval.c), but compiling into a fresh, independent
// BytecodeChunk and running it via a RECURSIVE vm_run call (sharing
// this same Vm's own stack/frames) instead of a tree-walking
// exec_block -- the same "compile+recursively vm_run a fresh nested
// pool/chunk" mechanism exec_load below also uses. Silently does
// nothing on a lex/parse failure, matching do_run's own exact silence
// (RUN was never given its own diagnostic for malformed code). Capped
// by the shared app->run_depth/MAX_RUN_DEPTH, incremented/decremented
// around the recursive call exactly like do_run itself -- a
// self-referential RUN (MAKE "x [RUN :x] / RUN :x) would otherwise
// blow the C call stack, since each nested RUN is a real recursive
// vm_run call, not a bytecode loop.
//
// frame_floor mitigates the same class of gap MAP/FILTER/REDUCE/
// FOREACH's own recursive-vm_run templates already accept as a
// documented limitation (see exec_map_compiled's own comment): a bare
// OUTPUT/STOP at the RUN'd snippet's own top level (not inside its own
// TO...END) would pop a frame belonging to whatever REAL procedure
// enclosed the whole RUN call -- but unlike a template (whose body is
// compiled inline into the SAME chunk as the rest of the program), the
// popped frame's own return_pc is only meaningful in the OUTER chunk,
// not this recursive call's own freshly-compiled scratch one, so this
// is detected (not fully prevented -- the recursive call has already
// run to whatever conclusion that produced by the time this checks)
// after the fact, same "documented gap, not silent corruption" spirit,
// just a narrower and rarer case to actually hit in practice.
static void exec_run(Vm *vm, LogoApp *app, EvalValue val) {
    if (app->run_depth >= MAX_RUN_DEPTH) {
        append_output(app, "RUN: too deeply nested, ignored\n");
        return;
    }
    char code_text[512];
    eval_value_to_text(app, val, code_text, sizeof(code_text));

    app->run_depth++;
    LogoToken tokens[VM_RUN_MAX_TOKENS];
    int n = logo_lex(code_text, tokens, VM_RUN_MAX_TOKENS);
    if (n >= 0) {
        ParseResult *scratch = calloc(1, sizeof(ParseResult));
        logo_parse(tokens, n, scratch);
        if (scratch->error_count == 0) {
            BytecodeChunk *scratch_chunk = calloc(1, sizeof(BytecodeChunk));
            int start_pc = compile_program(&scratch->pool, scratch->program, scratch_chunk);
            int frame_floor = vm->frame_count;
            vm_run(vm, app, &scratch->pool, scratch_chunk, start_pc);
            if (vm->frame_count < frame_floor) {
                append_output(app, "RUN: OUTPUT/STOP escaping the RUN'd snippet's own top level is not fully supported\n");
            }
            free(scratch_chunk);
        }
        parse_result_destroy(scratch);
    }
    app->run_depth--;
}

// EXECTIME -- exec_run's own mechanism, timed with g_get_monotonic_time
// (microseconds) instead of run for pure side effect, and value-
// returning where RUN itself deliberately isn't (see exec_run's own
// comment on why: RUN/APPLY are commands, not operators). Timing is
// safe to return here specifically because it's EXECTIME's own
// measurement, not something read out of the executed code itself.
// Ported from interpreter.c 2026-08-12 (docs/ROADMAP.md's "Remaining
// old-engine builtins" entry) -- same run_depth guard as exec_run, so
// a self-referential EXECTIME can't blow the C call stack either, and
// the elapsed time is still returned even if the code never actually
// ran (too deeply nested, or a lex/parse failure), matching
// interpreter.c's own "always returns something" behavior.
static EvalValue exec_exectime(Vm *vm, LogoApp *app, EvalValue val) {
    gint64 start = g_get_monotonic_time();
    if (app->run_depth >= MAX_RUN_DEPTH) {
        append_output(app, "EXECTIME: too deeply nested, ignored\n");
        return num_val((double)(g_get_monotonic_time() - start));
    }
    char code_text[512];
    eval_value_to_text(app, val, code_text, sizeof(code_text));

    app->run_depth++;
    LogoToken tokens[VM_RUN_MAX_TOKENS];
    int n = logo_lex(code_text, tokens, VM_RUN_MAX_TOKENS);
    if (n >= 0) {
        ParseResult *scratch = calloc(1, sizeof(ParseResult));
        logo_parse(tokens, n, scratch);
        if (scratch->error_count == 0) {
            BytecodeChunk *scratch_chunk = calloc(1, sizeof(BytecodeChunk));
            int start_pc = compile_program(&scratch->pool, scratch->program, scratch_chunk);
            int frame_floor = vm->frame_count;
            vm_run(vm, app, &scratch->pool, scratch_chunk, start_pc);
            if (vm->frame_count < frame_floor) {
                append_output(app, "EXECTIME: OUTPUT/STOP escaping the timed snippet's own top level is not fully supported\n");
            }
            free(scratch_chunk);
        }
        parse_result_destroy(scratch);
    }
    app->run_depth--;
    return num_val((double)(g_get_monotonic_time() - start));
}

// OP_LOAD -- same mechanism as exec_run above (compile a fresh scratch
// pool/chunk, run it via a recursive vm_run call sharing this Vm's own
// stack/frames), but reading its own source from a file instead of an
// already-evaluated value, and with NO run_depth cap at all -- matching
// do_load's own comment that LOAD, unlike RUN, has never had one.
// `path` is a compile-time-known literal (OP_LOAD's own .text), not a
// runtime value -- LOAD's own ARG_QUOTED_WORD grammar (matching
// interpreter.c's own raw sscanf convention) is the only argument
// shape it can ever syntactically take, so there's no "computed path"
// case to handle. The parser's own eager-LOAD-following pre-pass (see
// parser.c) has already hoisted this same file's own TO...END
// procedures into the main program's AstPool/BytecodeChunk at compile
// time -- this recursive vm_run call's only remaining job is the
// loaded file's own top-level (non-TO) statements, exactly matching
// do_load's own split (its own re-parsed scratch pool's TO...END nodes
// are harmlessly compiled but never actually reached as statements).
static void exec_load(Vm *vm, LogoApp *app, const char *path) {
    char *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        append_output(app, "LOAD: could not read file\n");
        g_error_free(error);
        return;
    }
    LogoToken *tokens = malloc(sizeof(LogoToken) * VM_LOAD_MAX_TOKENS);
    int n = logo_lex(contents, tokens, VM_LOAD_MAX_TOKENS);
    if (n >= 0) {
        ParseResult *scratch = calloc(1, sizeof(ParseResult));
        logo_parse(tokens, n, scratch);
        if (scratch->error_count == 0) {
            BytecodeChunk *scratch_chunk = calloc(1, sizeof(BytecodeChunk));
            int start_pc = compile_program(&scratch->pool, scratch->program, scratch_chunk);
            int frame_floor = vm->frame_count;
            vm_run(vm, app, &scratch->pool, scratch_chunk, start_pc);
            if (vm->frame_count < frame_floor) {
                append_output(app, "LOAD: OUTPUT/STOP escaping the loaded file's own top level is not fully supported\n");
            }
            free(scratch_chunk);
        }
        parse_result_destroy(scratch);
    }
    free(tokens);
    g_free(contents);
}

// OP_OUTPUT/OP_STOP -- pops the current VmFrame (and its matching
// vm->scopes[] scope, in lockstep, per vm.h's own note), truncating
// the value stack back to that frame's own base plus exactly one
// result value: `value` for OP_OUTPUT, num_val(0) for OP_STOP -- the
// same "OUTPUT overrides, falling off the end / STOP defaults to
// num_val(0)" behavior call_ast_procedure's own has_output_value
// handling produces. Jumps `*pc` back to the caller's own return_pc.
// `produced` (1 for OP_OUTPUT, 0 for OP_STOP) is recorded on `vm` for
// the OP_CHECK_OUTPUT that always immediately follows an expression-
// position call to read.
static void exec_return(Vm *vm, EvalValue value, int produced, int *pc) {
    vm->last_call_produced_output = produced;
    vm->last_call_resolved = 1; // a call that reached its own OUTPUT/STOP was always resolved
    if (vm->frame_count <= 0) {
        // OUTPUT/STOP at the top level (no enclosing call) -- can't
        // happen for a well-formed compiled program (the compiler only
        // ever emits these inside a procedure body), so just fall
        // through to OP_HALT's own pc rather than corrupting state.
        *pc = -1;
        return;
    }
    VmFrame *frame = &vm->frames[--vm->frame_count];
    vm->stack_top = frame->value_stack_base;
    push(vm, value);
    vm->scope_depth--;
    *pc = frame->return_pc;
}

// Binds `name` to `v` as an ordinary Logo variable -- the exact same
// per-type dispatch OP_SET_VAR's own case in vm_run below uses, pulled
// out here so all four exec_*_compiled functions can share it.
static void bind_template_var(Vm *vm, LogoApp *app, const char *name, EvalValue v) {
    ScopeStack ss = vm_scope_stack(vm);
    if (v.type == VALUE_WORD) set_var_word(app, ss, name, v.word);
    else if (v.type == VALUE_LIST) set_var_list(app, ss, name, v.list_head);
    else if (v.type == VALUE_ARRAY) set_var_array(app, ss, name, v.list_head, (int)v.number);
    else set_var(app, ss, name, v.number);
}

// A placeholder variable's own name is unique per COMPILE-TIME call
// site (see compiler.c's own Compiler.template_counter), which rules
// out two DIFFERENT MAP/FILTER/REDUCE/FOREACH calls ever colliding --
// but the SAME call site can still be reached twice at once through
// recursion (a template that, for one element, calls a procedure which
// itself recurses back into the very same MAP/FILTER/REDUCE/FOREACH
// call), and unconditionally deleting the placeholder once the whole
// loop finishes is wrong for that case: the inner (recursive)
// invocation's own cleanup would delete the variable out from under
// the OUTER invocation, which is still mid-flight, still holding a
// pending "?" read for its own current element. SavedVar/save_var/
// restore_var fix this the same way the FOR loop's own bug was fixed
// last batch -- state that must survive a recursive reentry of the
// same compiled construct can't live in a single shared slot that's
// just written-then-read; it needs real nesting. Here, that nesting
// comes for free from the C call stack itself: each exec_*_compiled
// invocation's own `saved` is a C-local, so save/restore naturally
// nest correctly across however many recursive reentries happen, with
// no new opcode or stack-depth tracking required (contrast FOR's own
// fix, which needed OP_PEEK/OP_POKE precisely because ITS persistent
// state had no equivalent natural C-stack home). Confirmed against a
// real recursive-template test in test_vm.c, not just reasoned through
// -- an earlier version of these functions unconditionally called
// eval_delete_var at the end of the loop instead, which is exactly the
// bug this replaces.
typedef struct {
    int existed;
    EvalValue value;
} SavedVar;

static SavedVar save_var(Vm *vm, LogoApp *app, const char *name) {
    SavedVar sv;
    Variable *v = find_var(app, vm_scope_stack(vm), name);
    sv.existed = (v != NULL);
    if (v == NULL) { sv.value = num_val(0); return sv; }
    if (v->type == VALUE_WORD) sv.value = word_val(v->word);
    else if (v->type == VALUE_LIST) sv.value = list_val(v->list_head);
    else if (v->type == VALUE_ARRAY) sv.value = array_val(v->list_head, (int)v->number);
    else sv.value = num_val(v->number);
    return sv;
}

static void restore_var(Vm *vm, LogoApp *app, const char *name, SavedVar sv) {
    if (sv.existed) bind_template_var(vm, app, name, sv.value);
    else eval_delete_var(app, name);
}

// A list literal's own elements are always internally VALUE_WORD (see
// node_to_value/eval_build_list_literal -- a list literal never
// contains a LIST_ELEM_NUMBER, only LIST_ELEM_WORD, even for a
// numeric-looking element like "1"). eval.c's own runtime template
// mechanism (eval_apply_template_expr/eval_apply_template_condition/
// eval_reduce_value/eval_foreach_value) never notices this, because it
// substitutes the element's own TEXT into the template and re-lexes
// the whole thing from scratch -- and the lexer's own number-scanning
// rule turns a numeric-looking token into a genuine LOGO_TOK_NUMBER
// (an AST_NUMBER, evaluating to a real VALUE_NUMBER) purely from its
// own spelling, regardless of what internal type tag the original list
// element happened to carry. This VM's own placeholder binding has no
// re-lex step to do that promotion implicitly, so it has to replicate
// it explicitly here, or every ordering comparison against a
// placeholder (OP_CMP_GT/LT/LE/GE -- exec_compare's own non-numeric
// fallback is unconditionally 0 for anything that isn't ALREADY
// VALUE_NUMBER, unlike arithmetic's own eval_to_number, which coerces
// any numeric-looking word regardless) silently reads every element as
// "not a number" and always fails. Confirmed as a real bug, not
// guessed: test_filter_keeps_matching_elements (`FILTER [? > 2]
// [1 2 3 4]`) filtered out every element before this fix.
static EvalValue coerce_template_element(EvalValue v) {
    if (v.type == VALUE_WORD && v.word[0] != '\0') {
        char *end;
        double n = strtod(v.word, &end);
        if (*end == '\0') return num_val(n);
    }
    return v;
}

// OP_MAP_COMPILED/OP_FILTER_COMPILED/OP_REDUCE_COMPILED/
// OP_FOREACH_COMPILED -- see compiler.c's own compile_template_call
// for the full design rationale (why a real per-call-site-unique
// variable binding instead of eval.c's own runtime
// text-substitute-and-reparse, why the template's own body had to be
// grafted into the main AstPool and compiled inline into this same
// chunk). Each of the four mirrors its own eval_X_value (eval.c)
// node-for-node, just sourcing each iteration's transformed
// value/truthiness/effect from a RECURSIVE vm_run call into
// `instr->a` (the template's own compiled start pc, reached no other
// way -- ending in OP_HALT, whose own plain C `return;` correctly
// scopes back to just that one recursive call, since `pc` is a local
// variable each invocation) instead of eval_apply_template_expr/
// eval_apply_template_condition/inline substitution.
//
// `frame_floor` (recorded before the loop, checked after every
// recursive vm_run call) is a defensive, deliberately narrow
// mitigation for OUTPUT/STOP executed DIRECTLY inside a template body
// (not through a further nested real procedure call): such a call
// would pop the frame of whichever REAL procedure is enclosing this
// whole MAP/FILTER/REDUCE/FOREACH call (via exec_return), but C's own
// call/return means control still returns right here, to this loop,
// rather than further up where that frame actually belonged -- popping
// out of the loop early the moment that's detected is a fail-safe, not
// a real fix (the tree-walker has its own analogous quirk here: e.g.
// do_map's own loop never checks stop_requested at all).
static void exec_map_compiled(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const Instr *instr) {
    EvalValue list_arg = pop(vm);
    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    SavedVar saved = save_var(vm, app, instr->text);
    int new_head = -1;
    int *next_slot = &new_head;
    int frame_floor = vm->frame_count;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(vm, app, instr->text, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        EvalValue result = pop(vm);
        int node = value_to_node(app, result);
        if (node < 0) {
            restore_var(vm, app, instr->text, saved);
            push(vm, list_pool_exhausted(app));
            return;
        }
        *next_slot = node;
        next_slot = &app->list_pool[node].next;
    }
    restore_var(vm, app, instr->text, saved);
    push(vm, list_val(new_head));
}

static void exec_filter_compiled(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const Instr *instr) {
    EvalValue list_arg = pop(vm);
    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    SavedVar saved = save_var(vm, app, instr->text);
    int new_head = -1;
    int *next_slot = &new_head;
    int frame_floor = vm->frame_count;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(vm, app, instr->text, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        EvalValue result = pop(vm);
        if (eval_is_truthy(result)) {
            int node = list_node_copy(app, idx);
            if (node < 0) {
                restore_var(vm, app, instr->text, saved);
                push(vm, list_pool_exhausted(app));
                return;
            }
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    restore_var(vm, app, instr->text, saved);
    push(vm, list_val(new_head));
}

// REDUCE gets two placeholder variables ("<base>_1"/"<base>_2" for
// "?1"/"?2", the accumulator and the current element) derived from
// `instr->text` (the base name compile_template_call generated) --
// this suffixing convention must stay in sync with compiler.c's own
// (documented there too).
static void exec_reduce_compiled(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const Instr *instr) {
    EvalValue list_arg = pop(vm);
    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    if (iter_head == -1) {
        push(vm, num_val(0)); // nothing to reduce -- matches eval_reduce_value's own early return
        return;
    }
    char var1[40], var2[40];
    snprintf(var1, sizeof(var1), "%s_1", instr->text);
    snprintf(var2, sizeof(var2), "%s_2", instr->text);
    SavedVar saved1 = save_var(vm, app, var1);
    SavedVar saved2 = save_var(vm, app, var2);
    EvalValue acc = coerce_template_element(node_to_value(&app->list_pool[iter_head]));
    int frame_floor = vm->frame_count;
    for (int idx = app->list_pool[iter_head].next; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(vm, app, var1, acc);
        bind_template_var(vm, app, var2, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        acc = pop(vm);
    }
    restore_var(vm, app, var1, saved1);
    restore_var(vm, app, var2, saved2);
    push(vm, acc);
}

static void exec_foreach_compiled(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const Instr *instr) {
    EvalValue list_arg = pop(vm);
    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    SavedVar saved = save_var(vm, app, instr->text);
    int frame_floor = vm->frame_count;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(vm, app, instr->text, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        if (app->stop_requested || app->throw_requested) break; // matches do_foreach's own check
    }
    restore_var(vm, app, instr->text, saved);
    vm->last_call_produced_output = 0;
    vm->last_call_resolved = 1; // FOREACH never "outputs" a value -- same as OP_VOID_RESULT
    push(vm, num_val(0));
}

VmRunResult vm_run(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, int start_pc) {
    int pc = start_pc;
    vm->vm_run_depth++;
    while (pc >= 0 && pc < chunk->count) {
        const Instr *instr = &chunk->code[pc];
        switch (instr->op) {
            case OP_PUSH_NUMBER:
                push(vm, num_val(instr->number));
                pc++;
                break;
            case OP_PUSH_WORD:
                // .a is an index into chunk->word_literals (see
                // bytecode.h) -- -1 only if compile-time
                // bytecode_add_word_literal ever overflowed
                // MAX_CHUNK_WORD_LITERALS, which pushes an empty word
                // rather than reading out of bounds.
                push(vm, word_val(instr->a >= 0 ? chunk->word_literals[instr->a] : ""));
                pc++;
                break;
            case OP_PUSH_VAR: {
                // Mirrors eval_expr's own AST_VARREF case exactly,
                // including "unset :name reads as 0".
                Variable *v = find_var(app, vm_scope_stack(vm), instr->text);
                if (v == NULL) push(vm, num_val(0));
                else if (v->type == VALUE_WORD) push(vm, word_val(v->word));
                else if (v->type == VALUE_NUMBER) push(vm, num_val(v->number));
                else if (v->type == VALUE_LIST) push(vm, list_val(v->list_head));
                else push(vm, array_val(v->list_head, (int)v->number));
                pc++;
                break;
            }
            case OP_SET_VAR: {
                EvalValue v = pop(vm);
                ScopeStack ss = vm_scope_stack(vm);
                if (v.type == VALUE_WORD) set_var_word(app, ss, instr->text, v.word);
                else if (v.type == VALUE_LIST) set_var_list(app, ss, instr->text, v.list_head);
                else if (v.type == VALUE_ARRAY) set_var_array(app, ss, instr->text, v.list_head, (int)v.number);
                else set_var(app, ss, instr->text, v.number);
                pc++;
                break;
            }
            case OP_POP:
                pop(vm);
                pc++;
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
                exec_arith(vm, instr->op);
                pc++;
                break;
            case OP_NEG:
                push(vm, num_val(-eval_to_number(pop(vm))));
                pc++;
                break;
            case OP_CMP_LT: case OP_CMP_GT: case OP_CMP_EQ:
            case OP_CMP_LE: case OP_CMP_GE: case OP_CMP_NE:
                exec_compare(app, vm, instr->op);
                pc++;
                break;
            case OP_NOT:
                push(vm, num_val(!eval_is_truthy(pop(vm))));
                pc++;
                break;
            case OP_AND: {
                // No short-circuiting -- both operands were already
                // unconditionally pushed by the compiler, matching
                // eval_condition's own AST_AND case.
                int right = eval_is_truthy(pop(vm));
                int left = eval_is_truthy(pop(vm));
                push(vm, num_val(left && right));
                pc++;
                break;
            }
            case OP_OR: {
                int right = eval_is_truthy(pop(vm));
                int left = eval_is_truthy(pop(vm));
                push(vm, num_val(left || right));
                pc++;
                break;
            }
            case OP_JUMP:
                pc = instr->a;
                break;
            case OP_JUMP_IF_FALSE:
                pc = eval_is_truthy(pop(vm)) ? pc + 1 : instr->a;
                break;
            case OP_CALL_BUILTIN: {
                int argc = instr->a;
                EvalValue args[AST_MAX_PARAMS];
                for (int i = argc - 1; i >= 0; i--) {
                    args[i] = (i < AST_MAX_PARAMS) ? pop(vm) : (pop(vm), num_val(0));
                }
                int produced;
                EvalValue result = call_builtin(vm, app, pool, chunk, instr->text, args, &produced);
                vm->last_call_produced_output = produced;
                vm->last_call_resolved = 1; // an ordinary builtin call is always "resolved" in ast_eval's own sense
                push(vm, result);
                pc++;
                break;
            }
            case OP_CALL_PROC:
                exec_call_proc(vm, app, chunk, instr, &pc);
                break;
            case OP_CHECK_OUTPUT: {
                // Only ever emitted right after a call construct used
                // in expression position. That call already left
                // exactly one value on the stack; if it actually
                // resolved to something (vm->last_call_resolved -- 0
                // only for a call to a since-ERASEd/never-defined
                // procedure, which already printed its own more
                // specific "I don't know how to X" and must NOT also
                // get this generic message) but didn't produce a real
                // value (vm->last_call_produced_output), replace it
                // with word_val("") and report the same diagnostic
                // eval_expr's own AST_CALL case does.
                if (vm->last_call_resolved && !vm->last_call_produced_output) {
                    append_output(app, instr->text);
                    append_output(app, ": didn't output a value\n");
                    pop(vm);
                    push(vm, word_val(""));
                }
                pc++;
                break;
            }
            case OP_OUTPUT:
                exec_return(vm, pop(vm), /*produced=*/1, &pc);
                break;
            case OP_STOP:
                exec_return(vm, num_val(0), /*produced=*/0, &pc);
                break;
            case OP_HALT:
                vm->vm_run_depth--;
                return VM_RUN_HALTED;
            case OP_PUSH_LIST_LITERAL:
                // See bytecode.h's own list_literals[] comment: .a
                // indexes chunk's own rendered source text now, not an
                // AST node -- built fresh each visit exactly like
                // eval_expr's own AST_LIST_LITERAL case does.
                push(vm, eval_build_list_literal_from_text(app, chunk->list_literals[instr->a]));
                pc++;
                break;
            case OP_LOCAL:
                eval_local_declare(app, vm_scope_stack(vm), instr->text);
                pc++;
                break;
            case OP_ERASE:
                // Blanks both copies of this procedure's own identity:
                // the AST_PROC_DEF node itself (still needed for
                // TEXT/SAVE/SHOW, which stay pool-dependent -- see
                // docs/BYTECODE_VM_DESIGN.md's "Self-contained
                // BytecodeChunk" entry) and chunk->procs[] (needed by
                // every OP_CALL_PROC/OP_SEND/OP_APPLY/OP_LAUNCH lookup
                // now that none of them touch pool anymore).
                eval_erase_declare(app, pool, instr->text);
                bytecode_erase_proc(chunk, instr->text);
                pc++;
                break;
            case OP_SEND:
                exec_send(vm, app, chunk, &pc);
                break;
            case OP_CHECK_SEND_OUTPUT: {
                // Same job as OP_CHECK_OUTPUT, but the name to report
                // comes from vm->last_send_message (a runtime value
                // exec_send itself just resolved), not from a
                // compile-time .text field -- see bytecode.h's own
                // comment on why SEND can't use the ordinary opcode.
                if (vm->last_call_resolved && !vm->last_call_produced_output) {
                    append_output(app, vm->last_send_message);
                    append_output(app, ": didn't output a value\n");
                    pop(vm);
                    push(vm, word_val(""));
                }
                pc++;
                break;
            }
            case OP_APPLY:
                exec_apply(vm, app, chunk, &pc);
                break;
            case OP_VOID_DISCARD:
                pop(vm);
                vm->last_call_produced_output = 0;
                vm->last_call_resolved = 1; // APPLY is always "resolved" -- never the unknown-procedure diagnostic case, matching do_apply's own unconditional void return
                push(vm, num_val(0));
                pc++;
                break;
            case OP_RUN:
                exec_run(vm, app, pop(vm));
                pc++;
                break;
            case OP_EXECTIME:
                push(vm, exec_exectime(vm, app, pop(vm)));
                vm->last_call_produced_output = 1; // always a real value (elapsed time), unlike RUN
                vm->last_call_resolved = 1; // same "an ordinary call is always resolved" convention OP_CALL_BUILTIN uses
                pc++;
                break;
            case OP_LOAD:
                exec_load(vm, app, instr->text);
                pc++;
                break;
            case OP_VOID_RESULT:
                vm->last_call_produced_output = 0;
                vm->last_call_resolved = 1; // MAKE/LOCAL/ERASE/WHILE/CATCH are always "resolved" -- never the unknown-procedure case
                push(vm, num_val(0));
                pc++;
                break;
            case OP_CHECK_THROW:
                // See bytecode.h's own comment and compiler.c's own
                // compile_block: skips the rest of the CURRENT block
                // only (jumping to instr->a, that block's own end),
                // mirroring exec_block's own cooperative
                // "if (throw_requested) break". Composed across nested
                // blocks (via each one's own OP_CHECK_THROW instances)
                // and the auto-appended OP_STOP at the end of every
                // procedure body, this is what actually propagates an
                // uncaught throw all the way up to whichever CATCH (or
                // the top level's own OP_CHECK_UNCAUGHT_THROW) stops it.
                pc = app->throw_requested ? instr->a : pc + 1;
                break;
            case OP_CATCH_CHECK: {
                // The tag value has been sitting on the stack since
                // compile_call's own CATCH branch pushed it, before the
                // block even ran -- see bytecode.h's own comment on why
                // it isn't a VM-level scratch field (nested CATCH would
                // clobber it there).
                char tag_text[64];
                eval_value_to_text(app, pop(vm), tag_text, sizeof(tag_text));
                eval_catch_check(app, tag_text);
                pc++;
                break;
            }
            case OP_CHECK_UNCAUGHT_THROW:
                if (app->throw_requested) eval_report_uncaught_throw(app);
                pc++;
                break;
            case OP_PEEK:
                push(vm, peek(vm, instr->a));
                pc++;
                break;
            case OP_POKE:
                poke(vm, instr->a, pop(vm));
                pc++;
                break;
            case OP_REPCOUNT_PUSH:
                if (vm->repcount_depth < MAX_VM_SCOPE_DEPTH) vm->repcount_stack[vm->repcount_depth++] = 1;
                pc++;
                break;
            case OP_REPCOUNT_INCR:
                if (vm->repcount_depth > 0) vm->repcount_stack[vm->repcount_depth - 1] += 1;
                pc++;
                break;
            case OP_REPCOUNT_POP:
                if (vm->repcount_depth > 0) vm->repcount_depth--;
                pc++;
                break;
            case OP_MAP_COMPILED:
                exec_map_compiled(vm, app, pool, chunk, instr);
                vm->last_call_produced_output = 1;
                vm->last_call_resolved = 1;
                pc++;
                break;
            case OP_FILTER_COMPILED:
                exec_filter_compiled(vm, app, pool, chunk, instr);
                vm->last_call_produced_output = 1;
                vm->last_call_resolved = 1;
                pc++;
                break;
            case OP_REDUCE_COMPILED:
                exec_reduce_compiled(vm, app, pool, chunk, instr);
                vm->last_call_produced_output = 1;
                vm->last_call_resolved = 1;
                pc++;
                break;
            case OP_FOREACH_COMPILED:
                exec_foreach_compiled(vm, app, pool, chunk, instr); // sets its own last_call_* fields, same as OP_VOID_RESULT
                pc++;
                break;
            case OP_WAIT: {
                double seconds = eval_to_number(pop(vm));
                if (vm->vm_run_depth > 1) {
                    append_output(app, "WAIT: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    pc++;
                    break;
                }
                if (seconds > 0) {
                    vm->pc = pc + 1; // the OP_VOID_RESULT that compile_call always emits right after OP_WAIT
                    vm->suspend_seconds = seconds;
                    vm->vm_run_depth--;
                    return VM_RUN_SUSPENDED_WAIT;
                }
                pc++;
                break;
            }
            case OP_WAITKEY: {
                if (vm->vm_run_depth > 1) {
                    append_output(app, "WAITKEY: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    push(vm, word_val(""));
                    vm->last_call_produced_output = 1;
                    vm->last_call_resolved = 1;
                    pc++;
                    break;
                }
                vm->pc = pc + 1;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_WAITKEY;
            }
            case OP_INPUT: {
                if (vm->vm_run_depth > 1) {
                    append_output(app, "INPUT: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    push(vm, word_val(""));
                    vm->last_call_produced_output = 1;
                    vm->last_call_resolved = 1;
                    pc++;
                    break;
                }
                vm->pc = pc + 1;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_INPUT;
            }
            case OP_PAUSE: {
                if (vm->vm_run_depth > 1) {
                    append_output(app, "PAUSE: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    pc++;
                    break;
                }
                int my_level = ++app->pause_depth;
                char msg[64];
                snprintf(msg, sizeof(msg), "Paused (level %d). Type CONTINUE to resume.\n", my_level);
                append_output(app, msg);
                vm->pc = pc + 1;
                vm->pause_level = my_level;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_PAUSE;
            }
            case OP_ANIMATESPRITE: {
                int frames = (int)eval_to_number(pop(vm));
                double delay = eval_to_number(pop(vm));
                if (vm->vm_run_depth > 1) {
                    append_output(app, "ANIMATESPRITE: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    pc++;
                    break;
                }
                Turtle *t = current_turtle(app);
                if (t->sprite_index < 0) {
                    append_output(app, "ANIMATESPRITE: no sprite set (use SETSPRITE first)\n");
                    pc++;
                    break;
                }
                if (frames <= 0) {
                    pc++;
                    break;
                }
                int frame_count = app->sprite_frame_cols[t->sprite_index] * app->sprite_frame_rows[t->sprite_index];
                if (delay <= 0) {
                    // No suspend at all -- matches interpreter.c's own
                    // loop, which never actually yields to GTK's main
                    // loop when delay <= 0, so intermediate frames were
                    // never visible there either, just an instant jump
                    // to the final frame.
                    for (int i = 0; i < frames; i++) {
                        t->sprite_frame = (t->sprite_frame + 1) % frame_count;
                    }
                    pc++;
                    break;
                }
                t->sprite_frame = (t->sprite_frame + 1) % frame_count;
                vm->pc = pc + 1;
                vm->suspend_seconds = delay;
                vm->suspend_frames_remaining = frames - 1;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_ANIMATESPRITE;
            }
            case OP_LAUNCH:
                if (exec_launch(vm, app, chunk, &pc)) {
                    vm->vm_run_depth--;
                    return VM_RUN_SUSPENDED_LAUNCH;
                }
                break;
            case OP_AWAIT: {
                if (vm->vm_run_depth > 1) {
                    append_output(app, "AWAIT: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    pc++;
                    break;
                }
                vm->pc = pc + 1;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_AWAIT;
            }
            case OP_YIELD: {
                if (vm->vm_run_depth > 1) {
                    append_output(app, "YIELD: not supported inside a MAP/FILTER/REDUCE/FOREACH template, RUN, or LOAD\n");
                    pc++;
                    break;
                }
                vm->pc = pc + 1;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_YIELD;
            }
            case OP_MOTION_DELAY: {
                // Silent no-op, not a reported refusal -- see this
                // opcode's own bytecode.h comment for why depth>1 (and
                // an unset/zero speed) skip quietly here, unlike every
                // other suspend opcode's vm_run_depth>1 check.
                if (vm->vm_run_depth > 1 || app->turtle_speed_delay <= 0) {
                    pc++;
                    break;
                }
                vm->pc = pc + 1;
                vm->suspend_seconds = app->turtle_speed_delay;
                vm->vm_run_depth--;
                return VM_RUN_SUSPENDED_MOTION_DELAY;
            }
            default:
                pc++;
                break;
        }
    }
    vm->vm_run_depth--;
    return VM_RUN_HALTED;
}

VmRunResult vm_resume(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk) {
    return vm_run(vm, app, pool, chunk, vm->pc);
}

VmRunResult vm_resume_with_key(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const char *key_name) {
    push(vm, word_val(key_name));
    vm->last_call_produced_output = 1;
    vm->last_call_resolved = 1;
    return vm_run(vm, app, pool, chunk, vm->pc);
}

VmRunResult vm_resume_with_input(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const char *line) {
    push(vm, word_val(line));
    vm->last_call_produced_output = 1;
    vm->last_call_resolved = 1;
    return vm_run(vm, app, pool, chunk, vm->pc);
}

VmRunResult vm_resume_animatesprite(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk) {
    if (vm->suspend_frames_remaining <= 0) {
        return vm_run(vm, app, pool, chunk, vm->pc);
    }
    Turtle *t = current_turtle(app);
    int frame_count = app->sprite_frame_cols[t->sprite_index] * app->sprite_frame_rows[t->sprite_index];
    t->sprite_frame = (t->sprite_frame + 1) % frame_count;
    vm->suspend_frames_remaining--;
    return VM_RUN_SUSPENDED_ANIMATESPRITE;
}
