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
static EvalValue call_builtin(LogoApp *app, const char *name, EvalValue *args, int *produced) {
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
// procedure's own compiled body. On MAX_SCOPE_DEPTH (recursion too
// deep, already reported to output by eval_push_scope_for_call), skips
// the call entirely and pushes num_val(0) in its place -- matching
// call_ast_procedure's own *produced=0/num_val(0) behavior on the same
// condition.
static void exec_call_proc(Vm *vm, LogoApp *app, AstPool *pool, const Instr *instr, int *pc) {
    int argc = instr->b;
    EvalValue args[AST_MAX_PARAMS];
    for (int i = argc - 1; i >= 0; i--) {
        args[i] = (i < AST_MAX_PARAMS) ? pop(vm) : (pop(vm), num_val(0));
    }
    int def_node = find_proc_def(pool, instr->text);
    if (def_node < 0 || vm->frame_count >= MAX_VM_FRAMES) {
        // *pc must still advance past this OP_CALL_PROC on failure --
        // leaving it unchanged would re-execute the very same call
        // forever (fatal for exactly the recursion-too-deep case this
        // guards against, where the caller keeps retrying the same
        // now-permanently-failing call).
        vm->last_call_produced_output = 0;
        push(vm, num_val(0));
        *pc = *pc + 1;
        return;
    }
    AstNode *def = &pool->nodes[def_node];
    if (!eval_push_scope_for_call(app, def, args, argc < AST_MAX_PARAMS ? argc : AST_MAX_PARAMS)) {
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
                EvalValue result = call_builtin(app, instr->text, args, &produced);
                vm->last_call_produced_output = produced;
                push(vm, result);
                pc++;
                break;
            }
            case OP_CALL_PROC:
                exec_call_proc(vm, app, pool, instr, &pc);
                break;
            case OP_CHECK_OUTPUT: {
                // Only ever emitted right after an OP_CALL_PROC used in
                // expression position. That call already left exactly
                // one value on the stack; if its own body never called
                // OUTPUT (vm->last_call_produced_output, set by the
                // OP_STOP/OP_OUTPUT that just returned, or by
                // exec_call_proc's own early-failure paths), replace it
                // with word_val("") and report the same diagnostic
                // eval_expr's own AST_CALL case does.
                if (!vm->last_call_produced_output) {
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
            case OP_VOID_RESULT:
                vm->last_call_produced_output = 0;
                push(vm, num_val(0));
                pc++;
                break;
            default:
                pc++;
                break;
        }
    }
}
