// eval.c
//
// See eval.h for scope/rationale. A note on what's deliberately NOT
// here yet, matching parser.c's own "bounded slice, not full coverage"
// framing: Ctrl+C interrupt checking is absent, since
// g_interrupt_requested is a file-static flag in interpreter.c with no
// exposed getter -- documented follow-up work in
// docs/BYTECODE_VM_DESIGN.md, not a silent gap. List and array values
// ARE fully supported (list_alloc_node/list_node_copy/set_var_list/
// set_var_array, exposed in interpreter.h), sharing the exact same
// app->list_pool eval_logo's own list/array operators build into, so a
// value built by one engine is indistinguishable from one built by the
// other.
//
// MAP/FILTER/REDUCE's templates are the one place this file re-enters
// the front end at runtime rather than just walking an already-built
// AST: interpreter.c's own MAP/FILTER/REDUCE substitute an element's
// text into a template string and re-parse it, only possible there
// because parsing and execution are the same pass. This evaluator has
// no such single-pass parser to hook into mid-execution, so it goes
// through the same lex -> parse -> eval pipeline any script does, just
// on a short synthesized snippet instead (see eval_apply_template_expr/
// eval_apply_template_condition, and logo_parse_expr/
// logo_parse_condition in parser.c).
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
#include "lexer.h"
#include "parser.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
    int list_head; // type == VALUE_LIST: index into app->list_pool, -1 = empty -- the SAME pool eval_logo's own list operators use
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

static EvalValue list_val(int head) {
    EvalValue v = {0};
    v.type = VALUE_LIST;
    v.list_head = head;
    return v;
}

static EvalValue array_val(int start, int length) {
    EvalValue v = {0};
    v.type = VALUE_ARRAY;
    v.list_head = start;
    v.number = length; // an array's `number` field holds its length, not a value -- matches interpreter.c's own array_value
    return v;
}

// A "list storage full" report shared by every list-construction
// operator below -- same wording and "loud, not silent" policy as
// interpreter.c's own list_pool_exhausted_error.
static EvalValue list_pool_exhausted(LogoApp *app) {
    append_output(app, "list storage full, list operation ignored\n");
    return word_val("");
}

// Map a list element node to an EvalValue -- mirrors interpreter.c's
// own list_node_to_value.
static EvalValue node_to_value(const ListNode *node) {
    if (node->type == LIST_ELEM_NUMBER) return num_val(node->number);
    if (node->type == LIST_ELEM_LIST) return list_val(node->sublist_head);
    return word_val(node->word);
}

// Store `v` as a single new list-element node (a list contributes its
// existing chain by index, no deep copy needed) -- mirrors
// interpreter.c's own list_node_from_value, built on the now-exposed
// list_alloc_node. Returns -1 (pool exhausted) same as that function.
static int value_to_node(LogoApp *app, EvalValue v) {
    int idx = list_alloc_node(app);
    if (idx < 0) return -1;
    ListNode *node = &app->list_pool[idx];
    node->next = -1;
    if (v.type == VALUE_NUMBER) {
        node->type = LIST_ELEM_NUMBER;
        node->number = v.number;
    } else if (v.type == VALUE_LIST) {
        node->type = LIST_ELEM_LIST;
        node->sublist_head = v.list_head;
    } else {
        node->type = LIST_ELEM_WORD;
        snprintf(node->word, sizeof(node->word), "%s", v.word);
    }
    return idx;
}

static void list_elements_to_text(LogoApp *app, int head, char *out, size_t out_size);

// Append one list/array cell's rendering to `out` -- mirrors
// interpreter.c's own append_node_text (a nested LIST_ELEM_LIST
// recurses one level deeper, bracketed).
static void append_node_text(LogoApp *app, const ListNode *node, char *out, size_t out_size) {
    if (node->type == LIST_ELEM_NUMBER) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", node->number);
        strncat(out, buf, out_size - strlen(out) - 1);
    } else if (node->type == LIST_ELEM_LIST) {
        strncat(out, "[", out_size - strlen(out) - 1);
        char sub[512];
        list_elements_to_text(app, node->sublist_head, sub, sizeof(sub));
        strncat(out, sub, out_size - strlen(out) - 1);
        strncat(out, "]", out_size - strlen(out) - 1);
    } else {
        strncat(out, node->word, out_size - strlen(out) - 1);
    }
}

// Render a list's elements, space-separated, without enclosing
// brackets at this level -- mirrors interpreter.c's own
// list_elements_to_text exactly (PRINT [a b] -> "a b").
static void list_elements_to_text(LogoApp *app, int head, char *out, size_t out_size) {
    out[0] = '\0';
    int first = 1;
    for (int idx = head; idx != -1; idx = app->list_pool[idx].next) {
        if (!first) strncat(out, " ", out_size - strlen(out) - 1);
        first = 0;
        append_node_text(app, &app->list_pool[idx], out, out_size);
    }
}

// Render an array's `length` cells (starting at `start`), space-
// separated and enclosed in { } -- mirrors interpreter.c's own
// array_elements_to_text exactly. Unlike a list, an array's own braces
// show even as PRINT's top-level value, keeping the two concrete types
// visually distinguishable.
static void array_elements_to_text(LogoApp *app, int start, int length, char *out, size_t out_size) {
    out[0] = '\0';
    strncat(out, "{", out_size - strlen(out) - 1);
    for (int i = 0; i < length; i++) {
        if (i > 0) strncat(out, " ", out_size - strlen(out) - 1);
        append_node_text(app, &app->list_pool[start + i], out, out_size);
    }
    strncat(out, "}", out_size - strlen(out) - 1);
}

// Render any value as text -- mirrors interpreter.c's own
// value_to_text (a word as-is, a number %g-formatted, a list via
// list_elements_to_text, an array via array_elements_to_text).
static void eval_value_to_text(LogoApp *app, EvalValue v, char *out, size_t out_size) {
    if (v.type == VALUE_WORD) snprintf(out, out_size, "%s", v.word);
    else if (v.type == VALUE_LIST) list_elements_to_text(app, v.list_head, out, out_size);
    else if (v.type == VALUE_ARRAY) array_elements_to_text(app, v.list_head, (int)v.number, out, out_size);
    else snprintf(out, out_size, "%g", v.number);
}

// Coerce to a number the same way interpreter.c's value_to_number
// does: a word reads the number its text starts with (0 if it
// doesn't -- strtod itself already returns 0 in that case, so there's
// no need for the "did it actually parse anything" check
// value_to_number makes, just to arrive at the same answer); a list
// has no meaningful numeric reading either way.
static double eval_to_number(EvalValue v) {
    if (v.type == VALUE_NUMBER) return v.number;
    if (v.type == VALUE_LIST) return 0;
    return strtod(v.word, NULL);
}

// A value's truthiness -- mirrors interpreter.c's is_truthy.
static int eval_is_truthy(EvalValue v) {
    if (v.type == VALUE_LIST) return v.list_head != -1;
    if (v.type == VALUE_WORD) return v.word[0] != '\0' && strcasecmp(v.word, "FALSE") != 0;
    return v.number != 0;
}

// Equality for = / <> when at least one side isn't a number --
// mirrors interpreter.c's values_equal (text comparison,
// case-insensitive; a list renders through eval_value_to_text same as
// anything else).
static int eval_values_equal(LogoApp *app, EvalValue a, EvalValue b) {
    if (a.type == VALUE_NUMBER && b.type == VALUE_NUMBER) return a.number == b.number;
    char a_text[512], b_text[512];
    eval_value_to_text(app, a, a_text, sizeof(a_text));
    eval_value_to_text(app, b, b_text, sizeof(b_text));
    return strcasecmp(a_text, b_text) == 0;
}

static EvalValue eval_expr(LogoApp *app, AstPool *pool, int node_idx);

// --- MAP/FILTER/REDUCE's template substitution ------------------------
//
// Each template (e.g. `[? * 2]`, `[?1 + ?2]`) is a list literal, so
// eval_expr already hands back an ordinary VALUE_LIST whose elements
// are the template's raw, untyped words -- exactly interpreter.c's own
// template_val. Rendering that list back to flat text (via the
// already-existing list_elements_to_text), substituting the current
// element's text in for "?"/"?1"/"?2", then lexing+parsing+evaluating
// the result is this evaluator's equivalent of interpreter.c's
// apply_template_expr/apply_template_condition -- see the file comment
// at the top of this file for why it has to go through the front end
// again rather than something more direct.

// Whether `word`'s entire text parses as a number (not just a leading
// prefix) -- mirrors interpreter.c's own word_is_entirely_a_number,
// used only to decide whether eval_value_to_source_text below is safe
// to substitute a WORD bare vs. needing 'raw text' quoting.
static int eval_word_is_entirely_a_number(const char *word) {
    if (word[0] == '\0') return 0;
    char *end;
    strtod(word, &end);
    return *end == '\0';
}

// Like eval_value_to_text, but always brackets a list and quotes a
// non-numeric word with 'raw text' syntax -- for when the rendered
// text is about to be *re-parsed* as Logo source (this file's template
// substitution) rather than shown to a person. Mirrors interpreter.c's
// own value_to_source_text exactly, including its reasoning: a
// substituted list needs to look like literal [...] syntax so
// parse_factor/parse_bareword_value's own `[` branch reconstructs it
// as one value again, and a substituted non-numeric word needs 'raw
// text' quoting so it lexes back as a single WORD token rather than
// spilling into the surrounding template as its own bareword.
static void eval_value_to_source_text(LogoApp *app, EvalValue v, char *out, size_t out_size) {
    if (v.type == VALUE_LIST) {
        char inner[512];
        list_elements_to_text(app, v.list_head, inner, sizeof(inner));
        snprintf(out, out_size, "[%s]", inner);
    } else if (v.type == VALUE_WORD && !eval_word_is_entirely_a_number(v.word)) {
        snprintf(out, out_size, "'%s'", v.word);
    } else {
        eval_value_to_text(app, v, out, out_size);
    }
}

// Substitute every whitespace-delimited occurrence of `placeholder` in
// `template_text` with `replacement` -- a token-for-token swap, not a
// substring replacement (so "?2" is left alone when substituting "?").
// Mirrors interpreter.c's own substitute_placeholder exactly.
static void eval_substitute_placeholder(const char *template_text, const char *placeholder, const char *replacement, char *out, size_t out_size) {
    out[0] = '\0';
    const char *p = template_text;
    int first = 1;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char token[512] = {0};
        int n = 0;
        if (sscanf(p, "%511s%n", token, &n) != 1 || n == 0) break;
        if (!first) strncat(out, " ", out_size - strlen(out) - 1);
        first = 0;
        strncat(out, strcmp(token, placeholder) == 0 ? replacement : token, out_size - strlen(out) - 1);
        p += n;
    }
}

#define MAX_TEMPLATE_TOKENS 128

// Substitute `element` for every "?" in `template_text`, then lex,
// parse, and evaluate the result as an expression -- MAP/REDUCE's own
// operator form. `scratch` is a caller-owned, caller-reused ParseResult
// (its AstPool is ~6.7MB, too large to calloc fresh per list element --
// same heap-only rule as everywhere else this project uses AstPool/
// LogoApp). A malformed template (lex or parse failure) reads as 0,
// same "quietly inert rather than crashing" fallback interpreter.c's
// own parse_expr effectively gets by returning word_value("") on its
// own internal errors.
static EvalValue eval_apply_template_expr(LogoApp *app, const char *template_text, EvalValue element, ParseResult *scratch) {
    char el_text[512], expr_text[512];
    eval_value_to_source_text(app, element, el_text, sizeof(el_text));
    eval_substitute_placeholder(template_text, "?", el_text, expr_text, sizeof(expr_text));
    LogoToken tokens[MAX_TEMPLATE_TOKENS];
    int n = logo_lex(expr_text, tokens, MAX_TEMPLATE_TOKENS);
    if (n < 0) return num_val(0);
    int node = logo_parse_expr(tokens, n, scratch);
    if (node < 0) return num_val(0);
    return eval_expr(app, &scratch->pool, node);
}

// Same substitution as eval_apply_template_expr, but parses the result
// as a *condition* (comparisons, AND/OR/NOT) instead of a plain
// expression -- FILTER's template is a predicate (e.g. `[? > 2]`), and
// an expression-only parse doesn't understand `>` at all. Mirrors
// interpreter.c's own apply_template_condition.
static int eval_apply_template_condition(LogoApp *app, const char *template_text, EvalValue element, ParseResult *scratch) {
    char el_text[512], expr_text[512];
    eval_value_to_source_text(app, element, el_text, sizeof(el_text));
    eval_substitute_placeholder(template_text, "?", el_text, expr_text, sizeof(expr_text));
    LogoToken tokens[MAX_TEMPLATE_TOKENS];
    int n = logo_lex(expr_text, tokens, MAX_TEMPLATE_TOKENS);
    if (n < 0) return 0;
    int node = logo_parse_condition(tokens, n, scratch);
    if (node < 0) return 0;
    return eval_is_truthy(eval_expr(app, &scratch->pool, node));
}

// --- List-construction operators (FPUT/LPUT/SENTENCE/LIST/BUTLAST) --
//
// Each mirrors its interpreter.c namesake (list_fput/list_lput/
// list_sentence/list_wrap_pair/list_butlast) exactly, just built on
// EvalValue/value_to_node/list_node_copy instead of the private Value
// type -- these are real functions, not inlined into exec_call's
// dispatch below, for the same reason interpreter.c itself factors
// them out: each is a few lines of real list-splicing logic, not a
// one-liner.

static EvalValue eval_list_butlast(LogoApp *app, EvalValue arg) {
    int count = 0;
    for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
    if (count <= 1) return list_val(-1);
    int new_head = -1;
    int *next_slot = &new_head;
    int idx = arg.list_head;
    for (int i = 0; i < count - 1; i++) {
        int copy = list_node_copy(app, idx);
        if (copy < 0) return list_pool_exhausted(app);
        *next_slot = copy;
        next_slot = &app->list_pool[copy].next;
        idx = app->list_pool[idx].next;
    }
    return list_val(new_head);
}

static EvalValue eval_list_fput(LogoApp *app, EvalValue thing, EvalValue list) {
    if (list.type == VALUE_WORD) {
        if (thing.type == VALUE_LIST) {
            append_output(app, "FPUT: can't add a list to a word\n");
            return word_val("");
        }
        char thing_text[512];
        eval_value_to_text(app, thing, thing_text, sizeof(thing_text));
        EvalValue result = word_val("");
        snprintf(result.word, sizeof(result.word), "%s%s", thing_text, list.word);
        return result;
    }
    int list_head = (list.type == VALUE_LIST) ? list.list_head : value_to_node(app, list);
    if (list.type != VALUE_LIST && list_head < 0) return list_pool_exhausted(app);
    int new_node = value_to_node(app, thing);
    if (new_node < 0) return list_pool_exhausted(app);
    app->list_pool[new_node].next = list_head;
    return list_val(new_node);
}

static EvalValue eval_list_lput(LogoApp *app, EvalValue thing, EvalValue list) {
    if (list.type == VALUE_WORD) {
        if (thing.type == VALUE_LIST) {
            append_output(app, "LPUT: can't add a list to a word\n");
            return word_val("");
        }
        char thing_text[512];
        eval_value_to_text(app, thing, thing_text, sizeof(thing_text));
        EvalValue result = word_val("");
        snprintf(result.word, sizeof(result.word), "%s%s", list.word, thing_text);
        return result;
    }
    int src_head = (list.type == VALUE_LIST) ? list.list_head : value_to_node(app, list);
    if (list.type != VALUE_LIST && src_head < 0) return list_pool_exhausted(app);
    int new_head = -1;
    int *next_slot = &new_head;
    for (int idx = src_head; idx != -1; idx = app->list_pool[idx].next) {
        int copy = list_node_copy(app, idx);
        if (copy < 0) return list_pool_exhausted(app);
        *next_slot = copy;
        next_slot = &app->list_pool[copy].next;
    }
    int new_node = value_to_node(app, thing);
    if (new_node < 0) return list_pool_exhausted(app);
    *next_slot = new_node;
    return list_val(new_head);
}

static EvalValue eval_list_sentence(LogoApp *app, EvalValue a, EvalValue b) {
    int new_head = -1;
    int *next_slot = &new_head;
    EvalValue parts[2] = {a, b};
    for (int p = 0; p < 2; p++) {
        if (parts[p].type == VALUE_LIST) {
            for (int idx = parts[p].list_head; idx != -1; idx = app->list_pool[idx].next) {
                int copy = list_node_copy(app, idx);
                if (copy < 0) return list_pool_exhausted(app);
                *next_slot = copy;
                next_slot = &app->list_pool[copy].next;
            }
        } else {
            int node = value_to_node(app, parts[p]);
            if (node < 0) return list_pool_exhausted(app);
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    return list_val(new_head);
}

// WORD: concatenate two words directly (no separating space), erroring
// on a list argument -- mirrors interpreter.c's own list_word/WORD
// handling. Its own function (not inlined into exec_call's dispatch,
// like every list operator above) specifically to keep its two
// char[512] text buffers out of exec_call's own stack frame -- exec_call
// is one large function with a branch per built-in, recursing through
// call_ast_procedure for ordinary procedure calls, so extra per-branch
// locals there raise its per-call stack cost for every recursion level,
// not just calls to WORD (confirmed directly: adding this and the other
// list operators inline pushed the 200-level recursion test over the
// real stack under AddressSanitizer, where it was clean before).
static EvalValue eval_word_concat(LogoApp *app, EvalValue a, EvalValue b) {
    if (a.type == VALUE_LIST || b.type == VALUE_LIST) {
        append_output(app, "WORD: expected words, not a list\n");
        return word_val("");
    }
    char a_text[512], b_text[512];
    eval_value_to_text(app, a, a_text, sizeof(a_text));
    eval_value_to_text(app, b, b_text, sizeof(b_text));
    EvalValue result = word_val("");
    snprintf(result.word, sizeof(result.word), "%s%s", a_text, b_text);
    return result;
}

static EvalValue eval_list_wrap_pair(LogoApp *app, EvalValue a, EvalValue b) {
    int node_a = value_to_node(app, a);
    if (node_a < 0) return list_pool_exhausted(app);
    int node_b = value_to_node(app, b);
    if (node_b < 0) return list_pool_exhausted(app);
    app->list_pool[node_a].next = node_b;
    return list_val(node_a);
}

// Fills out[0..2] with a list's first two elements' numeric values and
// returns 1 only if the list has *exactly* two elements -- DISTANCE/
// TOWARDS's own [x y] point convention, the same one POS already
// returns. Mirrors interpreter.c's own list_as_two_numbers exactly.
static int eval_list_as_two_numbers(LogoApp *app, EvalValue v, double out[2]) {
    if (v.type != VALUE_LIST) return 0;
    int n = 0;
    for (int idx = v.list_head; idx != -1; idx = app->list_pool[idx].next, n++) {
        if (n < 2) out[n] = eval_to_number(node_to_value(&app->list_pool[idx]));
    }
    return n == 2;
}

// --- Property lists (SETPROP/GETPROP/REMOVEPROP/PROPLIST) -----------
//
// A separate namespace from ordinary variables, sharing
// app->plist_entries/plist_entry_count (see PlistEntry in
// logo_types.h) directly with eval_logo's own SETPROP/GETPROP/etc --
// a property set by one engine is visible to the other, same "shared
// state, not reimplemented logic" approach as everything else here.
// Each gets its own function (not inlined into exec_call's dispatch)
// for the same reason WORD/PROPLIST-shaped things already do: keeping
// exec_call's own per-call stack frame small, since it recurses
// through call_ast_procedure for every ordinary procedure call (see
// the WORD extraction above, and docs/BYTECODE_VM_DESIGN.md's
// Progress notes on the ASan stack-fragility this caused once already).

static EvalValue eval_getprop(LogoApp *app, EvalValue name_val, EvalValue key_val) {
    char plist_name[32], key[32];
    eval_value_to_text(app, name_val, plist_name, sizeof(plist_name));
    eval_value_to_text(app, key_val, key, sizeof(key));
    int idx = find_plist_entry(app, plist_name, key);
    if (idx < 0) return list_val(-1); // no such property: the empty list
    PlistEntry *e = &app->plist_entries[idx];
    if (e->type == VALUE_WORD) return word_val(e->word);
    if (e->type == VALUE_LIST) return list_val(e->list_head);
    return num_val(e->number); // array-valued properties: not supported yet, same as everywhere else
}

static void eval_setprop(LogoApp *app, EvalValue name_val, EvalValue key_val, EvalValue val) {
    char plist_name[32], key[32];
    eval_value_to_text(app, name_val, plist_name, sizeof(plist_name));
    eval_value_to_text(app, key_val, key, sizeof(key));
    int idx = find_plist_entry(app, plist_name, key);
    if (idx < 0) {
        if (app->plist_entry_count >= MAX_PLIST_ENTRIES) {
            append_output(app, "SETPROP: too many properties defined, not set\n");
            return;
        }
        idx = app->plist_entry_count++;
        snprintf(app->plist_entries[idx].plist_name, sizeof(app->plist_entries[idx].plist_name), "%s", plist_name);
        snprintf(app->plist_entries[idx].key, sizeof(app->plist_entries[idx].key), "%s", key);
    }
    PlistEntry *e = &app->plist_entries[idx];
    e->type = val.type;
    e->number = val.number;
    snprintf(e->word, sizeof(e->word), "%s", val.word);
    e->list_head = val.list_head;
}

static void eval_removeprop(LogoApp *app, EvalValue name_val, EvalValue key_val) {
    char plist_name[32], key[32];
    eval_value_to_text(app, name_val, plist_name, sizeof(plist_name));
    eval_value_to_text(app, key_val, key, sizeof(key));
    int idx = find_plist_entry(app, plist_name, key);
    // Swap-with-last removal, same as interpreter.c's own REMOVEPROP --
    // a plist's remaining entry order isn't documented as stable in
    // any Logo dialect.
    if (idx >= 0) {
        app->plist_entries[idx] = app->plist_entries[--app->plist_entry_count];
    }
}

static EvalValue eval_proplist(LogoApp *app, EvalValue name_val) {
    char plist_name[32];
    eval_value_to_text(app, name_val, plist_name, sizeof(plist_name));
    int head = -1;
    int *next_slot = &head;
    for (int i = 0; i < app->plist_entry_count; i++) {
        PlistEntry *e = &app->plist_entries[i];
        if (strcasecmp(e->plist_name, plist_name) != 0) continue;
        int key_node = value_to_node(app, word_val(e->key));
        if (key_node < 0) return list_pool_exhausted(app);
        *next_slot = key_node;
        next_slot = &app->list_pool[key_node].next;
        EvalValue val;
        if (e->type == VALUE_WORD) val = word_val(e->word);
        else if (e->type == VALUE_LIST) val = list_val(e->list_head);
        else val = num_val(e->number);
        int val_node = value_to_node(app, val);
        if (val_node < 0) return list_pool_exhausted(app);
        *next_slot = val_node;
        next_slot = &app->list_pool[val_node].next;
    }
    return list_val(head);
}

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
            } else if (arg_vals[i].type == VALUE_LIST) {
                slot->list_head = arg_vals[i].list_head;
            } else if (arg_vals[i].type == VALUE_ARRAY) {
                slot->list_head = arg_vals[i].list_head;
                slot->number = arg_vals[i].number;
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

// --- Prototype-style objects (NEW/SEND) -------------------------------
//
// An "object" is just a plist name (see the property-list functions
// above) with a "prototype" property pointing at its parent -- mirrors
// interpreter.c's own NEW/SEND exactly in spirit, sharing the same
// app->plist_entries records, just adapted to this engine's own
// procedure representation (AST_PROC_DEF nodes via find_proc_def,
// not a Procedure*) and its own calling convention (see
// BUILTIN_SIGNATURES's own comment on SEND for why the argument list
// is now explicit rather than positional).

// Walks obj's own plist for `message`, then follows however many
// "prototype" links it takes to find one, bounded by
// MAX_PROTOTYPE_CHAIN_DEPTH (logo_types.h) the same way
// interpreter.c's own resolve_message is -- a cyclic or absurdly long
// chain just runs out of hops rather than looping forever.
static PlistEntry *eval_resolve_message(LogoApp *app, const char *objname, const char *message) {
    char current[32];
    snprintf(current, sizeof(current), "%s", objname);
    for (int depth = 0; depth < MAX_PROTOTYPE_CHAIN_DEPTH; depth++) {
        int idx = find_plist_entry(app, current, message);
        if (idx >= 0) return &app->plist_entries[idx];
        int proto_idx = find_plist_entry(app, current, "prototype");
        if (proto_idx < 0 || app->plist_entries[proto_idx].type != VALUE_WORD) return NULL;
        snprintf(current, sizeof(current), "%s", app->plist_entries[proto_idx].word);
    }
    return NULL;
}

// Resolves `message` to a callable AST_PROC_DEF on obj, reporting
// whichever of SEND's error cases applies -- mirrors interpreter.c's
// own resolve_method, just returning an AST node index (this engine's
// own procedure representation) instead of a Procedure*.
static int eval_resolve_method(LogoApp *app, AstPool *pool, const char *objname, const char *message) {
    PlistEntry *e = eval_resolve_message(app, objname, message);
    if (e == NULL) {
        append_output(app, "SEND: ");
        append_output(app, objname);
        append_output(app, " does not understand ");
        append_output(app, message);
        append_output(app, "\n");
        return -1;
    }
    int def_node = (e->type == VALUE_WORD) ? find_proc_def(pool, e->word) : -1;
    if (def_node < 0) {
        append_output(app, "SEND: ");
        append_output(app, message);
        append_output(app, " is not a method on ");
        append_output(app, objname);
        append_output(app, "\n");
        return -1;
    }
    if (pool->nodes[def_node].param_count < 1) {
        append_output(app, "SEND: ");
        append_output(app, pool->nodes[def_node].text);
        append_output(app, " must take :self as its first input\n");
        return -1;
    }
    return def_node;
}

// --- Per-built-in dispatch functions ---------------------------------
//
// Every single built-in, however small, gets its own function here --
// not inlined as an exec_call branch, even the ones with just one or
// two small locals. This isn't stylistic: exec_call recurses through
// call_ast_procedure for every ordinary procedure call, so at deep
// recursion its own per-call stack frame matters, and confirmed
// directly (twice, in this same file, in one session -- see
// docs/BYTECODE_VM_DESIGN.md's Progress notes): enough small branches
// inlined together add up the same way one huge branch (WORD's own
// two char[512] buffers) already did once. Keeping every branch as a
// real function call means exec_call's own frame stays exactly
// {arg_idx[8], argc, name} regardless of how large BUILTIN_SIGNATURES
// grows -- add the next built-in as its own function too, not as a
// new inline branch, or this will resurface again.

static void do_fd(LogoApp *app, AstPool *pool, const int *arg_idx) {
    move_turtle_forward(app, eval_to_number(eval_expr(app, pool, arg_idx[0])));
}
static void do_bk(LogoApp *app, AstPool *pool, const int *arg_idx) {
    move_turtle_forward(app, -eval_to_number(eval_expr(app, pool, arg_idx[0])));
}
static void do_rt(LogoApp *app, AstPool *pool, const int *arg_idx) {
    current_turtle(app)->angle += eval_to_number(eval_expr(app, pool, arg_idx[0]));
}
static void do_lt(LogoApp *app, AstPool *pool, const int *arg_idx) {
    current_turtle(app)->angle -= eval_to_number(eval_expr(app, pool, arg_idx[0]));
}
static void do_setxy(LogoApp *app, AstPool *pool, const int *arg_idx) {
    double x = eval_to_number(eval_expr(app, pool, arg_idx[0]));
    double y = eval_to_number(eval_expr(app, pool, arg_idx[1]));
    move_turtle_to(app, x, y);
}
static void do_setheading(LogoApp *app, AstPool *pool, const int *arg_idx) {
    current_turtle(app)->angle = eval_to_number(eval_expr(app, pool, arg_idx[0]));
}
static void do_setx(LogoApp *app, AstPool *pool, const int *arg_idx) {
    move_turtle_to(app, eval_to_number(eval_expr(app, pool, arg_idx[0])), current_turtle(app)->y);
}
static void do_sety(LogoApp *app, AstPool *pool, const int *arg_idx) {
    move_turtle_to(app, current_turtle(app)->x, eval_to_number(eval_expr(app, pool, arg_idx[0])));
}
static EvalValue do_getx(LogoApp *app) {
    return num_val(current_turtle(app)->x);
}
static EvalValue do_gety(LogoApp *app) {
    return num_val(current_turtle(app)->y);
}
static EvalValue do_heading(LogoApp *app) {
    // The raw stored angle, same convention SETHEADING/RT/LT already
    // use -- not normalized to 0-360 (RT 720 just keeps adding).
    return num_val(current_turtle(app)->angle);
}
static EvalValue do_pos(LogoApp *app) {
    return eval_list_wrap_pair(app, num_val(current_turtle(app)->x), num_val(current_turtle(app)->y));
}
static EvalValue do_canvassize(LogoApp *app) {
    return eval_list_wrap_pair(app, num_val(app->canvas_width), num_val(app->canvas_height));
}
// Plain distance between two arbitrary [x y] points, not tied to the
// turtle's own position (unlike TOWARDS below) -- pass POS as one of
// the two points for "distance from here". Mirrors interpreter.c's own
// DISTANCE exactly.
static EvalValue do_distance(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue a = eval_expr(app, pool, arg_idx[0]);
    EvalValue b = eval_expr(app, pool, arg_idx[1]);
    double av[2], bv[2];
    if (!eval_list_as_two_numbers(app, a, av) || !eval_list_as_two_numbers(app, b, bv)) {
        append_output(app, "DISTANCE: expected two 2-element lists\n");
        return num_val(0);
    }
    double dx = bv[0] - av[0];
    double dy = bv[1] - av[1];
    return num_val(sqrt(dx * dx + dy * dy));
}
// The heading (same convention as HEADING/SETHEADING/RT/LT) to face
// directly from the turtle's current position toward point -- derived
// from the same dx/dy-vs-heading formula move_turtle_forward uses, so
// SETHEADING TOWARDS point then FORWARD DISTANCE POS point actually
// walks straight to point. Unlike HEADING (a live, unbounded
// accumulator), this is a freshly computed compass bearing, normalized
// to [0, 360). Mirrors interpreter.c's own TOWARDS exactly.
static EvalValue do_towards(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue p = eval_expr(app, pool, arg_idx[0]);
    double pv[2];
    if (!eval_list_as_two_numbers(app, p, pv)) {
        append_output(app, "TOWARDS: expected a 2-element list\n");
        return num_val(0);
    }
    double dx = pv[0] - current_turtle(app)->x;
    double dy = pv[1] - current_turtle(app)->y;
    double heading = atan2(dx, -dy) * 180.0 / M_PI;
    if (heading < 0) heading += 360.0;
    return num_val(heading);
}
static void do_penup(LogoApp *app) {
    current_turtle(app)->pen_down = FALSE;
}
static void do_pendown(LogoApp *app) {
    current_turtle(app)->pen_down = TRUE;
}
static void do_home(LogoApp *app) {
    move_turtle_to(app, home_x(app), home_y(app));
    current_turtle(app)->angle = 0;
}
static void do_clear(LogoApp *app) {
    app->line_count = 0;
    app->label_count = 0;
    app->raster_op_count = 0;
    for (int i = 0; i < app->turtle_count; i++) {
        app->turtles[i].x = home_x(app);
        app->turtles[i].y = home_y(app);
        app->turtles[i].angle = 0;
    }
}
static void do_print(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue v = eval_expr(app, pool, arg_idx[0]);
    char text[2048];
    eval_value_to_text(app, v, text, sizeof(text));
    char buf[2100];
    snprintf(buf, sizeof(buf), "%s\n", text);
    append_output(app, buf);
}
static void do_make(LogoApp *app, AstPool *pool, const int *arg_idx) {
    // arg_idx[0] is an AST_WORD -- the parser's ARG_QUOTED_WORD kind
    // already guarantees this holds the variable's name directly in
    // .text (see parser.c's MAKE signature).
    const char *varname = pool->nodes[arg_idx[0]].text;
    EvalValue val = eval_expr(app, pool, arg_idx[1]);
    if (val.type == VALUE_WORD) set_var_word(app, varname, val.word);
    else if (val.type == VALUE_LIST) set_var_list(app, varname, val.list_head);
    else if (val.type == VALUE_ARRAY) set_var_array(app, varname, val.list_head, (int)val.number);
    else set_var(app, varname, val.number);
}
static void do_output(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue val = eval_expr(app, pool, arg_idx[0]);
    app->output_type = val.type;
    app->output_number = val.number;
    snprintf(app->output_word, sizeof(app->output_word), "%s", val.word);
    app->output_list_head = val.list_head;
    app->has_output_value = TRUE;
    app->stop_requested = TRUE;
}
static void do_stop(LogoApp *app) {
    app->stop_requested = TRUE;
}
static void do_repeat(LogoApp *app, AstPool *pool, const int *arg_idx) {
    int count = (int)eval_to_number(eval_expr(app, pool, arg_idx[0]));
    int block_node = arg_idx[1];
    for (int i = 0; i < count && !app->stop_requested; i++) {
        exec_block(app, pool, block_node);
    }
}
static void do_while(LogoApp *app, AstPool *pool, const int *arg_idx) {
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
}
static EvalValue do_abs(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(fabs(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_sqrt(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(sqrt(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_power(LogoApp *app, AstPool *pool, const int *arg_idx) {
    double base = eval_to_number(eval_expr(app, pool, arg_idx[0]));
    double exponent = eval_to_number(eval_expr(app, pool, arg_idx[1]));
    return num_val(pow(base, exponent));
}
static EvalValue do_random(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(random_below(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_round(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(round(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_int(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(trunc(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
// Mirrors interpreter.c's own mod_result exactly: fmod's result takes
// the sign of the dividend, but MOD's result should take the sign of
// the divisor instead (Python-style, not C-style) -- e.g. -7 MOD 3 is
// 2, not -1.
static double eval_mod_result(double a, double b) {
    if (b == 0) return 0;
    double r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}
static EvalValue do_mod(LogoApp *app, AstPool *pool, const int *arg_idx) {
    double a = eval_to_number(eval_expr(app, pool, arg_idx[0]));
    double b = eval_to_number(eval_expr(app, pool, arg_idx[1]));
    return num_val(eval_mod_result(a, b));
}
// SIN/COS/TAN take degrees (converted to radians); ASIN/ACOS/ARCTAN
// return degrees (converted back from radians) -- matches
// interpreter.c's own convention throughout (SETHEADING/turtle angles
// are degrees), not the C math library's native radians.
static EvalValue do_sin(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(sin(eval_to_number(eval_expr(app, pool, arg_idx[0])) * M_PI / 180.0));
}
static EvalValue do_cos(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(cos(eval_to_number(eval_expr(app, pool, arg_idx[0])) * M_PI / 180.0));
}
static EvalValue do_tan(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(tan(eval_to_number(eval_expr(app, pool, arg_idx[0])) * M_PI / 180.0));
}
static EvalValue do_asin(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(asin(eval_to_number(eval_expr(app, pool, arg_idx[0]))) * 180.0 / M_PI);
}
static EvalValue do_acos(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(acos(eval_to_number(eval_expr(app, pool, arg_idx[0]))) * 180.0 / M_PI);
}
static EvalValue do_arctan(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(atan(eval_to_number(eval_expr(app, pool, arg_idx[0]))) * 180.0 / M_PI);
}
static EvalValue do_ln(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(log(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_log(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(log10(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_exp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return num_val(exp(eval_to_number(eval_expr(app, pool, arg_idx[0]))));
}
static EvalValue do_first(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    if (arg.type == VALUE_LIST) {
        return (arg.list_head < 0) ? word_val("") : node_to_value(&app->list_pool[arg.list_head]);
    }
    if (arg.type == VALUE_WORD) {
        if (arg.word[0] == '\0') return word_val("");
        char ch[2] = {arg.word[0], '\0'};
        return word_val(ch);
    }
    return arg; // FIRST of a bare number is itself
}
static EvalValue do_butfirst(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    if (arg.type == VALUE_LIST) return list_val(arg.list_head < 0 ? -1 : app->list_pool[arg.list_head].next);
    if (arg.type == VALUE_WORD) return word_val(arg.word[0] == '\0' ? "" : arg.word + 1);
    return list_val(-1); // BUTFIRST of a bare number is empty
}
static EvalValue do_last(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    if (arg.type == VALUE_LIST) {
        if (arg.list_head < 0) return word_val("");
        int idx = arg.list_head;
        while (app->list_pool[idx].next != -1) idx = app->list_pool[idx].next;
        return node_to_value(&app->list_pool[idx]);
    }
    if (arg.type == VALUE_WORD) {
        size_t len = strlen(arg.word);
        if (len == 0) return word_val("");
        char ch[2] = {arg.word[len - 1], '\0'};
        return word_val(ch);
    }
    return arg; // LAST of a bare number is itself
}
static EvalValue do_butlast(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    if (arg.type == VALUE_LIST) return eval_list_butlast(app, arg);
    if (arg.type == VALUE_WORD) {
        size_t len = strlen(arg.word);
        if (len == 0) return word_val("");
        EvalValue r = word_val(arg.word);
        r.word[len - 1] = '\0';
        return r;
    }
    return list_val(-1); // BUTLAST of a bare number is empty
}
static EvalValue do_count(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    if (arg.type == VALUE_LIST) {
        double count = 0;
        for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
        return num_val(count);
    }
    if (arg.type == VALUE_WORD) return num_val((double)strlen(arg.word));
    if (arg.type == VALUE_ARRAY) return num_val(arg.number); // an array's `number` field holds its length
    return num_val(1); // a bare number counts as a one-element list
}
static EvalValue do_empty(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    int empty;
    if (arg.type == VALUE_LIST) empty = (arg.list_head == -1);
    else if (arg.type == VALUE_WORD) empty = (arg.word[0] == '\0');
    else empty = 0; // a number is never "empty"
    return word_val(empty ? "TRUE" : "FALSE");
}
// Type predicates -- mirror interpreter.c's own WORD?/LIST?/NUMBER?/
// ARRAY? exactly: each just checks the evaluated argument's own
// ValueType tag (shared verbatim between the two engines, see
// logo_types.h), no coercion.
static EvalValue do_wordp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    return word_val(arg.type == VALUE_WORD ? "TRUE" : "FALSE");
}
static EvalValue do_listp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    return word_val(arg.type == VALUE_LIST ? "TRUE" : "FALSE");
}
static EvalValue do_numberp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    return word_val(arg.type == VALUE_NUMBER ? "TRUE" : "FALSE");
}
static EvalValue do_arrayp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue arg = eval_expr(app, pool, arg_idx[0]);
    return word_val(arg.type == VALUE_ARRAY ? "TRUE" : "FALSE");
}
static EvalValue do_fput(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_list_fput(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
static EvalValue do_lput(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_list_lput(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
static EvalValue do_word(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_word_concat(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
static EvalValue do_sentence(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_list_sentence(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
static EvalValue do_list(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_list_wrap_pair(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// MEMBER? thing container -- a list checks element membership
// (eval_values_equal per element), a word checks substring containment
// (its "elements" are characters), anything else (a number, or an
// array -- interpreter.c has no special ARRAY case here either) falls
// back to treating `container` as a single one-element value. Mirrors
// interpreter.c's own MEMBER? exactly, including that fallback.
static EvalValue do_memberp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue thing = eval_expr(app, pool, arg_idx[0]);
    EvalValue container = eval_expr(app, pool, arg_idx[1]);
    if (container.type == VALUE_LIST) {
        for (int idx = container.list_head; idx != -1; idx = app->list_pool[idx].next) {
            if (eval_values_equal(app, thing, node_to_value(&app->list_pool[idx]))) return word_val("TRUE");
        }
        return word_val("FALSE");
    }
    if (container.type == VALUE_WORD) {
        char thing_text[512];
        eval_value_to_text(app, thing, thing_text, sizeof(thing_text));
        return word_val(strstr(container.word, thing_text) != NULL ? "TRUE" : "FALSE");
    }
    return word_val(eval_values_equal(app, thing, container) ? "TRUE" : "FALSE");
}
// ARRAY size -- allocates `size` contiguous list_pool cells (direct
// index math, not chain-walked -- see array_val's own comment), each
// starting out an empty list. Mirrors interpreter.c's own ARRAY exactly.
static EvalValue do_array(LogoApp *app, AstPool *pool, const int *arg_idx) {
    int size = (int)eval_to_number(eval_expr(app, pool, arg_idx[0]));
    if (size < 1) {
        append_output(app, "ARRAY: size must be at least 1\n");
        return word_val("");
    }
    int start = app->list_pool_count;
    for (int i = 0; i < size; i++) {
        int idx = list_alloc_node(app);
        if (idx < 0) return list_pool_exhausted(app);
        app->list_pool[idx].type = LIST_ELEM_LIST;
        app->list_pool[idx].sublist_head = -1;
        app->list_pool[idx].next = -1;
    }
    return array_val(start, size);
}
// ITEM index thing -- 1-indexed lookup into a list (chain walk), word
// (character extract), array (direct index math), or bare number
// (only index 1 valid). Mirrors interpreter.c's own ITEM exactly.
static EvalValue do_item(LogoApp *app, AstPool *pool, const int *arg_idx) {
    int index = (int)eval_to_number(eval_expr(app, pool, arg_idx[0]));
    EvalValue thing = eval_expr(app, pool, arg_idx[1]);
    if (thing.type == VALUE_LIST) {
        int i = 1;
        for (int idx = thing.list_head; idx != -1; idx = app->list_pool[idx].next, i++) {
            if (i == index) return node_to_value(&app->list_pool[idx]);
        }
        append_output(app, "ITEM: index out of range\n");
        return word_val("");
    }
    if (thing.type == VALUE_WORD) {
        int len = (int)strlen(thing.word);
        if (index < 1 || index > len) {
            append_output(app, "ITEM: index out of range\n");
            return word_val("");
        }
        char ch[2] = {thing.word[index - 1], '\0'};
        return word_val(ch);
    }
    if (thing.type == VALUE_ARRAY) {
        if (index < 1 || index > (int)thing.number) {
            append_output(app, "ITEM: index out of range\n");
            return word_val("");
        }
        return node_to_value(&app->list_pool[thing.list_head + (index - 1)]);
    }
    if (index == 1) return thing; // a bare number counts as a one-element list
    append_output(app, "ITEM: index out of range\n");
    return word_val("");
}
// Shared by SETITEM/FILLARRAY: overwrite one list_pool cell in place
// with `new_val`'s payload. Caller has already checked new_val isn't
// itself an array.
static void eval_array_store_cell(ListNode *node, EvalValue new_val) {
    if (new_val.type == VALUE_NUMBER) {
        node->type = LIST_ELEM_NUMBER;
        node->number = new_val.number;
    } else if (new_val.type == VALUE_LIST) {
        node->type = LIST_ELEM_LIST;
        node->sublist_head = new_val.list_head;
    } else {
        node->type = LIST_ELEM_WORD;
        snprintf(node->word, sizeof(node->word), "%s", new_val.word);
    }
}
// SETITEM index array value -- the one in-place mutation in this
// language (see set_var_array's own comment). Mirrors interpreter.c's
// own SETITEM exactly.
static void do_setitem(LogoApp *app, AstPool *pool, const int *arg_idx) {
    int index = (int)eval_to_number(eval_expr(app, pool, arg_idx[0]));
    EvalValue array_val_ = eval_expr(app, pool, arg_idx[1]);
    EvalValue new_val = eval_expr(app, pool, arg_idx[2]);
    if (array_val_.type != VALUE_ARRAY) {
        append_output(app, "SETITEM: expected an array\n");
    } else if (index < 1 || index > (int)array_val_.number) {
        append_output(app, "SETITEM: index out of range\n");
    } else if (new_val.type == VALUE_ARRAY) {
        append_output(app, "SETITEM: can't store an array inside an array\n");
    } else {
        eval_array_store_cell(&app->list_pool[array_val_.list_head + (index - 1)], new_val);
    }
}
// FILLARRAY array value -- SETITEM's per-cell assignment looped over
// every index instead of just one. Mirrors interpreter.c's own
// FILLARRAY exactly.
static void do_fillarray(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue array_val_ = eval_expr(app, pool, arg_idx[0]);
    EvalValue new_val = eval_expr(app, pool, arg_idx[1]);
    if (array_val_.type != VALUE_ARRAY) {
        append_output(app, "FILLARRAY: expected an array\n");
    } else if (new_val.type == VALUE_ARRAY) {
        append_output(app, "FILLARRAY: can't store an array inside an array\n");
    } else {
        for (int i = 0; i < (int)array_val_.number; i++) {
            eval_array_store_cell(&app->list_pool[array_val_.list_head + i], new_val);
        }
    }
}
// MAP template list -- applies `template` (see the file comment near
// eval_apply_template_expr) to each element of `list`, collecting the
// results into a new list. Mirrors interpreter.c's own MAP exactly,
// including its handling of a non-list `list` argument (wrapped as a
// one-element list via value_to_node first, same as
// list_node_from_value there).
static EvalValue do_map(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue template_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue list_arg = eval_expr(app, pool, arg_idx[1]);
    char template_text[512];
    eval_value_to_text(app, template_val, template_text, sizeof(template_text));

    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    ParseResult *scratch = calloc(1, sizeof(ParseResult));
    int new_head = -1;
    int *next_slot = &new_head;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        EvalValue result = eval_apply_template_expr(app, template_text, node_to_value(&app->list_pool[idx]), scratch);
        int node = value_to_node(app, result);
        if (node < 0) {
            free(scratch);
            return list_pool_exhausted(app);
        }
        *next_slot = node;
        next_slot = &app->list_pool[node].next;
    }
    free(scratch);
    return list_val(new_head);
}
// FILTER template list -- keeps each element of `list` whose template
// (evaluated as a *condition*, e.g. `[? > 2]`) is truthy, in a new
// list; a kept element is copied as-is (list_node_copy), not replaced
// by the template's own boolean result. Mirrors interpreter.c's own
// FILTER exactly.
static EvalValue do_filter(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue template_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue list_arg = eval_expr(app, pool, arg_idx[1]);
    char template_text[512];
    eval_value_to_text(app, template_val, template_text, sizeof(template_text));

    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    ParseResult *scratch = calloc(1, sizeof(ParseResult));
    int new_head = -1;
    int *next_slot = &new_head;
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        if (eval_apply_template_condition(app, template_text, node_to_value(&app->list_pool[idx]), scratch)) {
            int node = list_node_copy(app, idx);
            if (node < 0) {
                free(scratch);
                return list_pool_exhausted(app);
            }
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    free(scratch);
    return list_val(new_head);
}
// REDUCE template list -- folds left-to-right, seeding the accumulator
// with the list's own first element (no separate start-value
// argument); the template uses ?1 for the accumulator so far and ?2
// for the current element (REDUCE [?1 + ?2] [1 2 3 4] sums to 10).
// Inlines its own two-placeholder substitution rather than reusing
// eval_apply_template_expr (which only knows "?"), same as
// interpreter.c's own REDUCE does relative to apply_template_expr.
static EvalValue do_reduce(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue template_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue list_arg = eval_expr(app, pool, arg_idx[1]);
    char template_text[512];
    eval_value_to_text(app, template_val, template_text, sizeof(template_text));

    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    if (iter_head == -1) return num_val(0); // nothing to reduce

    ParseResult *scratch = calloc(1, sizeof(ParseResult));
    EvalValue acc = node_to_value(&app->list_pool[iter_head]);
    for (int idx = app->list_pool[iter_head].next; idx != -1; idx = app->list_pool[idx].next) {
        char acc_text[512], el_text[512], after_1[512], expr_text[512];
        EvalValue el = node_to_value(&app->list_pool[idx]);
        eval_value_to_source_text(app, acc, acc_text, sizeof(acc_text));
        eval_value_to_source_text(app, el, el_text, sizeof(el_text));
        eval_substitute_placeholder(template_text, "?1", acc_text, after_1, sizeof(after_1));
        eval_substitute_placeholder(after_1, "?2", el_text, expr_text, sizeof(expr_text));
        LogoToken tokens[MAX_TEMPLATE_TOKENS];
        int n = logo_lex(expr_text, tokens, MAX_TEMPLATE_TOKENS);
        int node = (n < 0) ? -1 : logo_parse_expr(tokens, n, scratch);
        if (node >= 0) acc = eval_expr(app, &scratch->pool, node);
    }
    free(scratch);
    return acc;
}
// FOREACH template list -- runs `template` (with "?" substituted for
// each element in turn) as a *statement*, not an expression, mirroring
// interpreter.c's own FOREACH exactly (a plain eval_logo call per
// element there). Unlike MAP/FILTER/REDUCE there's no
// logo_parse_expr/logo_parse_condition equivalent to reuse -- a
// statement can be an arbitrary sequence of commands (`[PRINT ? MAKE
// "sum :sum + ?]`), so this goes through logo_parse itself (the same
// whole-program entry point every real script uses) and runs the
// result through exec_block. Same caller-reused scratch ParseResult as
// the others. A malformed substituted statement is skipped rather than
// executed, the same "quietly inert" fallback used elsewhere in this
// file for a bad template. OUTPUT/STOP inside the template ends the
// loop early via app->stop_requested, the same flag REPEAT/WHILE check
// -- there's no THROW/interrupt equivalent in this engine yet to also
// check, unlike interpreter.c's own three-way condition here.
static void do_foreach(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue template_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue list_arg = eval_expr(app, pool, arg_idx[1]);
    char template_text[512];
    eval_value_to_text(app, template_val, template_text, sizeof(template_text));

    int iter_head = (list_arg.type == VALUE_LIST) ? list_arg.list_head : value_to_node(app, list_arg);
    ParseResult *scratch = calloc(1, sizeof(ParseResult));
    for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
        char el_text[512], code_text[512];
        eval_value_to_source_text(app, node_to_value(&app->list_pool[idx]), el_text, sizeof(el_text));
        eval_substitute_placeholder(template_text, "?", el_text, code_text, sizeof(code_text));
        LogoToken tokens[MAX_TEMPLATE_TOKENS];
        int n = logo_lex(code_text, tokens, MAX_TEMPLATE_TOKENS);
        if (n >= 0) {
            logo_parse(tokens, n, scratch);
            if (scratch->error_count == 0) exec_block(app, &scratch->pool, scratch->program);
        }
        if (app->stop_requested) break;
    }
    free(scratch);
}
static void do_setprop(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setprop(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]), eval_expr(app, pool, arg_idx[2]));
}
static EvalValue do_getprop(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_getprop(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
static void do_removeprop(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_removeprop(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
static EvalValue do_proplist(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_proplist(app, eval_expr(app, pool, arg_idx[0]));
}
static void do_new(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue obj_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue proto_val = eval_expr(app, pool, arg_idx[1]);
    eval_setprop(app, obj_val, word_val("prototype"), proto_val);
}
// SEND obj "message arglist -- resolves `message` through obj's
// prototype chain, then calls it with obj prepended as :self and
// arglist's own elements (a non-list arglist counts as one element,
// same convention APPLY already uses for its own argument list). Sets
// *resolved = 0 for SEND's own internal error cases (unknown message,
// a non-method property, wrong argument count) so eval_expr's generic
// "didn't output a value" wrapper doesn't ALSO fire after one of these
// already-specific messages -- mirrors do_user_procedure_call's own
// use of *resolved for the same reason.
static EvalValue do_send(LogoApp *app, AstPool *pool, const int *arg_idx, int *resolved, int *produced) {
    EvalValue obj_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue msg_val = eval_expr(app, pool, arg_idx[1]);
    char obj_text[32], msg_text[32];
    eval_value_to_text(app, obj_val, obj_text, sizeof(obj_text));
    eval_value_to_text(app, msg_val, msg_text, sizeof(msg_text));

    int def_node = eval_resolve_method(app, pool, obj_text, msg_text);
    if (def_node < 0) {
        if (resolved != NULL) *resolved = 0;
        return word_val("");
    }

    EvalValue arglist = eval_expr(app, pool, arg_idx[2]);
    EvalValue arg_vals[AST_MAX_PARAMS];
    arg_vals[0] = word_val(obj_text);
    int n = 1;
    if (arglist.type == VALUE_LIST) {
        for (int idx = arglist.list_head; idx != -1 && n < AST_MAX_PARAMS; idx = app->list_pool[idx].next) {
            arg_vals[n++] = node_to_value(&app->list_pool[idx]);
        }
    } else if (n < AST_MAX_PARAMS) {
        arg_vals[n++] = arglist;
    }

    AstNode *def = &pool->nodes[def_node];
    if (n != def->param_count) {
        append_output(app, "SEND: wrong number of inputs for message \"");
        append_output(app, msg_text);
        append_output(app, "\n");
        if (resolved != NULL) *resolved = 0;
        return word_val("");
    }

    int did_output;
    EvalValue r = call_ast_procedure(app, pool, def_node, arg_vals, n, &did_output);
    if (!did_output && produced != NULL) {
        // Only the operator/expression-position caller (produced !=
        // NULL) cares whether this produced a value -- a plain
        // statement-position SEND (produced == NULL, the same
        // convention every other command here follows) silently
        // discards a missing OUTPUT, exactly like an ordinary
        // procedure call does. Uses the message name here, not
        // "SEND" -- eval_expr's own generic wrapper would use the
        // AST_CALL node's own text ("SEND"), the wrong name for this
        // specific message, so *resolved is cleared too to make sure
        // that generic wrapper never also fires on top of this one.
        append_output(app, msg_text);
        append_output(app, ": didn't output a value\n");
        if (resolved != NULL) *resolved = 0;
        return word_val("");
    }
    if (produced != NULL) *produced = did_output;
    return r;
}

// Not a built-in -- must be a hoisted user procedure: the parser
// already guarantees this by construction (an AST_CALL node only
// exists for a name that resolved to a builtin or a hoisted procedure
// at parse time -- see parser.c's try_parse_call). find_proc_def
// failing here would mean the parser's hoisting and this evaluator's
// own tree-search fell out of sync, not a normal user error. Its own
// function for the same stack-frame reason as every do_* above, and
// because it's the recursive case: every user-procedure call chains
// back through call_ast_procedure from here.
static EvalValue do_user_procedure_call(LogoApp *app, AstPool *pool, const char *name, const int *arg_idx, int argc, int *resolved, int *produced) {
    int def_node = find_proc_def(pool, name);
    if (def_node < 0) {
        if (resolved != NULL) *resolved = 0;
        append_output(app, "I don't know how to ");
        append_output(app, name);
        append_output(app, "\n");
        return num_val(0);
    }
    EvalValue arg_vals[AST_MAX_PARAMS];
    int n = argc < AST_MAX_PARAMS ? argc : AST_MAX_PARAMS;
    for (int i = 0; i < n; i++) arg_vals[i] = eval_expr(app, pool, arg_idx[i]);
    int did_output;
    EvalValue r = call_ast_procedure(app, pool, def_node, arg_vals, n, &did_output);
    if (produced != NULL) *produced = did_output;
    return r;
}

// One AST_CALL node -- a built-in or a hoisted user procedure, in
// either statement position (resolved/produced/result may be NULL,
// meaning "discard whatever this produces") or expression position
// (all three captured). See parser.c's BUILTIN_SIGNATURES for which
// built-ins are covered; growing this alongside that table is
// incremental follow-up work -- as its own do_* function each time
// (see the file comment above the first one), not a new inline
// branch here.
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

    if (strcasecmp(name, "FD") == 0 || strcasecmp(name, "FORWARD") == 0) {
        do_fd(app, pool, arg_idx);
    } else if (strcasecmp(name, "BK") == 0 || strcasecmp(name, "BACK") == 0) {
        do_bk(app, pool, arg_idx);
    } else if (strcasecmp(name, "RT") == 0 || strcasecmp(name, "RIGHT") == 0) {
        do_rt(app, pool, arg_idx);
    } else if (strcasecmp(name, "LT") == 0 || strcasecmp(name, "LEFT") == 0) {
        do_lt(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETXY") == 0) {
        do_setxy(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETHEADING") == 0 || strcasecmp(name, "SETH") == 0) {
        do_setheading(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETX") == 0) {
        do_setx(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETY") == 0) {
        do_sety(app, pool, arg_idx);
    } else if (strcasecmp(name, "GETX") == 0) {
        if (result != NULL) *result = do_getx(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "GETY") == 0) {
        if (result != NULL) *result = do_gety(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "HEADING") == 0) {
        if (result != NULL) *result = do_heading(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "POS") == 0) {
        if (result != NULL) *result = do_pos(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "CANVASSIZE") == 0) {
        if (result != NULL) *result = do_canvassize(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "DISTANCE") == 0) {
        if (result != NULL) *result = do_distance(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "TOWARDS") == 0) {
        if (result != NULL) *result = do_towards(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "PENUP") == 0 || strcasecmp(name, "PU") == 0) {
        do_penup(app);
    } else if (strcasecmp(name, "PENDOWN") == 0 || strcasecmp(name, "PD") == 0) {
        do_pendown(app);
    } else if (strcasecmp(name, "HOME") == 0) {
        do_home(app);
    } else if (strcasecmp(name, "CLEAR") == 0 || strcasecmp(name, "CS") == 0) {
        do_clear(app);
    } else if (strcasecmp(name, "PRINT") == 0) {
        do_print(app, pool, arg_idx);
    } else if (strcasecmp(name, "MAKE") == 0) {
        do_make(app, pool, arg_idx);
    } else if (strcasecmp(name, "OUTPUT") == 0) {
        do_output(app, pool, arg_idx);
    } else if (strcasecmp(name, "STOP") == 0) {
        do_stop(app);
    } else if (strcasecmp(name, "REPEAT") == 0) {
        do_repeat(app, pool, arg_idx);
    } else if (strcasecmp(name, "WHILE") == 0) {
        do_while(app, pool, arg_idx);
    } else if (strcasecmp(name, "ABS") == 0) {
        if (result != NULL) *result = do_abs(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SQRT") == 0) {
        if (result != NULL) *result = do_sqrt(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "POWER") == 0) {
        if (result != NULL) *result = do_power(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "RANDOM") == 0) {
        if (result != NULL) *result = do_random(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ROUND") == 0) {
        if (result != NULL) *result = do_round(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "INT") == 0) {
        if (result != NULL) *result = do_int(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "MOD") == 0) {
        if (result != NULL) *result = do_mod(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SIN") == 0) {
        if (result != NULL) *result = do_sin(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "COS") == 0) {
        if (result != NULL) *result = do_cos(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "TAN") == 0) {
        if (result != NULL) *result = do_tan(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ASIN") == 0) {
        if (result != NULL) *result = do_asin(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ACOS") == 0) {
        if (result != NULL) *result = do_acos(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ARCTAN") == 0) {
        if (result != NULL) *result = do_arctan(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LN") == 0) {
        if (result != NULL) *result = do_ln(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LOG") == 0) {
        if (result != NULL) *result = do_log(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "EXP") == 0) {
        if (result != NULL) *result = do_exp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "FIRST") == 0) {
        if (result != NULL) *result = do_first(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "BUTFIRST") == 0) {
        if (result != NULL) *result = do_butfirst(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LAST") == 0) {
        if (result != NULL) *result = do_last(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "BUTLAST") == 0) {
        if (result != NULL) *result = do_butlast(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "COUNT") == 0) {
        if (result != NULL) *result = do_count(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "EMPTY?") == 0) {
        if (result != NULL) *result = do_empty(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "WORD?") == 0) {
        if (result != NULL) *result = do_wordp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LIST?") == 0) {
        if (result != NULL) *result = do_listp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "NUMBER?") == 0) {
        if (result != NULL) *result = do_numberp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ARRAY?") == 0) {
        if (result != NULL) *result = do_arrayp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "FPUT") == 0) {
        if (result != NULL) *result = do_fput(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LPUT") == 0) {
        if (result != NULL) *result = do_lput(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "WORD") == 0) {
        if (result != NULL) *result = do_word(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SENTENCE") == 0 || strcasecmp(name, "SE") == 0) {
        if (result != NULL) *result = do_sentence(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LIST") == 0) {
        if (result != NULL) *result = do_list(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "MEMBER?") == 0) {
        if (result != NULL) *result = do_memberp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "MAP") == 0) {
        if (result != NULL) *result = do_map(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "FILTER") == 0) {
        if (result != NULL) *result = do_filter(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "REDUCE") == 0) {
        if (result != NULL) *result = do_reduce(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "FOREACH") == 0) {
        do_foreach(app, pool, arg_idx);
    } else if (strcasecmp(name, "ARRAY") == 0) {
        if (result != NULL) *result = do_array(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "ITEM") == 0) {
        if (result != NULL) *result = do_item(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SETITEM") == 0) {
        do_setitem(app, pool, arg_idx);
    } else if (strcasecmp(name, "FILLARRAY") == 0) {
        do_fillarray(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETPROP") == 0) {
        do_setprop(app, pool, arg_idx);
    } else if (strcasecmp(name, "GETPROP") == 0) {
        if (result != NULL) *result = do_getprop(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "REMOVEPROP") == 0) {
        do_removeprop(app, pool, arg_idx);
    } else if (strcasecmp(name, "PROPLIST") == 0) {
        if (result != NULL) *result = do_proplist(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "NEW") == 0) {
        do_new(app, pool, arg_idx);
    } else if (strcasecmp(name, "SEND") == 0) {
        EvalValue r = do_send(app, pool, arg_idx, resolved, produced);
        if (result != NULL) *result = r;
    } else {
        EvalValue r = do_user_procedure_call(app, pool, name, arg_idx, argc, resolved, produced);
        if (result != NULL) *result = r;
    }
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
            if (v->type == VALUE_LIST) return list_val(v->list_head);
            return array_val(v->list_head, (int)v->number);
        }
        case AST_LIST_LITERAL: {
            // Builds a real list in app->list_pool, mirroring
            // interpreter.c's own parse_list_literal exactly: each
            // leaf is untyped raw text (an AST_WORD child's .text,
            // never number-vs-word typed at construction time), and a
            // nested AST_LIST_LITERAL recurses through this same case
            // via eval_expr.
            int head = -1;
            int *next_slot = &head;
            for (int c = node->first_child; c >= 0; c = pool->nodes[c].next_sibling) {
                AstNode *child = &pool->nodes[c];
                int idx = list_alloc_node(app);
                if (idx < 0) return list_pool_exhausted(app);
                ListNode *ln = &app->list_pool[idx];
                ln->next = -1;
                if (child->type == AST_LIST_LITERAL) {
                    EvalValue sub = eval_expr(app, pool, c);
                    ln->type = LIST_ELEM_LIST;
                    ln->sublist_head = sub.list_head;
                } else {
                    ln->type = LIST_ELEM_WORD;
                    snprintf(ln->word, sizeof(ln->word), "%s", child->text);
                }
                *next_slot = idx;
                next_slot = &app->list_pool[idx].next;
            }
            return list_val(head);
        }
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
                int equal = eval_values_equal(app, left, right);
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
