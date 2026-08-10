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
#include <string.h>
#include <strings.h>
#include <stdio.h>

static void push(Vm *vm, EvalValue v) {
    if (vm->stack_top >= MAX_VM_STACK) return; // can't happen for this vertical slice's own test scripts; see this file's own note on MAX_INSTRUCTIONS overflow handling in compiler.c for the same "not yet robustly reported" tradeoff
    vm->stack[vm->stack_top++] = v;
}

static EvalValue pop(Vm *vm) {
    if (vm->stack_top <= 0) return num_val(0);
    return vm->stack[--vm->stack_top];
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
static EvalValue call_builtin(LogoApp *app, AstPool *pool, const char *name, EvalValue *args, int *produced) {
    *produced = 1;
    if (strcasecmp(name, "PRINT") == 0) {
        eval_print_value(app, args[0]);
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

void vm_run(Vm *vm, LogoApp *app, AstPool *pool, BytecodeChunk *chunk, int start_pc) {
    int pc = start_pc;
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
                return;
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
                vm->last_call_resolved = 1; // MAKE/LOCAL/ERASE/WHILE are always "resolved" -- never the unknown-procedure case
                push(vm, num_val(0));
                pc++;
                break;
            default:
                pc++;
                break;
        }
    }
}
