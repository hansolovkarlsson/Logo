// eval.c
//
// See eval.h for scope/rationale. A note on what's deliberately NOT
// here yet, matching parser.c's own "bounded slice, not full coverage"
// framing: list/array values are never produced or consumed (a
// variable holding one, or a list literal used as a value, reports a
// clear message rather than silently misbehaving -- see eval_expr's
// AST_VARREF/AST_LIST_LITERAL cases) since that needs list_pool
// allocation, currently private to interpreter.c and not yet exposed;
// Ctrl+C interrupt checking is also absent, since g_interrupt_requested
// is a file-static flag in interpreter.c with no exposed getter.
// Both are documented follow-up work in docs/BYTECODE_VM_DESIGN.md,
// not silent gaps.
//
// One deliberate, documented simplification worth knowing up front:
// unlike the old interpreter (where FD/PRINT/MAKE/etc. are reachable
// only as statements, never as expression-position operators -- a
// separate keyword list from parse_factor's own operator chain), this
// evaluator's AST_CALL node serves both roles uniformly, so e.g.
// `PRINT FD 10` parses and runs here (FD's own side effect, then 0)
// where the old engine would behave quite differently. Narrow, and
// nobody writes real scripts this way, but real.

#include "eval.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// A value produced by evaluating an expression subtree. Mirrors
// interpreter.c's own private Value type (same fields ValueType/
// Variable already expose the shape of in logo_types.h) -- given its
// own name here since that struct itself is private to interpreter.c,
// and there's no need to expose it just for this.
typedef struct {
    ValueType type;
    double number;
    char word[512];
    int list_head; // always -1 here -- list values aren't produced yet
} EvalValue;

static EvalValue num_val(double n) {
    EvalValue v = {0};
    v.type = VALUE_NUMBER;
    v.number = n;
    v.list_head = -1;
    return v;
}

static EvalValue word_val(const char *w) {
    EvalValue v = {0};
    v.type = VALUE_WORD;
    snprintf(v.word, sizeof(v.word), "%s", w);
    v.list_head = -1;
    return v;
}

// Coerce to a number the same way interpreter.c's value_to_number
// does: a word reads the number its text starts with (0 if it
// doesn't -- strtod itself already returns 0 in that case, so there's
// no need for the "did it actually parse anything" check
// value_to_number makes, just to arrive at the same answer).
static double eval_to_number(EvalValue v) {
    if (v.type == VALUE_NUMBER) return v.number;
    return strtod(v.word, NULL);
}

// A value's truthiness -- mirrors interpreter.c's is_truthy.
static int eval_is_truthy(EvalValue v) {
    if (v.type == VALUE_WORD) return v.word[0] != '\0' && strcasecmp(v.word, "FALSE") != 0;
    return v.number != 0;
}

// Equality for = / <> when at least one side isn't a number --
// mirrors interpreter.c's values_equal (text comparison,
// case-insensitive).
static int eval_values_equal(EvalValue a, EvalValue b) {
    if (a.type == VALUE_NUMBER && b.type == VALUE_NUMBER) return a.number == b.number;
    char a_text[512], b_text[512];
    if (a.type == VALUE_WORD) snprintf(a_text, sizeof(a_text), "%s", a.word);
    else snprintf(a_text, sizeof(a_text), "%g", a.number);
    if (b.type == VALUE_WORD) snprintf(b_text, sizeof(b_text), "%s", b.word);
    else snprintf(b_text, sizeof(b_text), "%g", b.number);
    return strcasecmp(a_text, b_text) == 0;
}

static EvalValue eval_expr(LogoApp *app, AstPool *pool, int node_idx);
static int eval_condition(LogoApp *app, AstPool *pool, int node_idx);
static void exec_block(LogoApp *app, AstPool *pool, int block_node);
static void exec_call(LogoApp *app, AstPool *pool, int call_node, int *resolved, int *produced, EvalValue *result);

// Finds an AST_PROC_DEF by name anywhere in the pool -- not just
// program_node's own top-level children. Matches parser.c's own
// hoist_procedures, which scans the flat token stream for "TO name"
// with no bracket-depth tracking at all, so a TO could in principle
// appear nested inside a block; searching the whole pool (every
// procedure lives in the same flat array regardless of nesting, same
// "small program, linear scan is fine" precedent as interpreter.c's
// own Procedure/Variable/PlistEntry tables) matches that same reach
// rather than only searching the top level.
static int find_proc_def(AstPool *pool, const char *name) {
    for (int i = 0; i < pool->node_count; i++) {
        if (pool->nodes[i].type == AST_PROC_DEF && strcasecmp(pool->nodes[i].text, name) == 0) {
            return i;
        }
    }
    return -1;
}

// Calls the AST_PROC_DEF at def_node with arg_vals bound as its
// parameters. Reimplements interpreter.c's own call_procedure logic
// (push a scope, bind params, run the body, catch OUTPUT/STOP, pop the
// scope) rather than calling that function directly -- it's hardwired
// to a text-based Procedure.body run through eval_logo, while an
// AST_PROC_DEF's body is already a parsed AST_BLOCK run through
// exec_block instead. Shares the exact same scope-stack fields
// (app->scopes/app->scope_depth) and OUTPUT/STOP fields find_var and
// interpreter.c's own call_procedure both already read/write, so the
// two engines can't drift on scoping semantics even though procedure
// *storage* differs.
static EvalValue call_ast_procedure(LogoApp *app, AstPool *pool, int def_node, EvalValue *arg_vals, int arg_count, int *produced) {
    if (app->scope_depth >= MAX_SCOPE_DEPTH) {
        append_output(app, "Recursion too deep, call ignored\n");
        *produced = 0;
        return num_val(0);
    }
    AstNode *def = &pool->nodes[def_node];
    Scope *scope = &app->scopes[app->scope_depth];
    scope->count = def->param_count;
    snprintf(scope->proc_name, sizeof(scope->proc_name), "%s", def->text);
    for (int i = 0; i < def->param_count; i++) {
        snprintf(scope->vars[i].name, sizeof(scope->vars[i].name), "%s", def->param_names[i]);
        Variable *slot = &scope->vars[i];
        if (i < arg_count) {
            slot->type = arg_vals[i].type;
            if (arg_vals[i].type == VALUE_WORD) {
                snprintf(slot->word, sizeof(slot->word), "%s", arg_vals[i].word);
            } else {
                slot->number = arg_vals[i].number;
            }
        } else {
            slot->type = VALUE_NUMBER;
            slot->number = 0;
        }
    }
    app->scope_depth++;

    int body = def->first_child;
    if (body >= 0) exec_block(app, pool, body);

    int did_output = app->has_output_value;
    EvalValue result = num_val(0);
    if (did_output) {
        result.type = app->output_type;
        result.number = app->output_number;
        snprintf(result.word, sizeof(result.word), "%s", app->output_word);
        result.list_head = app->output_list_head;
    }
    app->stop_requested = FALSE;
    app->has_output_value = FALSE;
    app->scope_depth--;
    *produced = did_output;
    return result;
}

// One AST_CALL node -- a built-in or a hoisted user procedure, in
// either statement position (resolved/produced/result may be NULL,
// meaning "discard whatever this produces") or expression position
// (all three captured). See parser.c's BUILTIN_SIGNATURES for which
// built-ins are covered; growing this alongside that table is
// incremental follow-up work.
static void exec_call(LogoApp *app, AstPool *pool, int call_node, int *resolved, int *produced, EvalValue *result) {
    AstNode *node = &pool->nodes[call_node];
    const char *name = node->text;

    int arg_idx[AST_MAX_PARAMS];
    int argc = 0;
    for (int c = node->first_child; c >= 0; c = pool->nodes[c].next_sibling) {
        if (argc < AST_MAX_PARAMS) arg_idx[argc] = c;
        argc++;
    }

    if (resolved != NULL) *resolved = 1;
    if (produced != NULL) *produced = 0;
    if (result != NULL) *result = num_val(0);

#define ARGNUM(i) eval_to_number(eval_expr(app, pool, arg_idx[i]))
#define ARGVAL(i) eval_expr(app, pool, arg_idx[i])

    if (strcasecmp(name, "FD") == 0 || strcasecmp(name, "FORWARD") == 0) {
        move_turtle_forward(app, ARGNUM(0));
    } else if (strcasecmp(name, "BK") == 0 || strcasecmp(name, "BACK") == 0) {
        move_turtle_forward(app, -ARGNUM(0));
    } else if (strcasecmp(name, "RT") == 0 || strcasecmp(name, "RIGHT") == 0) {
        current_turtle(app)->angle += ARGNUM(0);
    } else if (strcasecmp(name, "LT") == 0 || strcasecmp(name, "LEFT") == 0) {
        current_turtle(app)->angle -= ARGNUM(0);
    } else if (strcasecmp(name, "SETXY") == 0) {
        double x = ARGNUM(0);
        double y = ARGNUM(1);
        move_turtle_to(app, x, y);
    } else if (strcasecmp(name, "SETHEADING") == 0 || strcasecmp(name, "SETH") == 0) {
        current_turtle(app)->angle = ARGNUM(0);
    } else if (strcasecmp(name, "PENUP") == 0 || strcasecmp(name, "PU") == 0) {
        current_turtle(app)->pen_down = FALSE;
    } else if (strcasecmp(name, "PENDOWN") == 0 || strcasecmp(name, "PD") == 0) {
        current_turtle(app)->pen_down = TRUE;
    } else if (strcasecmp(name, "HOME") == 0) {
        move_turtle_to(app, home_x(app), home_y(app));
        current_turtle(app)->angle = 0;
    } else if (strcasecmp(name, "CLEAR") == 0 || strcasecmp(name, "CS") == 0) {
        app->line_count = 0;
        app->label_count = 0;
        app->raster_op_count = 0;
        for (int i = 0; i < app->turtle_count; i++) {
            app->turtles[i].x = home_x(app);
            app->turtles[i].y = home_y(app);
            app->turtles[i].angle = 0;
        }
    } else if (strcasecmp(name, "PRINT") == 0) {
        EvalValue v = ARGVAL(0);
        char buf[560];
        if (v.type == VALUE_WORD) snprintf(buf, sizeof(buf), "%s\n", v.word);
        else snprintf(buf, sizeof(buf), "%g\n", v.number);
        append_output(app, buf);
    } else if (strcasecmp(name, "MAKE") == 0) {
        // arg_idx[0] is an AST_WORD -- the parser's ARG_QUOTED_WORD
        // kind already guarantees this holds the variable's name
        // directly in .text (see parser.c's MAKE signature).
        const char *varname = pool->nodes[arg_idx[0]].text;
        EvalValue val = ARGVAL(1);
        if (val.type == VALUE_WORD) set_var_word(app, varname, val.word);
        else set_var(app, varname, val.number);
    } else if (strcasecmp(name, "OUTPUT") == 0) {
        EvalValue val = ARGVAL(0);
        app->output_type = val.type;
        app->output_number = val.number;
        snprintf(app->output_word, sizeof(app->output_word), "%s", val.word);
        app->output_list_head = val.list_head;
        app->has_output_value = TRUE;
        app->stop_requested = TRUE;
    } else if (strcasecmp(name, "STOP") == 0) {
        app->stop_requested = TRUE;
    } else if (strcasecmp(name, "REPEAT") == 0) {
        int count = (int)ARGNUM(0);
        int block_node = arg_idx[1];
        for (int i = 0; i < count && !app->stop_requested; i++) {
            exec_block(app, pool, block_node);
        }
    } else if (strcasecmp(name, "WHILE") == 0) {
        int cond_node = arg_idx[0];
        int block_node = arg_idx[1];
        int iterations = 0;
        while (eval_condition(app, pool, cond_node)) {
            if (iterations >= MAX_WHILE_ITERATIONS) {
                append_output(app, "WHILE: stopped after too many iterations\n");
                break;
            }
            exec_block(app, pool, block_node);
            if (app->stop_requested) break;
            iterations++;
        }
    } else if (strcasecmp(name, "ABS") == 0) {
        if (result != NULL) *result = num_val(fabs(ARGNUM(0)));
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SQRT") == 0) {
        if (result != NULL) *result = num_val(sqrt(ARGNUM(0)));
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "POWER") == 0) {
        double base = ARGNUM(0);
        double exponent = ARGNUM(1);
        if (result != NULL) *result = num_val(pow(base, exponent));
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "RANDOM") == 0) {
        if (result != NULL) *result = num_val(random_below(ARGNUM(0)));
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ROUND") == 0) {
        if (result != NULL) *result = num_val(round(ARGNUM(0)));
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "INT") == 0) {
        if (result != NULL) *result = num_val(trunc(ARGNUM(0)));
        if (produced != NULL) *produced = 1;
    } else {
        // Not a built-in -- must be a hoisted user procedure: the
        // parser already guarantees this by construction (an AST_CALL
        // node only exists for a name that resolved to a builtin or a
        // hoisted procedure at parse time -- see parser.c's
        // try_parse_call). find_proc_def failing here would mean the
        // parser's hoisting and this evaluator's own tree-search fell
        // out of sync, not a normal user error.
        int def_node = find_proc_def(pool, name);
        if (def_node < 0) {
            if (resolved != NULL) *resolved = 0;
            append_output(app, "I don't know how to ");
            append_output(app, name);
            append_output(app, "\n");
            return;
        }
        EvalValue arg_vals[AST_MAX_PARAMS];
        int n = argc < AST_MAX_PARAMS ? argc : AST_MAX_PARAMS;
        for (int i = 0; i < n; i++) arg_vals[i] = ARGVAL(i);
        int did_output;
        EvalValue r = call_ast_procedure(app, pool, def_node, arg_vals, n, &did_output);
        if (produced != NULL) *produced = did_output;
        if (result != NULL) *result = r;
    }

#undef ARGNUM
#undef ARGVAL
}

static EvalValue eval_expr(LogoApp *app, AstPool *pool, int node_idx) {
    AstNode *node = &pool->nodes[node_idx];
    switch (node->type) {
        case AST_NUMBER:
            return num_val(node->number);
        case AST_WORD:
            return word_val(node->text);
        case AST_VARREF: {
            Variable *v = find_var(app, node->text);
            if (v == NULL) return num_val(0); // unset :name reads as 0, matching interpreter.c
            if (v->type == VALUE_WORD) return word_val(v->word);
            if (v->type == VALUE_NUMBER) return num_val(v->number);
            append_output(app, "AST evaluator: list/array variables aren't supported yet\n");
            return num_val(0);
        }
        case AST_LIST_LITERAL:
            append_output(app, "AST evaluator: list values aren't supported yet\n");
            return word_val("");
        case AST_NEG:
            return num_val(-eval_to_number(eval_expr(app, pool, node->first_child)));
        case AST_BINOP: {
            int left_node = node->first_child;
            int right_node = pool->nodes[left_node].next_sibling;
            double left = eval_to_number(eval_expr(app, pool, left_node));
            double right = eval_to_number(eval_expr(app, pool, right_node));
            switch (node->binop) {
                case AST_OP_ADD: return num_val(left + right);
                case AST_OP_SUB: return num_val(left - right);
                case AST_OP_MUL: return num_val(left * right);
                case AST_OP_DIV: return num_val(right != 0 ? left / right : 0);
            }
            return num_val(0);
        }
        case AST_CALL: {
            int resolved, produced;
            EvalValue result;
            exec_call(app, pool, node_idx, &resolved, &produced, &result);
            if (resolved && !produced) {
                append_output(app, node->text);
                append_output(app, ": didn't output a value\n");
                return word_val("");
            }
            return result;
        }
        default:
            // A condition-only node (AST_COMPARE/AST_NOT/AST_AND/
            // AST_OR) reached in value position, or anything else the
            // parser's own grammar shouldn't actually produce here.
            return num_val(eval_condition(app, pool, node_idx));
    }
}

static int eval_condition(LogoApp *app, AstPool *pool, int node_idx) {
    AstNode *node = &pool->nodes[node_idx];
    switch (node->type) {
        case AST_NOT:
            return !eval_condition(app, pool, node->first_child);
        case AST_AND: {
            // No short-circuiting -- both sides are always evaluated,
            // matching parse_bool_and's own unconditional rhs parse.
            int left = eval_condition(app, pool, node->first_child);
            int right = eval_condition(app, pool, pool->nodes[node->first_child].next_sibling);
            return left && right;
        }
        case AST_OR: {
            int left = eval_condition(app, pool, node->first_child);
            int right = eval_condition(app, pool, pool->nodes[node->first_child].next_sibling);
            return left || right;
        }
        case AST_COMPARE: {
            int left_node = node->first_child;
            int right_node = pool->nodes[left_node].next_sibling;
            EvalValue left = eval_expr(app, pool, left_node);
            EvalValue right = eval_expr(app, pool, right_node);
            if (left.type != VALUE_NUMBER || right.type != VALUE_NUMBER) {
                int equal = eval_values_equal(left, right);
                if (node->cmpop == AST_CMP_EQ) return equal;
                if (node->cmpop == AST_CMP_NE) return !equal;
                return 0;
            }
            switch (node->cmpop) {
                case AST_CMP_LT: return left.number < right.number;
                case AST_CMP_GT: return left.number > right.number;
                case AST_CMP_EQ: return left.number == right.number;
                case AST_CMP_LE: return left.number <= right.number;
                case AST_CMP_GE: return left.number >= right.number;
                case AST_CMP_NE: return left.number != right.number;
            }
            return 0;
        }
        default:
            return eval_is_truthy(eval_expr(app, pool, node_idx));
    }
}

static void exec_if(LogoApp *app, AstPool *pool, int if_node) {
    AstNode *node = &pool->nodes[if_node];
    int cond_node = node->first_child;
    int true_block = pool->nodes[cond_node].next_sibling;
    int false_block = pool->nodes[true_block].next_sibling; // -1 if there's no else

    if (eval_condition(app, pool, cond_node)) {
        exec_block(app, pool, true_block);
    } else if (false_block >= 0) {
        exec_block(app, pool, false_block);
    }
}

static void exec_statement(LogoApp *app, AstPool *pool, int node_idx) {
    AstNode *node = &pool->nodes[node_idx];
    if (node->type == AST_PROC_DEF) return; // already resolvable via find_proc_def; nothing to run
    if (node->type == AST_IF) {
        exec_if(app, pool, node_idx);
        return;
    }
    exec_call(app, pool, node_idx, NULL, NULL, NULL); // a plain command: discard whatever it OUTPUTs
}

static void exec_block(LogoApp *app, AstPool *pool, int block_node) {
    for (int c = pool->nodes[block_node].first_child; c >= 0; c = pool->nodes[c].next_sibling) {
        exec_statement(app, pool, c);
        // No g_interrupt_requested check here (see the file comment
        // at the top) -- only OUTPUT/STOP unwinds a block early today.
        if (app->stop_requested) break;
    }
}

void ast_eval(LogoApp *app, AstPool *pool, int program_node) {
    exec_block(app, pool, program_node);
    // A STOP with no enclosing procedure call, mirroring eval_logo's
    // own top-level recovery -- otherwise it would stay set forever
    // and silently no-op every later top-level statement.
    if (app->stop_requested) app->stop_requested = FALSE;
    app->has_output_value = FALSE;
}
