// vm.c
//
// See vm.h for the frame-layout decision and rationale. The dispatch
// loop below is deliberately a flat switch over an explicit program
// counter (`pc`), not recursive C function calls the way eval.c's own
// exec_block/eval_expr are -- that's the entire point of Stage 2 (see
// docs/BYTECODE_VM_DESIGN.md's own "Why"): a Logo-level call becomes a
// VmFrame push and a `pc` jump, not a new C stack frame, so Logo-level
// recursion depth stops being coupled to C stack depth (still coupled
// to app->scopes[]/MAX_SCOPE_DEPTH for this vertical slice -- see
// vm.h's own note -- but no longer to the C stack at all).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

// The five ordinary (non-suspending) sprite commands -- direct ports of
// interpreter.c's own do_loadsprite/do_loadspritesheet/do_setsprite/
// do_setspriteframe/do_stampsprite, deliberately vm.c-only (not also
// added to eval.c/ast_eval, the same scope the user chose for
// WAIT/WAITKEY/INPUT/PAUSE): bin/logo no longer runs on ast_eval at
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

static EvalValue call_builtin(LogoApp *app, AstPool *pool, const char *name, EvalValue *args, int *produced) {
    *produced = 1;
    if (strcasecmp(name, "PRINT") == 0) {
        eval_print_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "CONTINUE") == 0 || strcasecmp(name, "CO") == 0) {
        exec_continue(app);
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
    if (strcasecmp(name, "OPENREAD") == 0) return eval_openread_value(app, args[0]);
    if (strcasecmp(name, "OPENWRITE") == 0) return eval_openwrite_value(app, args[0]);
    if (strcasecmp(name, "OPENAPPEND") == 0) return eval_openappend_value(app, args[0]);
    if (strcasecmp(name, "READLINE") == 0) return eval_readline_value(app, args[0]);
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
    if (strcasecmp(name, "THING") == 0) return eval_thing_value(app, args[0]);
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
    if (strcasecmp(name, "SETHEADING") == 0) {
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
    if (strcasecmp(name, "HIDETURTLE") == 0) {
        do_hideturtle(app);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SHOWTURTLE") == 0) {
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
    if (strcasecmp(name, "SETPENCOLOR") == 0) {
        eval_setpencolor_value(app, args[0], args[1], args[2]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETPENWIDTH") == 0) {
        eval_setpenwidth_value(app, args[0]);
        *produced = 0;
        return num_val(0);
    }
    if (strcasecmp(name, "SETBACKGROUND") == 0) {
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
// VmFrame + app->scopes[] scope (via eval_push_scope_for_call, the
// same setup call_ast_procedure itself uses), and jumps `*pc` to the
// procedure's own compiled body. `find_proc_def` is re-resolved here
// at runtime, not trusted from compile time: compile_call only ever
// emits OP_CALL_PROC because `name` resolved to a real AST_PROC_DEF
// *when the program was compiled* -- but ERASE (see eval_erase_declare)
// can blank that same node's own text before this instruction actually
// runs, so a call compiled as "this is a procedure" can still resolve
// to nothing by the time it executes, exactly as do_user_procedure_call
// itself has to handle for the same reason (a script can ERASE a
// procedure then still try to call it).
static void exec_call_proc(Vm *vm, LogoApp *app, AstPool *pool, const Instr *instr, int *pc) {
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
    int def_node = find_proc_def(pool, instr->text);
    if (def_node < 0) {
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
        // have an analogous case for -- MAX_VM_FRAMES (256) is
        // deliberately larger than MAX_SCOPE_DEPTH (200), so
        // eval_push_scope_for_call's own check below always fires
        // first in practice. resolved stays 1 (no more "unresolved"
        // than a real call that happens to recurse very deep).
        vm->last_call_resolved = 1;
        vm->last_call_produced_output = 0;
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    AstNode *def = &pool->nodes[def_node];
    if (!eval_push_scope_for_call(app, def, args, argc < AST_MAX_PARAMS ? argc : AST_MAX_PARAMS)) {
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
static void exec_send(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, int *pc) {
    EvalValue arglist_val = pop(vm);
    EvalValue msg_val = pop(vm);
    EvalValue obj_val = pop(vm);

    char obj_text[32], msg_text[32];
    eval_value_to_text(app, obj_val, obj_text, sizeof(obj_text));
    eval_value_to_text(app, msg_val, msg_text, sizeof(msg_text));
    snprintf(vm->last_send_message, sizeof(vm->last_send_message), "%s", msg_text);

    int def_node = eval_resolve_method(app, pool, obj_text, msg_text); // prints its own specific "does not understand"/"is not a method on"/"must take :self" error
    if (def_node < 0) {
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }
    AstNode *def = &pool->nodes[def_node];

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

    int target_pc = bytecode_find_proc(chunk, def->text);
    if (target_pc < 0 || vm->frame_count >= MAX_VM_FRAMES) {
        // Can't happen for a well-formed compiled program -- def_node
        // was just found as a real AST_PROC_DEF, and compile_program's
        // own pass 1 compiles every one of those into chunk->procs.
        // Stay defensive rather than corrupting VM state.
        vm->last_call_resolved = 0;
        vm->last_call_produced_output = 0;
        push(vm, word_val(""));
        *pc = *pc + 1;
        return;
    }
    if (!eval_push_scope_for_call(app, def, arg_vals, n)) {
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

// OP_OUTPUT/OP_STOP -- pops the current VmFrame (and its matching
// app->scopes[] scope, in lockstep, per vm.h's own note), truncating
// the value stack back to that frame's own base plus exactly one
// result value: `value` for OP_OUTPUT, num_val(0) for OP_STOP -- the
// same "OUTPUT overrides, falling off the end / STOP defaults to
// num_val(0)" behavior call_ast_procedure's own has_output_value
// handling produces. Jumps `*pc` back to the caller's own return_pc.
// `produced` (1 for OP_OUTPUT, 0 for OP_STOP) is recorded on `vm` for
// the OP_CHECK_OUTPUT that always immediately follows an expression-
// position call to read.
static void exec_return(Vm *vm, LogoApp *app, EvalValue value, int produced, int *pc) {
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
    app->scope_depth--;
    *pc = frame->return_pc;
}

// Binds `name` to `v` as an ordinary Logo variable -- the exact same
// per-type dispatch OP_SET_VAR's own case in vm_run below uses, pulled
// out here so all four exec_*_compiled functions can share it.
static void bind_template_var(LogoApp *app, const char *name, EvalValue v) {
    if (v.type == VALUE_WORD) set_var_word(app, name, v.word);
    else if (v.type == VALUE_LIST) set_var_list(app, name, v.list_head);
    else if (v.type == VALUE_ARRAY) set_var_array(app, name, v.list_head, (int)v.number);
    else set_var(app, name, v.number);
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

static SavedVar save_var(LogoApp *app, const char *name) {
    SavedVar sv;
    Variable *v = find_var(app, name);
    sv.existed = (v != NULL);
    if (v == NULL) { sv.value = num_val(0); return sv; }
    if (v->type == VALUE_WORD) sv.value = word_val(v->word);
    else if (v->type == VALUE_LIST) sv.value = list_val(v->list_head);
    else if (v->type == VALUE_ARRAY) sv.value = array_val(v->list_head, (int)v->number);
    else sv.value = num_val(v->number);
    return sv;
}

static void restore_var(LogoApp *app, const char *name, SavedVar sv) {
    if (sv.existed) bind_template_var(app, name, sv.value);
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
    SavedVar saved = save_var(app, instr->text);
    int new_head = -1;
    int *next_slot = &new_head;
    int frame_floor = vm->frame_count;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(app, instr->text, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        EvalValue result = pop(vm);
        int node = value_to_node(app, result);
        if (node < 0) {
            restore_var(app, instr->text, saved);
            push(vm, list_pool_exhausted(app));
            return;
        }
        *next_slot = node;
        next_slot = &app->list_pool[node].next;
    }
    restore_var(app, instr->text, saved);
    push(vm, list_val(new_head));
}

static void exec_filter_compiled(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const Instr *instr) {
    EvalValue list_arg = pop(vm);
    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    SavedVar saved = save_var(app, instr->text);
    int new_head = -1;
    int *next_slot = &new_head;
    int frame_floor = vm->frame_count;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(app, instr->text, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        EvalValue result = pop(vm);
        if (eval_is_truthy(result)) {
            int node = list_node_copy(app, idx);
            if (node < 0) {
                restore_var(app, instr->text, saved);
                push(vm, list_pool_exhausted(app));
                return;
            }
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    restore_var(app, instr->text, saved);
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
    SavedVar saved1 = save_var(app, var1);
    SavedVar saved2 = save_var(app, var2);
    EvalValue acc = coerce_template_element(node_to_value(&app->list_pool[iter_head]));
    int frame_floor = vm->frame_count;
    for (int idx = app->list_pool[iter_head].next; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(app, var1, acc);
        bind_template_var(app, var2, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        acc = pop(vm);
    }
    restore_var(app, var1, saved1);
    restore_var(app, var2, saved2);
    push(vm, acc);
}

static void exec_foreach_compiled(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, const Instr *instr) {
    EvalValue list_arg = pop(vm);
    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    SavedVar saved = save_var(app, instr->text);
    int frame_floor = vm->frame_count;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        bind_template_var(app, instr->text, coerce_template_element(node_to_value(&app->list_pool[idx])));
        vm_run(vm, app, pool, chunk, instr->a);
        if (vm->frame_count < frame_floor) break;
        if (app->stop_requested || app->throw_requested) break; // matches do_foreach's own check
    }
    restore_var(app, instr->text, saved);
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
                push(vm, word_val(instr->text));
                pc++;
                break;
            case OP_PUSH_VAR: {
                // Mirrors eval_expr's own AST_VARREF case exactly,
                // including "unset :name reads as 0".
                Variable *v = find_var(app, instr->text);
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
                if (v.type == VALUE_WORD) set_var_word(app, instr->text, v.word);
                else if (v.type == VALUE_LIST) set_var_list(app, instr->text, v.list_head);
                else if (v.type == VALUE_ARRAY) set_var_array(app, instr->text, v.list_head, (int)v.number);
                else set_var(app, instr->text, v.number);
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
                EvalValue result = call_builtin(app, pool, instr->text, args, &produced);
                vm->last_call_produced_output = produced;
                vm->last_call_resolved = 1; // an ordinary builtin call is always "resolved" in ast_eval's own sense
                push(vm, result);
                pc++;
                break;
            }
            case OP_CALL_PROC:
                exec_call_proc(vm, app, pool, instr, &pc);
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
                exec_return(vm, app, pop(vm), /*produced=*/1, &pc);
                break;
            case OP_STOP:
                exec_return(vm, app, num_val(0), /*produced=*/0, &pc);
                break;
            case OP_HALT:
                vm->vm_run_depth--;
                return VM_RUN_HALTED;
            case OP_PUSH_LIST_LITERAL:
                // See bytecode.h's own comment: the literal's contents
                // stay in the AST, built fresh each visit exactly like
                // eval_expr's own AST_LIST_LITERAL case does.
                push(vm, eval_build_list_literal(app, pool, instr->a));
                pc++;
                break;
            case OP_LOCAL:
                eval_local_declare(app, instr->text);
                pc++;
                break;
            case OP_ERASE:
                eval_erase_declare(app, pool, instr->text);
                pc++;
                break;
            case OP_SEND:
                exec_send(vm, app, pool, chunk, &pc);
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
                    append_output(app, "WAIT: not supported inside a MAP/FILTER/REDUCE/FOREACH template\n");
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
                    append_output(app, "WAITKEY: not supported inside a MAP/FILTER/REDUCE/FOREACH template\n");
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
                    append_output(app, "INPUT: not supported inside a MAP/FILTER/REDUCE/FOREACH template\n");
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
                    append_output(app, "PAUSE: not supported inside a MAP/FILTER/REDUCE/FOREACH template\n");
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
                    append_output(app, "ANIMATESPRITE: not supported inside a MAP/FILTER/REDUCE/FOREACH template\n");
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
