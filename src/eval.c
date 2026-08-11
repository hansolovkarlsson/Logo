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
#include <glib/gstdio.h> // g_remove (DELETEFILE)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// EvalValue and its num_val/word_val/list_val/array_val constructors
// now live in eval.h (see that file's own comment) -- exposed so
// vm.c/compiler.c share the exact same value representation.

// A "list storage full" report shared by every list-construction
// operator below -- same wording and "loud, not silent" policy as
// interpreter.c's own list_pool_exhausted_error.
EvalValue list_pool_exhausted(LogoApp *app) {
    append_output(app, "list storage full, list operation ignored\n");
    return word_val("");
}

// Map a list element node to an EvalValue -- mirrors interpreter.c's
// own list_node_to_value.
EvalValue node_to_value(const ListNode *node) {
    if (node->type == LIST_ELEM_NUMBER) return num_val(node->number);
    if (node->type == LIST_ELEM_LIST) return list_val(node->sublist_head);
    return word_val(node->word);
}

// Store `v` as a single new list-element node (a list contributes its
// existing chain by index, no deep copy needed) -- mirrors
// interpreter.c's own list_node_from_value, built on the now-exposed
// list_alloc_node. Returns -1 (pool exhausted) same as that function.
int value_to_node(LogoApp *app, EvalValue v) {
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
void eval_value_to_text(LogoApp *app, EvalValue v, char *out, size_t out_size) {
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
double eval_to_number(EvalValue v) {
    if (v.type == VALUE_NUMBER) return v.number;
    if (v.type == VALUE_LIST) return 0;
    return strtod(v.word, NULL);
}

// A value's truthiness -- mirrors interpreter.c's is_truthy.
int eval_is_truthy(EvalValue v) {
    if (v.type == VALUE_LIST) return v.list_head != -1;
    if (v.type == VALUE_WORD) return v.word[0] != '\0' && strcasecmp(v.word, "FALSE") != 0;
    return v.number != 0;
}

// Equality for = / <> when at least one side isn't a number --
// mirrors interpreter.c's values_equal (text comparison,
// case-insensitive; a list renders through eval_value_to_text same as
// anything else).
int eval_values_equal(LogoApp *app, EvalValue a, EvalValue b) {
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

EvalValue eval_list_butlast(LogoApp *app, EvalValue arg) {
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

EvalValue eval_list_fput(LogoApp *app, EvalValue thing, EvalValue list) {
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

EvalValue eval_list_lput(LogoApp *app, EvalValue thing, EvalValue list) {
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

EvalValue eval_list_sentence(LogoApp *app, EvalValue a, EvalValue b) {
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
EvalValue eval_word_concat(LogoApp *app, EvalValue a, EvalValue b) {
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

EvalValue eval_list_wrap_pair(LogoApp *app, EvalValue a, EvalValue b) {
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

// Same as eval_list_as_two_numbers, but for a 3-element vector --
// CROSS's own "same length" check, since a 3D cross product is only
// defined for 3-element vectors. Mirrors interpreter.c's own
// list_as_three_numbers exactly.
static int eval_list_as_three_numbers(LogoApp *app, EvalValue v, double out[3]) {
    if (v.type != VALUE_LIST) return 0;
    int n = 0;
    for (int idx = v.list_head; idx != -1; idx = app->list_pool[idx].next, n++) {
        if (n < 3) out[n] = eval_to_number(node_to_value(&app->list_pool[idx]));
    }
    return n == 3;
}

// Tokenizes `text` (exactly `text_len` bytes, not necessarily
// NUL-terminated -- TEXT's own caller hands this a span straight out
// of the original source buffer, see do_text below) by whitespace into
// a list of words -- the shared core of PARSE (any value's printed
// text) and TEXT (a procedure's raw body text). Pure text split, not
// bracket- or quote-aware, mirroring interpreter.c's own
// list_tokenize_words exactly: a quoted word or a [bracketed block] in
// the source keeps its punctuation as literal characters glued onto
// whichever token it's part of. Sets *out_head and returns 1, or
// returns 0 (pool exhausted partway through) -- an out-parameter, not a
// -1 return, because -1 (empty list) is itself a legitimate successful
// result here (empty/all-whitespace text), not distinguishable from
// failure if the head were the only thing returned.
static int eval_list_tokenize_words(LogoApp *app, const char *text, size_t text_len, int *out_head) {
    int head = -1;
    int *next_slot = &head;
    const char *p = text;
    const char *end = text + text_len;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        const char *start = p;
        while (p < end && !isspace((unsigned char)*p)) p++;
        int node = list_alloc_node(app);
        if (node < 0) return 0;
        size_t len = (size_t)(p - start);
        if (len >= sizeof(app->list_pool[node].word)) len = sizeof(app->list_pool[node].word) - 1;
        memcpy(app->list_pool[node].word, start, len);
        app->list_pool[node].word[len] = '\0';
        app->list_pool[node].type = LIST_ELEM_WORD;
        app->list_pool[node].next = -1;
        *next_slot = node;
        next_slot = &app->list_pool[node].next;
    }
    *out_head = head;
    return 1;
}

// FLATTEN: recursively collects every leaf (number/word) reachable from
// `list_head`'s chain into one flat chain, appended via *next_slot --
// a sublist's own container node is discarded, only its leaves survive
// in order. Mirrors interpreter.c's own list_flatten_into exactly.
// Returns 0 (pool exhausted partway through) or 1.
static int eval_list_flatten_into(LogoApp *app, int list_head, int **next_slot) {
    for (int idx = list_head; idx != -1; idx = app->list_pool[idx].next) {
        ListNode *node = &app->list_pool[idx];
        if (node->type == LIST_ELEM_LIST) {
            if (!eval_list_flatten_into(app, node->sublist_head, next_slot)) return 0;
        } else {
            int new_idx = list_alloc_node(app);
            if (new_idx < 0) return 0;
            app->list_pool[new_idx] = *node;
            app->list_pool[new_idx].next = -1;
            **next_slot = new_idx;
            *next_slot = &app->list_pool[new_idx].next;
        }
    }
    return 1;
}

// SUBST: rebuilds `list_head`'s chain, replacing every element equal to
// `old_val` (compared the same way MEMBER?/eval_values_equal already
// do, so a whole matching sublist substitutes as one unit too, not just
// a leaf) with `new_val`. A non-matching sublist is recursed into and
// rebuilt in place. Mirrors interpreter.c's own list_subst_into
// exactly. Sets *out_head and returns 1, or returns 0 (pool exhausted
// partway through) -- same out-parameter reasoning as
// eval_list_tokenize_words above (an empty resulting list is itself a
// legitimate -1 head, not an error).
static int eval_list_subst_into(LogoApp *app, int list_head, EvalValue old_val, EvalValue new_val, int *out_head) {
    int head = -1;
    int *next_slot = &head;
    for (int idx = list_head; idx != -1; idx = app->list_pool[idx].next) {
        ListNode *node = &app->list_pool[idx];
        EvalValue elem = node_to_value(node);
        int new_idx;
        if (eval_values_equal(app, elem, old_val)) {
            new_idx = value_to_node(app, new_val);
        } else if (node->type == LIST_ELEM_LIST) {
            int sub_head;
            if (!eval_list_subst_into(app, node->sublist_head, old_val, new_val, &sub_head)) return 0;
            new_idx = list_alloc_node(app);
            if (new_idx >= 0) {
                app->list_pool[new_idx].type = LIST_ELEM_LIST;
                app->list_pool[new_idx].sublist_head = sub_head;
            }
        } else {
            new_idx = value_to_node(app, elem);
        }
        if (new_idx < 0) return 0;
        app->list_pool[new_idx].next = -1;
        *next_slot = new_idx;
        next_slot = &app->list_pool[new_idx].next;
    }
    *out_head = head;
    return 1;
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

EvalValue eval_getprop(LogoApp *app, EvalValue name_val, EvalValue key_val) {
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

void eval_setprop(LogoApp *app, EvalValue name_val, EvalValue key_val, EvalValue val) {
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

void eval_removeprop(LogoApp *app, EvalValue name_val, EvalValue key_val) {
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

EvalValue eval_proplist(LogoApp *app, EvalValue name_val) {
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
// find_proc_def now lives in ast.c/ast.h -- it never touched LogoApp,
// only `pool`, so it belongs with the rest of the AST-only module (see
// ast.h's own comment on why), and moving it there lets compiler.c
// call it directly without pulling in this file's interpreter.h
// dependency.

// The scope-push half of a procedure call (bind arg_vals as def's own
// parameters, push onto `ss`'s own scope stack) -- split out of
// call_ast_procedure so vm.c's OP_CALL_PROC/OP_SEND/OP_APPLY handlers
// can share it too, each passing vm_scope_stack(vm) instead of
// call_ast_procedure's own app_scope_stack(app) -- see ScopeStack's own
// comment in logo_types.h. vm.c's own frames don't run exec_block (a
// compiled procedure body runs through the VM's own instruction loop
// instead), so it has no use for the rest of call_ast_procedure (the
// exec_block call and the OUTPUT/STOP-catching afterward -- the VM's
// OP_OUTPUT/OP_STOP opcodes do that job directly against its own frame
// stack), only this setup half. Returns 0 (and leaves scope_depth/app
// output untouched beyond the message) if already at `ss.capacity`,
// matching call_ast_procedure's own prior behavior exactly.
int eval_push_scope_for_call(LogoApp *app, ScopeStack ss, const char *proc_name, int param_count, const char param_names[][32], EvalValue *arg_vals, int arg_count) {
    if (*ss.scope_depth >= ss.capacity) {
        append_output(app, "Recursion too deep, call ignored\n");
        return 0;
    }
    Scope *scope = &ss.scopes[*ss.scope_depth];
    scope->count = param_count;
    snprintf(scope->proc_name, sizeof(scope->proc_name), "%s", proc_name);
    for (int i = 0; i < param_count; i++) {
        snprintf(scope->vars[i].name, sizeof(scope->vars[i].name), "%s", param_names[i]);
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
    (*ss.scope_depth)++;
    return 1;
}

// Calls the AST_PROC_DEF at def_node with arg_vals bound as its
// parameters. Reimplements interpreter.c's own call_procedure logic
// (push a scope, bind params, run the body, catch OUTPUT/STOP, pop the
// scope) rather than calling that function directly -- it's hardwired
// to a text-based Procedure.body run through eval_logo, while an
// AST_PROC_DEF's body is already a parsed AST_BLOCK run through
// exec_block instead. Always uses app's own scope stack (ast_eval has
// no Vm to own a separate one) -- shares the exact same push/pop logic
// and find_var the VM uses against its own array, so the two engines
// can't drift on scoping *semantics* even though scope *storage* (and
// now capacity) differs.
static EvalValue call_ast_procedure(LogoApp *app, AstPool *pool, int def_node, EvalValue *arg_vals, int arg_count, int *produced) {
    AstNode *def = &pool->nodes[def_node];
    if (!eval_push_scope_for_call(app, app_scope_stack(app), def->text, def->param_count, def->param_names, arg_vals, arg_count)) {
        *produced = 0;
        return num_val(0);
    }

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
// chain just runs out of hops rather than looping forever. Exposed
// (not static) so vm.c's own exec_send can do this same live,
// pool-independent prototype-chain lookup directly, then resolve the
// resulting procedure NAME against chunk->procs[] itself (self-
// contained -- see docs/BYTECODE_VM_DESIGN.md's "Self-contained
// BytecodeChunk" entry) instead of going through eval_resolve_method
// below, which still resolves a name to an AstPool node specifically.
PlistEntry *eval_resolve_message(LogoApp *app, const char *objname, const char *message) {
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
int eval_resolve_method(LogoApp *app, AstPool *pool, const char *objname, const char *message) {
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

void eval_fd_value(LogoApp *app, EvalValue dist_val) {
    move_turtle_forward(app, eval_to_number(dist_val));
}
static void do_fd(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_fd_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_bk_value(LogoApp *app, EvalValue dist_val) {
    move_turtle_forward(app, -eval_to_number(dist_val));
}
static void do_bk(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_bk_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_rt_value(LogoApp *app, EvalValue angle_val) {
    current_turtle(app)->angle += eval_to_number(angle_val);
}
static void do_rt(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_rt_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_lt_value(LogoApp *app, EvalValue angle_val) {
    current_turtle(app)->angle -= eval_to_number(angle_val);
}
static void do_lt(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_lt_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_setxy_value(LogoApp *app, EvalValue x_val, EvalValue y_val) {
    move_turtle_to(app, eval_to_number(x_val), eval_to_number(y_val));
}
static void do_setxy(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setxy_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
void eval_setheading_value(LogoApp *app, EvalValue angle_val) {
    current_turtle(app)->angle = eval_to_number(angle_val);
}
static void do_setheading(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setheading_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_setx_value(LogoApp *app, EvalValue x_val) {
    move_turtle_to(app, eval_to_number(x_val), current_turtle(app)->y);
}
static void do_setx(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setx_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_sety_value(LogoApp *app, EvalValue y_val) {
    move_turtle_to(app, current_turtle(app)->x, eval_to_number(y_val));
}
static void do_sety(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_sety_value(app, eval_expr(app, pool, arg_idx[0]));
}
// do_getx/do_gety/do_heading/do_pos/do_canvassize below (and every
// other zero-argument turtle/drawing do_* in this file -- do_who,
// do_penup/do_pendown/do_home/do_clear/do_clean/do_hideturtle/
// do_showturtle/do_wrap/do_fence/do_window/do_fill) take only `app`,
// no AST argument at all -- there's no separate "wrapper vs. value-
// taking core" split to make for these the way FD/BUTFIRST/etc. above
// needed, since there's no eval_expr call to factor out. Each is
// already exactly the function vm.c's OP_CALL_BUILTIN dispatch calls
// directly -- just exposed here (static removed, declared in eval.h),
// kept under their original do_ names rather than renamed to match
// this file's eval_X_value convention, since renaming would be pure
// churn with nothing left to distinguish from a "core."
EvalValue do_getx(LogoApp *app) {
    return num_val(current_turtle(app)->x);
}
EvalValue do_gety(LogoApp *app) {
    return num_val(current_turtle(app)->y);
}
EvalValue do_heading(LogoApp *app) {
    // The raw stored angle, same convention SETHEADING/RT/LT already
    // use -- not normalized to 0-360 (RT 720 just keeps adding).
    return num_val(current_turtle(app)->angle);
}
EvalValue do_pos(LogoApp *app) {
    return eval_list_wrap_pair(app, num_val(current_turtle(app)->x), num_val(current_turtle(app)->y));
}
EvalValue do_canvassize(LogoApp *app) {
    return eval_list_wrap_pair(app, num_val(app->canvas_width), num_val(app->canvas_height));
}
// Plain distance between two arbitrary [x y] points, not tied to the
// turtle's own position (unlike TOWARDS below) -- pass POS as one of
// the two points for "distance from here". Mirrors interpreter.c's own
// DISTANCE exactly.
EvalValue eval_distance_value(LogoApp *app, EvalValue a, EvalValue b) {
    double av[2], bv[2];
    if (!eval_list_as_two_numbers(app, a, av) || !eval_list_as_two_numbers(app, b, bv)) {
        append_output(app, "DISTANCE: expected two 2-element lists\n");
        return num_val(0);
    }
    double dx = bv[0] - av[0];
    double dy = bv[1] - av[1];
    return num_val(sqrt(dx * dx + dy * dy));
}
static EvalValue do_distance(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_distance_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// The heading (same convention as HEADING/SETHEADING/RT/LT) to face
// directly from the turtle's current position toward point -- derived
// from the same dx/dy-vs-heading formula move_turtle_forward uses, so
// SETHEADING TOWARDS point then FORWARD DISTANCE POS point actually
// walks straight to point. Unlike HEADING (a live, unbounded
// accumulator), this is a freshly computed compass bearing, normalized
// to [0, 360). Mirrors interpreter.c's own TOWARDS exactly.
EvalValue eval_towards_value(LogoApp *app, EvalValue p) {
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
static EvalValue do_towards(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_towards_value(app, eval_expr(app, pool, arg_idx[0]));
}
void do_penup(LogoApp *app) {
    current_turtle(app)->pen_down = FALSE;
}
void do_pendown(LogoApp *app) {
    current_turtle(app)->pen_down = TRUE;
}
void do_home(LogoApp *app) {
    move_turtle_to(app, home_x(app), home_y(app));
    current_turtle(app)->angle = 0;
}
// TELL n -- switches which turtle FD/RT/SETXY/etc. control, creating
// it (at the default state) the first time it's addressed. A direct
// port: app->turtles[]/turtle_count/current_turtle are already plain
// LogoApp fields (not behind any static helper), and init_turtle is
// already exposed in interpreter.h, so every existing turtle do_*
// function (do_fd, do_setxy, do_home, do_clear, ...) already operates
// on whichever turtle is current -- this is the one missing piece that
// actually lets more than turtle 0 ever become current.
void eval_tell_value(LogoApp *app, EvalValue index_val) {
    int index = (int)eval_to_number(index_val);
    if (index < 0 || index >= MAX_TURTLES) {
        char msg[64];
        snprintf(msg, sizeof(msg), "TELL: turtle index must be 0-%d\n", MAX_TURTLES - 1);
        append_output(app, msg);
        return;
    }
    if (index >= app->turtle_count) {
        for (int i = app->turtle_count; i <= index; i++) {
            init_turtle(app, &app->turtles[i]);
        }
        app->turtle_count = index + 1;
    }
    app->current_turtle = index;
}
static void do_tell(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_tell_value(app, eval_expr(app, pool, arg_idx[0]));
}
// WHO -- the current turtle's index, the one thing TELL sets but has
// no way to read back on its own. Direct port.
EvalValue do_who(LogoApp *app) {
    return num_val(app->current_turtle);
}
void do_clear(LogoApp *app) {
    app->line_count = 0;
    app->label_count = 0;
    app->raster_op_count = 0;
    for (int i = 0; i < app->turtle_count; i++) {
        app->turtles[i].x = home_x(app);
        app->turtles[i].y = home_y(app);
        app->turtles[i].angle = 0;
    }
}
// Pure clamps mirroring interpreter.c's own clamp01/clamp_range
// exactly -- stateless, no LogoApp involved, so kept as local mirrors
// here rather than exposed through interpreter.h, same reasoning as
// eval_mod_result below.
static double eval_clamp01(double v) {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}
static double eval_clamp_range(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
// ARC angle radius -- draws a circle/arc of `radius` centered ON the
// turtle, starting at its current heading and sweeping through `angle`
// degrees; the turtle itself doesn't move. Direct port, sharing the
// now-exposed record_line directly (the same helper move_turtle_to
// itself uses for every ordinary move) since ARC deliberately bypasses
// move_turtle_to's own position-tracking.
void eval_arc_value(LogoApp *app, EvalValue angle_val, EvalValue radius_val) {
    double angle_deg = eval_to_number(angle_val);
    double radius = eval_to_number(radius_val);

    double center_x = current_turtle(app)->x;
    double center_y = current_turtle(app)->y;
    double start_heading = current_turtle(app)->angle;

    int segments = (int)eval_clamp_range(fabs(angle_deg) / 5.0, 8, 360);
    double step = angle_deg / segments;

    for (int i = 0; i < segments; i++) {
        double rad0 = (start_heading + step * i - 90.0) * M_PI / 180.0;
        double rad1 = (start_heading + step * (i + 1) - 90.0) * M_PI / 180.0;
        record_line(app,
                    center_x + radius * cos(rad0), center_y + radius * sin(rad0),
                    center_x + radius * cos(rad1), center_y + radius * sin(rad1));
    }
}
static void do_arc(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_arc_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
void do_clean(LogoApp *app) {
    app->line_count = 0;
    app->label_count = 0;
    app->raster_op_count = 0;
}
void do_hideturtle(LogoApp *app) {
    current_turtle(app)->visible = FALSE;
}
void do_showturtle(LogoApp *app) {
    current_turtle(app)->visible = TRUE;
}
// WRAP/FENCE/WINDOW -- what happens when a move would cross the canvas
// edge. Trivial one-line app->edge_mode setters: the actual behavior
// lives in move_turtle_to (already shared with interpreter.c since the
// turtle-motion batch), so these ports need nothing more.
void do_wrap(LogoApp *app) {
    app->edge_mode = EDGE_WRAP;
}
void do_fence(LogoApp *app) {
    app->edge_mode = EDGE_FENCE;
}
void do_window(LogoApp *app) {
    app->edge_mode = EDGE_WINDOW;
}
void eval_setpencolor_value(LogoApp *app, EvalValue r_val, EvalValue g_val, EvalValue b_val) {
    double r = eval_to_number(r_val);
    double g = eval_to_number(g_val);
    double b = eval_to_number(b_val);
    Turtle *t = current_turtle(app);
    t->pen_r = eval_clamp01(r / 255.0);
    t->pen_g = eval_clamp01(g / 255.0);
    t->pen_b = eval_clamp01(b / 255.0);
}
static void do_setpencolor(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setpencolor_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]), eval_expr(app, pool, arg_idx[2]));
}
void eval_setpenwidth_value(LogoApp *app, EvalValue width_val) {
    double width = eval_to_number(width_val);
    current_turtle(app)->pen_width = eval_clamp_range(width, MIN_PEN_WIDTH, MAX_PEN_WIDTH);
}
static void do_setpenwidth(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setpenwidth_value(app, eval_expr(app, pool, arg_idx[0]));
}
void eval_setbackground_value(LogoApp *app, EvalValue r_val, EvalValue g_val, EvalValue b_val) {
    double r = eval_to_number(r_val);
    double g = eval_to_number(g_val);
    double b = eval_to_number(b_val);
    app->bg_r = eval_clamp01(r / 255.0);
    app->bg_g = eval_clamp01(g / 255.0);
    app->bg_b = eval_clamp01(b / 255.0);
}
static void do_setbackground(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setbackground_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]), eval_expr(app, pool, arg_idx[2]));
}
void eval_setcanvassize_value(LogoApp *app, EvalValue width_val, EvalValue height_val) {
    double width = eval_to_number(width_val);
    double height = eval_to_number(height_val);
    if (width < MIN_CANVAS_SIZE || width > MAX_CANVAS_SIZE ||
        height < MIN_CANVAS_SIZE || height > MAX_CANVAS_SIZE) {
        char msg[96];
        snprintf(msg, sizeof(msg), "SETCANVASSIZE: width and height must be %d-%d\n",
                 (int)MIN_CANVAS_SIZE, (int)MAX_CANVAS_SIZE);
        append_output(app, msg);
        return;
    }
    app->canvas_width = width;
    app->canvas_height = height;
    app->line_count = 0;
    app->label_count = 0;
    app->raster_op_count = 0;
    for (int i = 0; i < app->turtle_count; i++) {
        app->turtles[i].x = home_x(app);
        app->turtles[i].y = home_y(app);
        app->turtles[i].angle = 0;
    }
    if (app->resize_canvas != NULL) {
        app->resize_canvas(app, width, height);
    }
}
static void do_setcanvassize(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setcanvassize_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// LABEL text -- draws text at the turtle's current position, in its
// current pen color. Pure data (position, color, text) recorded here,
// same as interpreter.c's own version -- ui.c's draw_scene does the
// actual Cairo text rendering, kept out of this file entirely.
void eval_label_value(LogoApp *app, EvalValue val) {
    if (app->label_count >= MAX_LABELS) return;
    Turtle *t = current_turtle(app);
    Label *label = &app->labels[app->label_count++];
    label->x = t->x;
    label->y = t->y;
    label->r = t->pen_r;
    label->g = t->pen_g;
    label->b = t->pen_b;
    eval_value_to_text(app, val, label->text, sizeof(label->text));
}
static void do_label(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_label_value(app, eval_expr(app, pool, arg_idx[0]));
}
// FILL -- flood-fills the region containing the turtle, bounded by
// whatever lines are drawn as of this exact moment, with the turtle's
// current pen color. Same "record plain data, let ui.c do the actual
// Cairo/rasterizing work" split as LABEL; line_count_at_call freezes
// the boundary so a line drawn after this FILL can't retroactively
// change what it filled.
void do_fill(LogoApp *app) {
    if (app->raster_op_count >= MAX_RASTER_OPS) return;
    Turtle *t = current_turtle(app);
    RasterOp *op = &app->raster_ops[app->raster_op_count++];
    op->kind = RASTER_OP_FILL;
    op->x = t->x;
    op->y = t->y;
    op->r = t->pen_r;
    op->g = t->pen_g;
    op->b = t->pen_b;
    op->line_count_at_call = app->line_count;
}
// ERASERECT w h -- paints a w-by-h rectangle centered on the turtle in
// the background color, same call-time-frozen treatment as FILL.
void eval_eraserect_value(LogoApp *app, EvalValue w_val, EvalValue h_val) {
    double w = eval_to_number(w_val);
    double h = eval_to_number(h_val);
    if (app->raster_op_count >= MAX_RASTER_OPS) return;
    Turtle *t = current_turtle(app);
    RasterOp *op = &app->raster_ops[app->raster_op_count++];
    op->kind = RASTER_OP_ERASE_RECT;
    op->x = t->x;
    op->y = t->y;
    op->w = w;
    op->h = h;
    op->line_count_at_call = app->line_count;
}
static void do_eraserect(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_eraserect_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
void eval_print_value(LogoApp *app, EvalValue v) {
    char text[2048];
    eval_value_to_text(app, v, text, sizeof(text));
    char buf[2100];
    snprintf(buf, sizeof(buf), "%s\n", text);
    append_output(app, buf);
}
static void do_print(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_print_value(app, eval_expr(app, pool, arg_idx[0]));
}
static void do_make(LogoApp *app, AstPool *pool, const int *arg_idx) {
    // arg_idx[0] is an AST_WORD -- the parser's ARG_QUOTED_WORD kind
    // already guarantees this holds the variable's name directly in
    // .text (see parser.c's MAKE signature).
    const char *varname = pool->nodes[arg_idx[0]].text;
    EvalValue val = eval_expr(app, pool, arg_idx[1]);
    ScopeStack ss = app_scope_stack(app);
    if (val.type == VALUE_WORD) set_var_word(app, ss, varname, val.word);
    else if (val.type == VALUE_LIST) set_var_list(app, ss, varname, val.list_head);
    else if (val.type == VALUE_ARRAY) set_var_array(app, ss, varname, val.list_head, (int)val.number);
    else set_var(app, ss, varname, val.number);
}
// THING name -- the reflective, computed-name sibling of :name (:name
// only ever takes a literal identifier written right there in the
// source, while THING takes any expression that evaluates to a word,
// e.g. THING WORD "item :n). Mirrors interpreter.c's own THING exactly,
// sharing the same find_var. `ss` is app_scope_stack(app) when called
// from do_thing below (ast_eval), or vm_scope_stack(vm) when called
// from vm.c's call_builtin (THING) -- see ScopeStack's own comment in
// logo_types.h.
EvalValue eval_thing_value(LogoApp *app, ScopeStack ss, EvalValue name_val) {
    char name_text[512];
    eval_value_to_text(app, name_val, name_text, sizeof(name_text));
    Variable *v = find_var(app, ss, name_text);
    if (v == NULL) return num_val(0);
    if (v->type == VALUE_WORD) return word_val(v->word);
    if (v->type == VALUE_LIST) return list_val(v->list_head);
    if (v->type == VALUE_ARRAY) return array_val(v->list_head, (int)v->number);
    return num_val(v->number);
}
static EvalValue do_thing(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_thing_value(app, app_scope_stack(app), eval_expr(app, pool, arg_idx[0]));
}
// LOCAL "name -- a variable scoped to the current call, without being
// a parameter. Only valid inside an active procedure call; the scope's
// vars array shares its fixed capacity (MAX_PARAMS) with real
// parameters. Mirrors interpreter.c's own LOCAL exactly, sharing the
// same scope-stack push/pop call_ast_procedure (or, under the VM,
// exec_call_proc) already reads/writes -- see ScopeStack's own comment
// in logo_types.h for why that sharing is safe despite the VM now
// having its own separate, deeper scope array.
void eval_local_declare(LogoApp *app, ScopeStack ss, const char *varname) {
    if (*ss.scope_depth <= 0) {
        append_output(app, "LOCAL: can only be used inside a procedure\n");
        return;
    }
    Scope *scope = &ss.scopes[*ss.scope_depth - 1];
    for (int i = 0; i < scope->count; i++) {
        if (strcasecmp(scope->vars[i].name, varname) == 0) return; // already local
    }
    if (scope->count >= MAX_PARAMS) {
        append_output(app, "LOCAL: too many local variables\n");
        return;
    }
    Variable *v = &scope->vars[scope->count++];
    snprintf(v->name, sizeof(v->name), "%s", varname);
    v->type = VALUE_NUMBER;
    v->number = 0;
}
static void do_local(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_local_declare(app, app_scope_stack(app), pool->nodes[arg_idx[0]].text);
}
// NAMES -- every currently-defined global variable's name, as a list.
// Mirrors interpreter.c's own NAMES exactly, sharing the same
// app->variables/var_count globals table. Takes no AST argument at
// all, so (unlike every other do_* here) it was already a plain
// value-taking function before this batch -- just needed `static`
// removed and a name matching this batch's own eval_X_value
// convention (every do_* below still forwards to it under its own
// original name, so exec_call's own dispatch table is untouched).
EvalValue eval_names_value(LogoApp *app) {
    int head = -1;
    int *next_slot = &head;
    for (int i = 0; i < app->var_count; i++) {
        int node = value_to_node(app, word_val(app->variables[i].name));
        if (node < 0) return list_pool_exhausted(app);
        *next_slot = node;
        next_slot = &app->list_pool[node].next;
    }
    return list_val(head);
}
// PROCEDURES -- every currently-defined procedure's name, as a list.
// Unlike NAMES, this can't share interpreter.c's own app->procedures
// table -- this engine's own TO definitions are AST_PROC_DEF nodes in
// `pool`, never registered into that text-based table at all, so this
// walks the pool directly instead (the same "search every node,
// regardless of nesting" reach find_proc_def already uses). Skips a
// blank .text -- ERASE's own way of "deleting" a procedure here (see
// do_erase below), since there's no app->procedures-style array to
// physically shift entries out of the way in.
EvalValue do_procedures(LogoApp *app, AstPool *pool) {
    int head = -1;
    int *next_slot = &head;
    for (int i = 0; i < pool->node_count; i++) {
        if (pool->nodes[i].type == AST_PROC_DEF && pool->nodes[i].text[0] != '\0') {
            int node = value_to_node(app, word_val(pool->nodes[i].text));
            if (node < 0) return list_pool_exhausted(app);
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    return list_val(head);
}
// ERASE "name -- deletes a procedure. interpreter.c physically removes
// it from app->procedures[] (shifting later entries down); this engine
// has no such array to shift -- TO definitions are AST_PROC_DEF nodes
// living in `pool`, found by name via find_proc_def's own linear scan
// -- so "deleting" one just means making it permanently unmatchable:
// blanking its own .text to the empty string, which can never equal a
// real call's name (an AST_CALL node's own .text is always non-empty).
// Both find_proc_def (so a later call correctly reports "I don't know
// how to X") and do_procedures above (skipping blank-text entries) rely
// on this.
// Takes `pool` (not just `app`, unlike every other function this batch
// exposes) since it directly mutates an AST_PROC_DEF node's own text --
// deletion here means "blank the name so no call/PROCEDURES listing
// can ever match it again," not removing anything from a separate
// procedure table the way interpreter.c's own ERASE does (this engine
// has none). Exposed for vm.c's own OP_ERASE (a special form, like
// MAKE/LOCAL -- ERASE's argument is a raw procedure name, ARG_QUOTED_WORD,
// never an evaluated expression, so it can't go through the ordinary
// OP_CALL_BUILTIN path the same way GETPROP/SETPROP etc. do).
void eval_erase_declare(LogoApp *app, AstPool *pool, const char *name) {
    int def_node = find_proc_def(pool, name);
    if (def_node < 0) {
        append_output(app, "ERASE: no such procedure \"");
        append_output(app, name);
        append_output(app, "\n");
        return;
    }
    pool->nodes[def_node].text[0] = '\0';
}
static void do_erase(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_erase_declare(app, pool, pool->nodes[arg_idx[0]].text);
}
// TEXT "name -- the read-as-data complement to SHOW: instead of
// printing a procedure's own TO...END definition, outputs its raw
// body text tokenized into a flat list of words. Reuses
// eval_list_tokenize_words directly against the AST_PROC_DEF's own
// body_text/body_len (see ast.h and parse_proc_def in parser.c) --
// this engine's TO definitions never had their original source text
// retained anywhere before this, which is exactly what blocked TEXT
// (and SAVE, see do_save below) until now.
EvalValue eval_text_value(LogoApp *app, AstPool *pool, EvalValue name_val) {
    const char *name = name_val.word;
    int def_node = find_proc_def(pool, name);
    if (def_node < 0) {
        append_output(app, "TEXT: no such procedure \"");
        append_output(app, name);
        append_output(app, "\n");
        return list_val(-1);
    }
    AstNode *def = &pool->nodes[def_node];
    int head;
    const char *body = def->body_text != NULL ? def->body_text : "";
    if (!eval_list_tokenize_words(app, body, (size_t)def->body_len, &head)) return list_pool_exhausted(app);
    return list_val(head);
}
static EvalValue do_text(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_text_value(app, pool, word_val(pool->nodes[arg_idx[0]].text));
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
    for (int i = 0; i < count && !app->stop_requested && !app->throw_requested; i++) {
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
        if (app->stop_requested || app->throw_requested) break;
        iterations++;
    }
}
// FOREVER [block] -- an infinite loop, only escaped via STOP/OUTPUT/
// THROW inside the block, capped by the same iteration ceiling as
// WHILE so a script that forgets its own STOP doesn't hang. Mirrors
// interpreter.c's own FOREVER exactly.
static void do_forever(LogoApp *app, AstPool *pool, const int *arg_idx) {
    int block_node = arg_idx[0];
    int iterations = 0;
    for (;;) {
        if (iterations >= MAX_WHILE_ITERATIONS) {
            append_output(app, "FOREVER: stopped after too many iterations\n");
            break;
        }
        exec_block(app, pool, block_node);
        if (app->stop_requested || app->throw_requested) break;
        iterations++;
    }
}
// Value-taking cores (see eval.h's own note): pure EvalValue->EvalValue
// functions needing no app/pool at all, same shape eval_int_value just
// below already established -- exposed for vm.c's call_builtin, found
// missing there by the same audit that found TEXT/SHOW/file-I/O
// missing (2026-08-10, see docs/ROADMAP.md's own note on the full
// 35-name list this surfaced).
EvalValue eval_abs_value(EvalValue v) {
    return num_val(fabs(eval_to_number(v)));
}
static EvalValue do_abs(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_abs_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_sqrt_value(EvalValue v) {
    return num_val(sqrt(eval_to_number(v)));
}
static EvalValue do_sqrt(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_sqrt_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_power_value(EvalValue base_val, EvalValue exponent_val) {
    return num_val(pow(eval_to_number(base_val), eval_to_number(exponent_val)));
}
static EvalValue do_power(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue base_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue exponent_val = eval_expr(app, pool, arg_idx[1]);
    return eval_power_value(base_val, exponent_val);
}
EvalValue eval_random_value(EvalValue v) {
    return num_val(random_below(eval_to_number(v)));
}
static EvalValue do_random(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_random_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_round_value(EvalValue v) {
    return num_val(round(eval_to_number(v)));
}
static EvalValue do_round(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_round_value(eval_expr(app, pool, arg_idx[0]));
}
// Exposed (unlike its sibling math operators, still eval.c-private --
// see docs/BYTECODE_VM_DESIGN.md's own note on this being a genuinely
// small, deliberate addition, not "port all math operators") because
// compiler.c's own REPEAT compilation needs this exact truncation:
// REPEAT's own count is truncated once, up front, exactly like
// do_repeat's own `(int)eval_to_number(...)` -- without it, a
// fractional count (REPEAT 3.5 [...]) would run one extra iteration
// under a naive decrement-until-positive loop.
EvalValue eval_int_value(EvalValue v) {
    return num_val(trunc(eval_to_number(v)));
}
static EvalValue do_int(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_int_value(eval_expr(app, pool, arg_idx[0]));
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
EvalValue eval_mod_value(EvalValue a_val, EvalValue b_val) {
    return num_val(eval_mod_result(eval_to_number(a_val), eval_to_number(b_val)));
}
static EvalValue do_mod(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue a_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue b_val = eval_expr(app, pool, arg_idx[1]);
    return eval_mod_value(a_val, b_val);
}
// SIN/COS/TAN take degrees (converted to radians); ASIN/ACOS/ARCTAN
// return degrees (converted back from radians) -- matches
// interpreter.c's own convention throughout (SETHEADING/turtle angles
// are degrees), not the C math library's native radians.
EvalValue eval_sin_value(EvalValue v) {
    return num_val(sin(eval_to_number(v) * M_PI / 180.0));
}
static EvalValue do_sin(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_sin_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_cos_value(EvalValue v) {
    return num_val(cos(eval_to_number(v) * M_PI / 180.0));
}
static EvalValue do_cos(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_cos_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_tan_value(EvalValue v) {
    return num_val(tan(eval_to_number(v) * M_PI / 180.0));
}
static EvalValue do_tan(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_tan_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_asin_value(EvalValue v) {
    return num_val(asin(eval_to_number(v)) * 180.0 / M_PI);
}
static EvalValue do_asin(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_asin_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_acos_value(EvalValue v) {
    return num_val(acos(eval_to_number(v)) * 180.0 / M_PI);
}
static EvalValue do_acos(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_acos_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_arctan_value(EvalValue v) {
    return num_val(atan(eval_to_number(v)) * 180.0 / M_PI);
}
static EvalValue do_arctan(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_arctan_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_ln_value(EvalValue v) {
    return num_val(log(eval_to_number(v)));
}
static EvalValue do_ln(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_ln_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_log_value(EvalValue v) {
    return num_val(log10(eval_to_number(v)));
}
static EvalValue do_log(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_log_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_exp_value(EvalValue v) {
    return num_val(exp(eval_to_number(v)));
}
static EvalValue do_exp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_exp_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_first_value(LogoApp *app, EvalValue arg) {
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
static EvalValue do_first(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_first_value(app, eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_butfirst_value(LogoApp *app, EvalValue arg) {
    if (arg.type == VALUE_LIST) return list_val(arg.list_head < 0 ? -1 : app->list_pool[arg.list_head].next);
    if (arg.type == VALUE_WORD) return word_val(arg.word[0] == '\0' ? "" : arg.word + 1);
    return list_val(-1); // BUTFIRST of a bare number is empty
}
static EvalValue do_butfirst(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_butfirst_value(app, eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_last_value(LogoApp *app, EvalValue arg) {
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
static EvalValue do_last(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_last_value(app, eval_expr(app, pool, arg_idx[0]));
}
// BUTLAST arg -- unlike FIRST/LAST/BUTFIRST above, the LIST case is
// substantial enough (real list-splicing, not a one-liner) to already
// live in its own function, eval_list_butlast, shared with
// interpreter.c's own analogous helper in spirit (see that function's
// own comment) -- this wrapper just adds the WORD/bare-number cases
// eval_list_butlast itself doesn't handle, mirroring do_first/
// do_last's own type-dispatch shape.
EvalValue eval_butlast_value(LogoApp *app, EvalValue arg) {
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
static EvalValue do_butlast(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_butlast_value(app, eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_count_value(LogoApp *app, EvalValue arg) {
    if (arg.type == VALUE_LIST) {
        double count = 0;
        for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
        return num_val(count);
    }
    if (arg.type == VALUE_WORD) return num_val((double)strlen(arg.word));
    if (arg.type == VALUE_ARRAY) return num_val(arg.number); // an array's `number` field holds its length
    return num_val(1); // a bare number counts as a one-element list
}
static EvalValue do_count(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_count_value(app, eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_empty_value(EvalValue arg) {
    int empty;
    if (arg.type == VALUE_LIST) empty = (arg.list_head == -1);
    else if (arg.type == VALUE_WORD) empty = (arg.word[0] == '\0');
    else empty = 0; // a number is never "empty"
    return word_val(empty ? "TRUE" : "FALSE");
}
static EvalValue do_empty(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_empty_value(eval_expr(app, pool, arg_idx[0]));
}
// Type predicates -- mirror interpreter.c's own WORD?/LIST?/NUMBER?/
// ARRAY? exactly: each just checks the evaluated argument's own
// ValueType tag (shared verbatim between the two engines, see
// logo_types.h), no coercion.
EvalValue eval_wordp_value(EvalValue arg) {
    return word_val(arg.type == VALUE_WORD ? "TRUE" : "FALSE");
}
static EvalValue do_wordp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_wordp_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_listp_value(EvalValue arg) {
    return word_val(arg.type == VALUE_LIST ? "TRUE" : "FALSE");
}
static EvalValue do_listp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_listp_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_numberp_value(EvalValue arg) {
    return word_val(arg.type == VALUE_NUMBER ? "TRUE" : "FALSE");
}
static EvalValue do_numberp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_numberp_value(eval_expr(app, pool, arg_idx[0]));
}
EvalValue eval_arrayp_value(EvalValue arg) {
    return word_val(arg.type == VALUE_ARRAY ? "TRUE" : "FALSE");
}
static EvalValue do_arrayp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_arrayp_value(eval_expr(app, pool, arg_idx[0]));
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
// PICK thing -- a random element of a list, a random character of a
// word, a random cell of an array, or the bare value itself for a
// number (a bare number counts as a one-element list). Mirrors
// interpreter.c's own PICK exactly, sharing the same random_below.
EvalValue eval_pick_value(LogoApp *app, EvalValue arg) {
    if (arg.type == VALUE_LIST) {
        int count = 0;
        for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
        if (count == 0) {
            append_output(app, "PICK: empty list\n");
            return word_val("");
        }
        int target = (int)random_below(count);
        int i = 0;
        for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next, i++) {
            if (i == target) return node_to_value(&app->list_pool[idx]);
        }
    }
    if (arg.type == VALUE_WORD) {
        size_t len = strlen(arg.word);
        if (len == 0) {
            append_output(app, "PICK: empty word\n");
            return word_val("");
        }
        char ch[2] = {arg.word[(int)random_below((double)len)], '\0'};
        return word_val(ch);
    }
    if (arg.type == VALUE_ARRAY) {
        int idx = (int)random_below(arg.number);
        return node_to_value(&app->list_pool[arg.list_head + idx]);
    }
    return arg; // a bare number counts as a one-element list
}
static EvalValue do_pick(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_pick_value(app, eval_expr(app, pool, arg_idx[0]));
}
// FLATTEN list -- collects every leaf reachable from list's chain,
// discarding sublist structure. Wraps a non-list argument as a
// one-element list first, same convention as MAP/FOREACH's own
// non-list handling.
EvalValue eval_flatten_value(LogoApp *app, EvalValue arg) {
    int input_head = (arg.type == VALUE_LIST) ? arg.list_head : value_to_node(app, arg);
    if (arg.type != VALUE_LIST && input_head < 0) return list_pool_exhausted(app);
    int head = -1;
    int *next_slot = &head;
    if (!eval_list_flatten_into(app, input_head, &next_slot)) return list_pool_exhausted(app);
    return list_val(head);
}
static EvalValue do_flatten(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_flatten_value(app, eval_expr(app, pool, arg_idx[0]));
}
// PARSE thing -- tokenizes any value's printed text (same rendering
// PRINT uses) by whitespace into a list of words, the reverse of what
// PRINT/eval_value_to_text already does for a plain word.
EvalValue eval_parse_value(LogoApp *app, EvalValue arg) {
    char text[512];
    eval_value_to_text(app, arg, text, sizeof(text));
    int head;
    if (!eval_list_tokenize_words(app, text, strlen(text), &head)) return list_pool_exhausted(app);
    return list_val(head);
}
static EvalValue do_parse(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_parse_value(app, eval_expr(app, pool, arg_idx[0]));
}
// SUBST old new thing -- a non-list thing just compares directly
// (old->new if it matches, thing unchanged otherwise); a list rebuilds
// its whole chain via eval_list_subst_into.
EvalValue eval_subst_value(LogoApp *app, EvalValue old_val, EvalValue new_val, EvalValue thing) {
    if (thing.type != VALUE_LIST) {
        return eval_values_equal(app, thing, old_val) ? new_val : thing;
    }
    int head;
    if (!eval_list_subst_into(app, thing.list_head, old_val, new_val, &head)) return list_pool_exhausted(app);
    return list_val(head);
}
static EvalValue do_subst(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue old_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue new_val = eval_expr(app, pool, arg_idx[1]);
    EvalValue thing = eval_expr(app, pool, arg_idx[2]);
    return eval_subst_value(app, old_val, new_val, thing);
}
// DOT a b -- the dot product of two same-length numeric lists.
EvalValue eval_dot_value(LogoApp *app, EvalValue a, EvalValue b) {
    if (a.type != VALUE_LIST || b.type != VALUE_LIST) {
        append_output(app, "DOT: expected two lists\n");
        return num_val(0);
    }
    double sum = 0;
    int idx_a = a.list_head, idx_b = b.list_head;
    while (idx_a != -1 && idx_b != -1) {
        sum += eval_to_number(node_to_value(&app->list_pool[idx_a]))
             * eval_to_number(node_to_value(&app->list_pool[idx_b]));
        idx_a = app->list_pool[idx_a].next;
        idx_b = app->list_pool[idx_b].next;
    }
    if (idx_a != -1 || idx_b != -1) {
        append_output(app, "DOT: lists must be the same length\n");
        return num_val(0);
    }
    return num_val(sum);
}
static EvalValue do_dot(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue a = eval_expr(app, pool, arg_idx[0]);
    EvalValue b = eval_expr(app, pool, arg_idx[1]);
    return eval_dot_value(app, a, b);
}
// CROSS a b -- the 3D cross product of two 3-element numeric lists.
EvalValue eval_cross_value(LogoApp *app, EvalValue a, EvalValue b) {
    double av[3], bv[3];
    if (!eval_list_as_three_numbers(app, a, av) || !eval_list_as_three_numbers(app, b, bv)) {
        append_output(app, "CROSS: expected two 3-element lists\n");
        return list_val(-1);
    }
    double cx = av[1] * bv[2] - av[2] * bv[1];
    double cy = av[2] * bv[0] - av[0] * bv[2];
    double cz = av[0] * bv[1] - av[1] * bv[0];
    int n0 = value_to_node(app, num_val(cx));
    int n1 = value_to_node(app, num_val(cy));
    int n2 = value_to_node(app, num_val(cz));
    if (n0 < 0 || n1 < 0 || n2 < 0) return list_pool_exhausted(app);
    app->list_pool[n0].next = n1;
    app->list_pool[n1].next = n2;
    return list_val(n0);
}
static EvalValue do_cross(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue a = eval_expr(app, pool, arg_idx[0]);
    EvalValue b = eval_expr(app, pool, arg_idx[1]);
    return eval_cross_value(app, a, b);
}
// MEMBER? thing container -- a list checks element membership
// (eval_values_equal per element), a word checks substring containment
// (its "elements" are characters), anything else (a number, or an
// array -- interpreter.c has no special ARRAY case here either) falls
// back to treating `container` as a single one-element value. Mirrors
// interpreter.c's own MEMBER? exactly, including that fallback.
EvalValue eval_memberp_value(LogoApp *app, EvalValue thing, EvalValue container) {
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
static EvalValue do_memberp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue thing = eval_expr(app, pool, arg_idx[0]);
    EvalValue container = eval_expr(app, pool, arg_idx[1]);
    return eval_memberp_value(app, thing, container);
}
// ARRAY size -- allocates `size` contiguous list_pool cells (direct
// index math, not chain-walked -- see array_val's own comment), each
// starting out an empty list. Mirrors interpreter.c's own ARRAY exactly.
EvalValue eval_array_value(LogoApp *app, EvalValue size_val) {
    int size = (int)eval_to_number(size_val);
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
static EvalValue do_array(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_array_value(app, eval_expr(app, pool, arg_idx[0]));
}
// ITEM index thing -- 1-indexed lookup into a list (chain walk), word
// (character extract), array (direct index math), or bare number
// (only index 1 valid). Mirrors interpreter.c's own ITEM exactly.
EvalValue eval_item_value(LogoApp *app, EvalValue index_val, EvalValue thing) {
    int index = (int)eval_to_number(index_val);
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
static EvalValue do_item(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_item_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
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
void eval_setitem_value(LogoApp *app, EvalValue index_val, EvalValue array_val_, EvalValue new_val) {
    int index = (int)eval_to_number(index_val);
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
static void do_setitem(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_setitem_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]), eval_expr(app, pool, arg_idx[2]));
}
// FILLARRAY array value -- SETITEM's per-cell assignment looped over
// every index instead of just one. Mirrors interpreter.c's own
// FILLARRAY exactly.
void eval_fillarray_value(LogoApp *app, EvalValue array_val_, EvalValue new_val) {
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
static void do_fillarray(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_fillarray_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// MAP template list -- applies `template` (see the file comment near
// eval_apply_template_expr) to each element of `list`, collecting the
// results into a new list. Mirrors interpreter.c's own MAP exactly,
// including its handling of a non-list `list` argument (wrapped as a
// one-element list via value_to_node first, same as
// list_node_from_value there).
// The dynamic (runtime-computed-template) path -- used directly by
// do_map, and by vm.c's own call_builtin "MAP" branch when
// compiler.c's own compile_template_call couldn't see the template at
// compile time (e.g. `MAP :tmpl [1 2 3]`). vm.c's own compiled-once
// fast path (see docs/BYTECODE_VM_DESIGN.md's Progress log) is a
// completely separate mechanism, not built on this function at all.
EvalValue eval_map_value(LogoApp *app, EvalValue template_val, EvalValue list_arg) {
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
            parse_result_destroy(scratch);
            return list_pool_exhausted(app);
        }
        *next_slot = node;
        next_slot = &app->list_pool[node].next;
    }
    parse_result_destroy(scratch);
    return list_val(new_head);
}
static EvalValue do_map(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_map_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// FILTER template list -- keeps each element of `list` whose template
// (evaluated as a *condition*, e.g. `[? > 2]`) is truthy, in a new
// list; a kept element is copied as-is (list_node_copy), not replaced
// by the template's own boolean result. Mirrors interpreter.c's own
// FILTER exactly.
EvalValue eval_filter_value(LogoApp *app, EvalValue template_val, EvalValue list_arg) {
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
                parse_result_destroy(scratch);
                return list_pool_exhausted(app);
            }
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    parse_result_destroy(scratch);
    return list_val(new_head);
}
static EvalValue do_filter(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_filter_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// REDUCE template list -- folds left-to-right, seeding the accumulator
// with the list's own first element (no separate start-value
// argument); the template uses ?1 for the accumulator so far and ?2
// for the current element (REDUCE [?1 + ?2] [1 2 3 4] sums to 10).
// Inlines its own two-placeholder substitution rather than reusing
// eval_apply_template_expr (which only knows "?"), same as
// interpreter.c's own REDUCE does relative to apply_template_expr.
EvalValue eval_reduce_value(LogoApp *app, EvalValue template_val, EvalValue list_arg) {
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
    parse_result_destroy(scratch);
    return acc;
}
static EvalValue do_reduce(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_reduce_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
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
// file for a bad template. OUTPUT/STOP/THROW inside the template ends
// the loop early via app->stop_requested/throw_requested, the same
// flags REPEAT/WHILE/FOREVER check (no interrupt equivalent exists in
// this engine, unlike interpreter.c's own three-way condition here).
void eval_foreach_value(LogoApp *app, EvalValue template_val, EvalValue list_arg) {
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
        if (app->stop_requested || app->throw_requested) break;
    }
    parse_result_destroy(scratch);
}
static void do_foreach(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_foreach_value(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
}
// RUN thing -- executes a stored word/list as Logo source, exactly as
// if it had been typed directly (RUN'd code shares the caller's scope,
// no push_scope, matching interpreter.c). Reuses FOREACH's own
// re-entrant lex/parse-into-a-scratch-ParseResult machinery, since a
// RUN'd thing is likewise a whole statement sequence, not a single
// expression/condition. Capped by run_depth (a much lower ceiling than
// ordinary recursion, since RUN is considerably more expensive per
// level) rather than scope_depth, guarding against a self-referential
// RUN (MAKE "x [RUN :x] / RUN :x) blowing the C call stack. RUN never
// hands back a value (confirmed in docs/LANGUAGE.md), so this is void
// -- statement position only, matching its BUILTIN_SIGNATURES entry.
static void do_run(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue val = eval_expr(app, pool, arg_idx[0]);
    if (app->run_depth >= MAX_RUN_DEPTH) {
        append_output(app, "RUN: too deeply nested, ignored\n");
        return;
    }
    char code_text[512];
    eval_value_to_text(app, val, code_text, sizeof(code_text));

    app->run_depth++;
    LogoToken tokens[MAX_TEMPLATE_TOKENS];
    int n = logo_lex(code_text, tokens, MAX_TEMPLATE_TOKENS);
    if (n >= 0) {
        ParseResult *scratch = calloc(1, sizeof(ParseResult));
        logo_parse(tokens, n, scratch);
        if (scratch->error_count == 0) exec_block(app, &scratch->pool, scratch->program);
        parse_result_destroy(scratch);
    }
    app->run_depth--;
}
// APPLY "name arglist -- calls a procedure with arguments taken from a
// list, instead of parsed positionally from the command line. A
// non-list argument counts as a one-element list, the same convention
// SEND's own arglist already uses. Unlike interpreter.c's own APPLY
// (which looks up a text-based Procedure via find_procedure), this
// engine's procedures are AST_PROC_DEF nodes, so it resolves through
// find_proc_def/call_ast_procedure instead -- the same pair
// do_user_procedure_call and do_send already use. Never hands back a
// value (confirmed in docs/LANGUAGE.md, matching RUN), so void.
static void do_apply(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue name_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue list_val = eval_expr(app, pool, arg_idx[1]);
    char name_text[64];
    eval_value_to_text(app, name_val, name_text, sizeof(name_text));

    int def_node = find_proc_def(pool, name_text);
    if (def_node < 0) {
        append_output(app, "APPLY: no such procedure \"");
        append_output(app, name_text);
        append_output(app, "\n");
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

    AstNode *def = &pool->nodes[def_node];
    if (n != def->param_count) {
        append_output(app, "APPLY: wrong number of inputs for procedure \"");
        append_output(app, name_text);
        append_output(app, "\n");
        return;
    }

    int did_output;
    call_ast_procedure(app, pool, def_node, arg_vals, n, &did_output);
}
// THROW "tag -- sets throw_requested/throw_tag, exactly mirroring
// interpreter.c's own THROW. Unwinding itself (breaking out of nested
// exec_block/do_repeat/do_while/do_forever/do_foreach calls) is
// handled at each of those sites, not here.
void eval_throw_value(LogoApp *app, EvalValue tag_val) {
    eval_value_to_text(app, tag_val, app->throw_tag, sizeof(app->throw_tag));
    app->throw_requested = TRUE;
}
static void do_throw(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_throw_value(app, eval_expr(app, pool, arg_idx[0]));
}
// CATCH "tag [block] -- runs block, then clears throw_requested only if
// it's set AND its tag matches this CATCH's own tag; a non-matching
// throw is deliberately left set so it keeps propagating toward
// whichever ancestor CATCH (if any) does match, exactly mirroring
// interpreter.c's own CATCH.
void eval_catch_check(LogoApp *app, const char *tag_text) {
    if (app->throw_requested && strcasecmp(app->throw_tag, tag_text) == 0) {
        app->throw_requested = FALSE;
    }
}
// The top level's own uncaught-THROW recovery message, shared between
// ast_eval_from (below) and vm.c's own OP_CHECK_UNCAUGHT_THROW -- same
// wording either way, one place to keep it instead of two.
void eval_report_uncaught_throw(LogoApp *app) {
    append_output(app, "THROW: no CATCH found for \"");
    append_output(app, app->throw_tag);
    append_output(app, "\n");
    app->throw_requested = FALSE;
}
// Removes a global variable by name -- swap-with-last, same pattern as
// REMOVEPROP's own property removal. Only ever searches app->variables
// (globals), never a scope -- matches its one caller's own use (vm.c's
// MAP/FILTER/REDUCE/FOREACH compiled-template loops cleaning up their
// own internal placeholder variable, which is always global).
void eval_delete_var(LogoApp *app, const char *name) {
    for (int i = 0; i < app->var_count; i++) {
        if (strcasecmp(app->variables[i].name, name) == 0) {
            app->variables[i] = app->variables[--app->var_count];
            return;
        }
    }
}
static void do_catch(LogoApp *app, AstPool *pool, const int *arg_idx) {
    EvalValue tag_val = eval_expr(app, pool, arg_idx[0]);
    char tag_text[64];
    eval_value_to_text(app, tag_val, tag_text, sizeof(tag_text));
    int block_node = arg_idx[1];

    exec_block(app, pool, block_node);
    eval_catch_check(app, tag_text);
}
// General file I/O -- OPENREAD/OPENWRITE/OPENAPPEND/READLINE/EOF?/
// DIRECTORY (operators) and CLOSE/FILEPRINT/DELETEFILE/LOAD
// (statements). Direct ports of interpreter.c's own versions, sharing
// app->file_channels[] directly (a plain LogoApp field, like
// turtles[]/turtle_count) plus the now-exposed find_free_file_channel
// -- the one bit of file-channel logic worth sharing rather than
// re-deriving, same reasoning as find_var/find_plist_entry. Whole-file,
// not real streaming I/O, matching FileChannel's own design (see
// logo_types.h): a read channel loads everything up front and serves
// READLINE out of read_buffer/read_pos; a write channel only actually
// touches disk when CLOSE flushes its write_buffer.
// Value-taking cores (see eval.h's own note on this pattern): exposed
// so vm.c's call_builtin can share them directly rather than
// reimplementing -- these 12 file-I/O/TEXT/SHOW/SAVE builtins were
// declared in parser.c's own BUILTIN_SIGNATURES from Stage 1 but never
// wired into the bytecode VM at all until this refactor (confirmed by
// grep: zero hits in vm.c before this), a real, previously-unnoticed
// gap found while scoping ANIMATESPRITE/sprites -- they parsed fine
// but silently no-op'd through the VM. LOAD is deliberately NOT among
// them: its own do_load below runs a loaded file's top-level
// statements via exec_block (this tree-walker), which the VM has no
// equivalent hook for without its own dedicated opcode -- a separate,
// bigger piece, not a value-taking-core refactor.
EvalValue eval_openread_value(LogoApp *app, EvalValue path_val) {
    char path_text[512];
    eval_value_to_text(app, path_val, path_text, sizeof(path_text));
    int idx = find_free_file_channel(app);
    if (idx < 0) return num_val(-1);
    char *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(path_text, &contents, NULL, &error)) {
        g_error_free(error);
        return num_val(-1);
    }
    FileChannel *fc = &app->file_channels[idx];
    fc->mode = FILE_CHANNEL_READ;
    fc->read_buffer = contents;
    fc->read_pos = 0;
    return num_val(idx);
}
static EvalValue do_openread(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_openread_value(app, eval_expr(app, pool, arg_idx[0]));
}

EvalValue eval_openwrite_value(LogoApp *app, EvalValue path_val) {
    char path_text[512];
    eval_value_to_text(app, path_val, path_text, sizeof(path_text));
    int idx = find_free_file_channel(app);
    if (idx < 0) return num_val(-1);
    FileChannel *fc = &app->file_channels[idx];
    fc->mode = FILE_CHANNEL_WRITE;
    fc->write_buffer = g_string_new(NULL);
    snprintf(fc->path, sizeof(fc->path), "%s", path_text);
    return num_val(idx);
}
static EvalValue do_openwrite(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_openwrite_value(app, eval_expr(app, pool, arg_idx[0]));
}

EvalValue eval_openappend_value(LogoApp *app, EvalValue path_val) {
    char path_text[512];
    eval_value_to_text(app, path_val, path_text, sizeof(path_text));
    int idx = find_free_file_channel(app);
    if (idx < 0) return num_val(-1);
    FileChannel *fc = &app->file_channels[idx];
    fc->mode = FILE_CHANNEL_WRITE;
    char *existing = NULL;
    if (g_file_get_contents(path_text, &existing, NULL, NULL)) {
        fc->write_buffer = g_string_new(existing);
        g_free(existing);
    } else {
        fc->write_buffer = g_string_new(NULL);
    }
    snprintf(fc->path, sizeof(fc->path), "%s", path_text);
    return num_val(idx);
}
static EvalValue do_openappend(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_openappend_value(app, eval_expr(app, pool, arg_idx[0]));
}

EvalValue eval_readline_value(LogoApp *app, EvalValue idx_val) {
    int idx = (int)eval_to_number(idx_val);
    if (idx < 0 || idx >= MAX_OPEN_FILES || app->file_channels[idx].mode != FILE_CHANNEL_READ) {
        return word_val("");
    }
    FileChannel *fc = &app->file_channels[idx];
    size_t len = strlen(fc->read_buffer);
    if (fc->read_pos >= len) return word_val("");
    size_t start = fc->read_pos;
    const char *newline = strchr(fc->read_buffer + start, '\n');
    size_t end = newline != NULL ? (size_t)(newline - fc->read_buffer) : len;
    char line[512];
    size_t line_len = end - start;
    if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
    memcpy(line, fc->read_buffer + start, line_len);
    line[line_len] = '\0';
    if (line_len > 0 && line[line_len - 1] == '\r') line[line_len - 1] = '\0'; // CRLF files
    fc->read_pos = newline != NULL ? end + 1 : len;
    return word_val(line);
}
static EvalValue do_readline(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_readline_value(app, eval_expr(app, pool, arg_idx[0]));
}

EvalValue eval_eofp_value(LogoApp *app, EvalValue idx_val) {
    int idx = (int)eval_to_number(idx_val);
    if (idx < 0 || idx >= MAX_OPEN_FILES || app->file_channels[idx].mode != FILE_CHANNEL_READ) {
        return word_val("TRUE");
    }
    FileChannel *fc = &app->file_channels[idx];
    return word_val(fc->read_pos >= strlen(fc->read_buffer) ? "TRUE" : "FALSE");
}
static EvalValue do_eofp(LogoApp *app, AstPool *pool, const int *arg_idx) {
    return eval_eofp_value(app, eval_expr(app, pool, arg_idx[0]));
}

EvalValue eval_directory_value(LogoApp *app) {
    int head = -1;
    int *next_slot = &head;
    GDir *dir = g_dir_open(".", 0, NULL);
    if (dir != NULL) {
        const char *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            int node = value_to_node(app, word_val(name));
            if (node < 0) { g_dir_close(dir); return list_pool_exhausted(app); }
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
        g_dir_close(dir);
    }
    return list_val(head);
}
static EvalValue do_directory(LogoApp *app) {
    return eval_directory_value(app);
}

void eval_close_value(LogoApp *app, EvalValue idx_val) {
    int idx = (int)eval_to_number(idx_val);
    if (idx < 0 || idx >= MAX_OPEN_FILES || app->file_channels[idx].mode == FILE_CHANNEL_CLOSED) {
        append_output(app, "CLOSE: no such open channel\n");
        return;
    }
    FileChannel *fc = &app->file_channels[idx];
    if (fc->mode == FILE_CHANNEL_WRITE) {
        GError *error = NULL;
        if (!g_file_set_contents(fc->path, fc->write_buffer->str, (gssize)fc->write_buffer->len, &error)) {
            append_output(app, "CLOSE: could not write file\n");
            g_error_free(error);
        }
        g_string_free(fc->write_buffer, TRUE);
        fc->write_buffer = NULL;
    } else {
        g_free(fc->read_buffer);
        fc->read_buffer = NULL;
    }
    fc->mode = FILE_CHANNEL_CLOSED;
}
static void do_close(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_close_value(app, eval_expr(app, pool, arg_idx[0]));
}

void eval_fileprint_value(LogoApp *app, EvalValue idx_val, EvalValue text_val) {
    int idx = (int)eval_to_number(idx_val);
    if (idx < 0 || idx >= MAX_OPEN_FILES || app->file_channels[idx].mode != FILE_CHANNEL_WRITE) {
        append_output(app, "FILEPRINT: channel not open for writing\n");
        return;
    }
    char line[512];
    eval_value_to_text(app, text_val, line, sizeof(line));
    g_string_append(app->file_channels[idx].write_buffer, line);
    g_string_append_c(app->file_channels[idx].write_buffer, '\n');
}
static void do_fileprint(LogoApp *app, AstPool *pool, const int *arg_idx) {
    // Evaluated as two separate statements, not nested into one call,
    // to preserve the exact original left-to-right evaluation order --
    // C doesn't guarantee an order between two eval_expr calls made as
    // sibling arguments to the same outer call.
    EvalValue idx_val = eval_expr(app, pool, arg_idx[0]);
    EvalValue text_val = eval_expr(app, pool, arg_idx[1]);
    eval_fileprint_value(app, idx_val, text_val);
}
// DELETEFILE/LOAD's own path argument is ARG_QUOTED_WORD, not ARG_EXPR
// -- matching interpreter.c's own raw sscanf("%s")-plus-leading-quote-
// check convention for these two (and SAVE, deferred below), the exact
// same restriction MAKE's varname/LOCAL's varname already have (see
// parser.c's own file comment on ARG_QUOTED_WORD): DELETEFILE WORD "a
// ".txt isn't a computed expression here, it's a syntax error, same as
// in interpreter.c.
void eval_deletefile_value(LogoApp *app, EvalValue path_val) {
    const char *path = path_val.word;
    if (g_remove(path) != 0) {
        append_output(app, "DELETEFILE: could not delete \"");
        append_output(app, path);
        append_output(app, "\n");
    }
}
static void do_deletefile(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_deletefile_value(app, word_val(pool->nodes[arg_idx[0]].text));
}
// LOAD "path -- reads a file and runs it as Logo source, the same
// re-entrant lex/parse-into-a-scratch-ParseResult machinery RUN/
// FOREACH already use. A bigger token budget than MAX_TEMPLATE_TOKENS
// (128, sized for a single small template/RUN'd value): a LOAD'd file
// is a whole external script, potentially many statements, not a short
// in-memory snippet. Like RUN, this runs through exec_block (a nested
// execution sharing the caller's scope, not a fresh top-level
// recovery point) -- an uncaught THROW inside a LOAD'd file propagates
// up to whatever called LOAD, exactly mirroring interpreter.c's own
// LOAD (a plain nested eval_logo call, no special top-level recovery
// of its own). No recursion-depth guard, also matching interpreter.c
// exactly -- unlike RUN, LOAD has never had one.
#define MAX_LOAD_TOKENS 8192
static void do_load(LogoApp *app, AstPool *pool, const int *arg_idx) {
    const char *path = pool->nodes[arg_idx[0]].text;
    char *contents = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(path, &contents, NULL, &error)) {
        append_output(app, "LOAD: could not read file\n");
        g_error_free(error);
        return;
    }
    LogoToken *tokens = malloc(sizeof(LogoToken) * MAX_LOAD_TOKENS);
    int n = logo_lex(contents, tokens, MAX_LOAD_TOKENS);
    if (n >= 0) {
        ParseResult *scratch = calloc(1, sizeof(ParseResult));
        logo_parse(tokens, n, scratch);
        if (scratch->error_count == 0) exec_block(app, &scratch->pool, scratch->program);
        parse_result_destroy(scratch);
    }
    free(tokens);
    g_free(contents);
}
// Renders one procedure's "TO name :params\n<body>\nEND\n" text into
// `out` -- the exact rendering interpreter.c's own append_procedure_text
// builds, shared by SAVE (every procedure, plus its own extra
// trailing '\n' per entry -- see do_save below) and SHOW (just the one
// asked for, no extra trailing '\n'). Each header is rebuilt from the
// node's already-known .text/.param_names (note the ':' has to be
// added back explicitly: this engine's own param_names never include
// it, unlike interpreter.c's own Procedure.param_names -- see ast.h's
// own note on this), and the body comes straight from
// .body_text/.body_len (see do_text above), the same literal-
// source-text capture that unblocked TEXT.
static void eval_append_procedure_text(GString *out, AstNode *n) {
    g_string_append(out, "TO ");
    g_string_append(out, n->text);
    for (int p = 0; p < n->param_count; p++) {
        g_string_append_c(out, ' ');
        g_string_append_c(out, ':');
        g_string_append(out, n->param_names[p]);
    }
    g_string_append_c(out, '\n');
    if (n->body_text != NULL && n->body_len > 0) {
        g_string_append_len(out, n->body_text, n->body_len);
    }
    g_string_append(out, "\nEND\n");
}
// SAVE "path -- writes every currently-defined procedure out as
// TO...END Logo source, readable back in by LOAD. Direct port of
// interpreter.c's own serialize_procedures, walking pool for
// AST_PROC_DEF nodes (the same reach do_procedures already uses,
// including its own skip-a-blank-.text-entry rule for an ERASE'd
// procedure) instead of interpreter.c's own app->procedures[] table.
void eval_save_value(LogoApp *app, AstPool *pool, EvalValue path_val) {
    const char *path = path_val.word;
    GString *out = g_string_new(NULL);
    for (int i = 0; i < pool->node_count; i++) {
        AstNode *n = &pool->nodes[i];
        if (n->type != AST_PROC_DEF || n->text[0] == '\0') continue;
        eval_append_procedure_text(out, n);
        // serialize_procedures appends one more '\n' per procedure on
        // top of append_procedure_text's own trailing "END\n" (a
        // separate step in interpreter.c, easy to miss) -- confirmed
        // directly (not assumed) via a byte-for-byte SAVE output diff
        // before this line was added: without it, procedures ran
        // together with no blank line between them, and the real
        // output has one after the very last procedure too.
        g_string_append_c(out, '\n');
    }
    GError *error = NULL;
    if (g_file_set_contents(path, out->str, (gssize)out->len, &error)) {
        append_output(app, "Saved ");
        append_output(app, path);
        append_output(app, "\n");
    } else {
        append_output(app, "SAVE: could not write file\n");
        g_error_free(error);
    }
    g_string_free(out, TRUE);
}
static void do_save(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_save_value(app, pool, word_val(pool->nodes[arg_idx[0]].text));
}
// SHOW "name -- prints one procedure's own TO...END definition back to
// the history pane, reusing eval_append_procedure_text directly (the
// same rendering SAVE writes to a file for every procedure, just this
// one and without SAVE's own extra per-procedure trailing blank line).
void eval_show_value(LogoApp *app, AstPool *pool, EvalValue name_val) {
    const char *name = name_val.word;
    int def_node = find_proc_def(pool, name);
    if (def_node < 0) {
        append_output(app, "SHOW: no such procedure \"");
        append_output(app, name);
        append_output(app, "\n");
        return;
    }
    GString *out = g_string_new(NULL);
    eval_append_procedure_text(out, &pool->nodes[def_node]);
    append_output(app, out->str);
    g_string_free(out, TRUE);
}
static void do_show(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_show_value(app, pool, word_val(pool->nodes[arg_idx[0]].text));
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
// NEW obj proto -- just SETPROP obj "prototype proto under the hood
// (see the property-list section above for the shared app->plist_entries
// state this rests on). Its own function, not inlined into do_new,
// specifically so the "prototype" key string lives in exactly one
// place -- vm.c's OP_CALL_BUILTIN "NEW" handler calls this too, rather
// than repeating that literal.
void eval_new_declare(LogoApp *app, EvalValue obj_val, EvalValue proto_val) {
    eval_setprop(app, obj_val, word_val("prototype"), proto_val);
}
static void do_new(LogoApp *app, AstPool *pool, const int *arg_idx) {
    eval_new_declare(app, eval_expr(app, pool, arg_idx[0]), eval_expr(app, pool, arg_idx[1]));
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
// Unpacks arglist_val into arg_vals, with obj_text's own value as
// arg_vals[0] (:self) followed by arglist_val's own elements (a
// non-list arglist counts as one element, same convention APPLY
// already uses). Shared between do_send (which then calls
// call_ast_procedure, a synchronous tree-walking call) and vm.c's own
// exec_send (which instead pushes a VmFrame and jumps into the
// resolved procedure's compiled body -- SEND's target isn't known
// until runtime either way, but once resolved, the argument-unpacking
// itself is identical). Returns how many arg_vals were filled (capped
// at max_args).
int eval_send_unpack_args(LogoApp *app, const char *obj_text, EvalValue arglist_val, EvalValue *arg_vals, int max_args) {
    arg_vals[0] = word_val(obj_text);
    int n = 1;
    if (arglist_val.type == VALUE_LIST) {
        for (int idx = arglist_val.list_head; idx != -1 && n < max_args; idx = app->list_pool[idx].next) {
            arg_vals[n++] = node_to_value(&app->list_pool[idx]);
        }
    } else if (n < max_args) {
        arg_vals[n++] = arglist_val;
    }
    return n;
}
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
    int n = eval_send_unpack_args(app, obj_text, arglist, arg_vals, AST_MAX_PARAMS);

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
    } else if (strcasecmp(name, "TELL") == 0) {
        do_tell(app, pool, arg_idx);
    } else if (strcasecmp(name, "WHO") == 0) {
        if (result != NULL) *result = do_who(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "CLEAR") == 0 || strcasecmp(name, "CS") == 0) {
        do_clear(app);
    } else if (strcasecmp(name, "ARC") == 0) {
        do_arc(app, pool, arg_idx);
    } else if (strcasecmp(name, "CLEAN") == 0) {
        do_clean(app);
    } else if (strcasecmp(name, "HIDETURTLE") == 0 || strcasecmp(name, "HT") == 0) {
        do_hideturtle(app);
    } else if (strcasecmp(name, "SHOWTURTLE") == 0 || strcasecmp(name, "ST") == 0) {
        do_showturtle(app);
    } else if (strcasecmp(name, "WRAP") == 0) {
        do_wrap(app);
    } else if (strcasecmp(name, "FENCE") == 0) {
        do_fence(app);
    } else if (strcasecmp(name, "WINDOW") == 0) {
        do_window(app);
    } else if (strcasecmp(name, "SETPENCOLOR") == 0 || strcasecmp(name, "SETPC") == 0) {
        do_setpencolor(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETPENWIDTH") == 0 || strcasecmp(name, "SETPW") == 0) {
        do_setpenwidth(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETBACKGROUND") == 0 || strcasecmp(name, "SETBG") == 0) {
        do_setbackground(app, pool, arg_idx);
    } else if (strcasecmp(name, "SETCANVASSIZE") == 0) {
        do_setcanvassize(app, pool, arg_idx);
    } else if (strcasecmp(name, "LABEL") == 0) {
        do_label(app, pool, arg_idx);
    } else if (strcasecmp(name, "FILL") == 0) {
        do_fill(app);
    } else if (strcasecmp(name, "ERASERECT") == 0) {
        do_eraserect(app, pool, arg_idx);
    } else if (strcasecmp(name, "ERASE") == 0) {
        do_erase(app, pool, arg_idx);
    } else if (strcasecmp(name, "PRINT") == 0) {
        do_print(app, pool, arg_idx);
    } else if (strcasecmp(name, "MAKE") == 0) {
        do_make(app, pool, arg_idx);
    } else if (strcasecmp(name, "THING") == 0) {
        if (result != NULL) *result = do_thing(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "LOCAL") == 0) {
        do_local(app, pool, arg_idx);
    } else if (strcasecmp(name, "NAMES") == 0) {
        if (result != NULL) *result = eval_names_value(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "PROCEDURES") == 0) {
        if (result != NULL) *result = do_procedures(app, pool);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "TEXT") == 0) {
        if (result != NULL) *result = do_text(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SHOW") == 0) {
        do_show(app, pool, arg_idx);
    } else if (strcasecmp(name, "OUTPUT") == 0) {
        do_output(app, pool, arg_idx);
    } else if (strcasecmp(name, "STOP") == 0) {
        do_stop(app);
    } else if (strcasecmp(name, "REPEAT") == 0) {
        do_repeat(app, pool, arg_idx);
    } else if (strcasecmp(name, "WHILE") == 0) {
        do_while(app, pool, arg_idx);
    } else if (strcasecmp(name, "FOREVER") == 0) {
        do_forever(app, pool, arg_idx);
    } else if (strcasecmp(name, "CATCH") == 0) {
        do_catch(app, pool, arg_idx);
    } else if (strcasecmp(name, "THROW") == 0) {
        do_throw(app, pool, arg_idx);
    } else if (strcasecmp(name, "RUN") == 0) {
        do_run(app, pool, arg_idx);
    } else if (strcasecmp(name, "APPLY") == 0) {
        do_apply(app, pool, arg_idx);
    } else if (strcasecmp(name, "OPENREAD") == 0) {
        if (result != NULL) *result = do_openread(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "OPENWRITE") == 0) {
        if (result != NULL) *result = do_openwrite(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "OPENAPPEND") == 0) {
        if (result != NULL) *result = do_openappend(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "READLINE") == 0) {
        if (result != NULL) *result = do_readline(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "EOF?") == 0) {
        if (result != NULL) *result = do_eofp(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "DIRECTORY") == 0) {
        if (result != NULL) *result = do_directory(app);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "CLOSE") == 0) {
        do_close(app, pool, arg_idx);
    } else if (strcasecmp(name, "FILEPRINT") == 0) {
        do_fileprint(app, pool, arg_idx);
    } else if (strcasecmp(name, "DELETEFILE") == 0) {
        do_deletefile(app, pool, arg_idx);
    } else if (strcasecmp(name, "LOAD") == 0) {
        do_load(app, pool, arg_idx);
    } else if (strcasecmp(name, "SAVE") == 0) {
        do_save(app, pool, arg_idx);
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
    } else if (strcasecmp(name, "PICK") == 0) {
        if (result != NULL) *result = do_pick(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "FLATTEN") == 0) {
        if (result != NULL) *result = do_flatten(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "PARSE") == 0) {
        if (result != NULL) *result = do_parse(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "SUBST") == 0) {
        if (result != NULL) *result = do_subst(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "DOT") == 0) {
        if (result != NULL) *result = do_dot(app, pool, arg_idx);
        if (produced != NULL) *produced = 1;
    } else if (strcasecmp(name, "CROSS") == 0) {
        if (result != NULL) *result = do_cross(app, pool, arg_idx);
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

// Builds a real list in app->list_pool from an AST_LIST_LITERAL node,
// mirroring interpreter.c's own parse_list_literal exactly: each leaf
// is untyped raw text (an AST_WORD child's .text, never number-vs-word
// typed at construction time), and a nested AST_LIST_LITERAL recurses
// through this same function directly (not back through eval_expr --
// functionally identical, since eval_expr's own AST_LIST_LITERAL case
// just calls this, but avoids a pointless extra dispatch through the
// whole eval_expr switch for something that can only ever be this one
// node type). ast_eval's own AST_LIST_LITERAL case is this function's
// only remaining direct caller of the AstPool-based form -- vm.c's own
// OP_PUSH_LIST_LITERAL instead calls eval_build_list_literal_from_text
// below (see docs/BYTECODE_VM_DESIGN.md's "Self-contained
// BytecodeChunk" entry for why the VM stopped keeping a live AstPool
// reference for this).
EvalValue eval_build_list_literal(LogoApp *app, AstPool *pool, int node_idx) {
    AstNode *node = &pool->nodes[node_idx];
    int head = -1;
    int *next_slot = &head;
    for (int c = node->first_child; c >= 0; c = pool->nodes[c].next_sibling) {
        AstNode *child = &pool->nodes[c];
        int idx = list_alloc_node(app);
        if (idx < 0) return list_pool_exhausted(app);
        ListNode *ln = &app->list_pool[idx];
        ln->next = -1;
        if (child->type == AST_LIST_LITERAL) {
            EvalValue sub = eval_build_list_literal(app, pool, c);
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

// vm.c's own OP_PUSH_LIST_LITERAL entry point: `text` is bracket-wrapped
// Logo source (e.g. "[1 2 [3 4]]"), compiler.c's own rendering of the
// original AST_LIST_LITERAL (render_list_literal_source) stored
// verbatim on the chunk instead of an AstPool node index -- see
// bytecode.h's own list_literals[] comment. Lexes/parses it fresh right
// here (same "re-parse text at runtime" shape OP_RUN/OP_LOAD already
// use for arbitrary code, just for one list-literal expression) into a
// throwaway scratch ParseResult, then defers to eval_build_list_literal
// above for the actual construction -- same tradeoff RUN/LOAD already
// accept: a fresh ~6.7MB ParseResult alloc/free per call, not amortized
// across repeated visits the way MAP/FILTER/REDUCE's own reused
// `scratch` is, so a list literal inside a hot loop pays a real,
// measurable-but-modest re-parse cost now that it didn't before. A
// malformed rendering (would mean a real bug in render_list_literal_source
// itself, not a user-facing error case) reads as an empty list, same
// "quietly inert rather than crashing" fallback eval_apply_template_expr
// already uses for its own lex/parse failures.
#define LIST_LITERAL_MAX_TOKENS 512
EvalValue eval_build_list_literal_from_text(LogoApp *app, const char *text) {
    LogoToken tokens[LIST_LITERAL_MAX_TOKENS];
    int n = logo_lex(text, tokens, LIST_LITERAL_MAX_TOKENS);
    if (n < 0) return list_val(-1);
    ParseResult *scratch = calloc(1, sizeof(ParseResult));
    if (!scratch) return list_val(-1);
    int node = logo_parse_expr(tokens, n, scratch);
    if (node < 0 || scratch->error_count > 0) {
        parse_result_destroy(scratch);
        return list_val(-1);
    }
    EvalValue result = eval_build_list_literal(app, &scratch->pool, node);
    parse_result_destroy(scratch);
    return result;
}

static EvalValue eval_expr(LogoApp *app, AstPool *pool, int node_idx) {
    AstNode *node = &pool->nodes[node_idx];
    switch (node->type) {
        case AST_NUMBER:
            return num_val(node->number);
        case AST_WORD:
            return word_val(node->text);
        case AST_VARREF: {
            Variable *v = find_var(app, app_scope_stack(app), node->text);
            if (v == NULL) return num_val(0); // unset :name reads as 0, matching interpreter.c
            if (v->type == VALUE_WORD) return word_val(v->word);
            if (v->type == VALUE_NUMBER) return num_val(v->number);
            if (v->type == VALUE_LIST) return list_val(v->list_head);
            return array_val(v->list_head, (int)v->number);
        }
        case AST_LIST_LITERAL:
            return eval_build_list_literal(app, pool, node_idx);
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

// FOR [var start limit step] [block] -- a counted loop; the loop
// variable is set via plain set_var each iteration (no push_scope,
// matching REPEAT/WHILE's own blocks -- MAKE-ing over an outer
// variable of the same name has the same effect it always would),
// unlike a real parameter. start/limit/step are evaluated once, up
// front, exactly mirroring interpreter.c's own FOR -- reevaluating
// them per-iteration isn't what today's interpreter does either.
// step's node may be absent (parse_for only appends it when the
// header had a 4th element before ]); when absent, node->first_child's
// chain has exactly 3 children (start, limit, block) instead of 4, and
// step defaults to +1 (or -1 if limit < start), same as interpreter.c.
static void exec_for(LogoApp *app, AstPool *pool, int for_node) {
    AstNode *node = &pool->nodes[for_node];
    int start_node = node->first_child;
    int limit_node = pool->nodes[start_node].next_sibling;
    int third_node = pool->nodes[limit_node].next_sibling;
    int fourth_node = pool->nodes[third_node].next_sibling;
    int step_node = (fourth_node >= 0) ? third_node : -1;
    int block_node = (fourth_node >= 0) ? fourth_node : third_node;

    double start = eval_to_number(eval_expr(app, pool, start_node));
    double limit = eval_to_number(eval_expr(app, pool, limit_node));
    double step = (step_node >= 0) ? eval_to_number(eval_expr(app, pool, step_node))
                                    : ((limit >= start) ? 1 : -1);
    if (step == 0) {
        append_output(app, "FOR: step must not be 0\n");
        return;
    }

    int iterations = 0;
    for (double i = start; (step > 0) ? (i <= limit) : (i >= limit); i += step) {
        if (iterations >= MAX_WHILE_ITERATIONS) {
            append_output(app, "FOR: stopped after too many iterations\n");
            break;
        }
        set_var(app, app_scope_stack(app), node->text, i);
        exec_block(app, pool, block_node);
        if (app->stop_requested || app->throw_requested) break;
        iterations++;
    }
}

static void exec_statement(LogoApp *app, AstPool *pool, int node_idx) {
    AstNode *node = &pool->nodes[node_idx];
    if (node->type == AST_PROC_DEF) return; // already resolvable via find_proc_def; nothing to run
    if (node->type == AST_IF) {
        exec_if(app, pool, node_idx);
        return;
    }
    if (node->type == AST_FOR) {
        exec_for(app, pool, node_idx);
        return;
    }
    exec_call(app, pool, node_idx, NULL, NULL, NULL); // a plain command: discard whatever it OUTPUTs
}

static void exec_block(LogoApp *app, AstPool *pool, int block_node) {
    for (int c = pool->nodes[block_node].first_child; c >= 0; c = pool->nodes[c].next_sibling) {
        exec_statement(app, pool, c);
        // No g_interrupt_requested check here (see the file comment at
        // the top) -- only OUTPUT/STOP/THROW unwinds a block early
        // today. Unlike ast_eval's own top-level loop below, a nested
        // block never recovers from an uncaught throw_requested itself
        // -- it just breaks, letting the throw keep propagating up the
        // C call stack toward whichever CATCH (or ast_eval's own
        // outermost recovery) actually stops it, exactly mirroring
        // interpreter.c's eval_logo at any nesting depth greater than 1.
        if (app->stop_requested || app->throw_requested) break;
    }
}

// The genuine top level -- unlike exec_block above (used for every
// nested block: loop bodies, procedure bodies, RUN'd chunks), this is
// the one place that recovers from an uncaught THROW, mirroring
// eval_logo's own eval_depth == 1 recovery. Runs statement-by-statement
// (rather than one exec_block call) so that recovery can happen after
// each one and let the *rest* of the top-level script keep running,
// instead of a single uncaught THROW aborting everything that follows
// it in the same script. Takes a starting node index rather than
// always `pool->nodes[program_node].first_child`, so a caller that
// re-parses a growing accumulated source from scratch each time (e.g.
// tools/logo_new_cli.c's REPL, re-running the whole session so far to
// keep earlier TO definitions resolvable via find_proc_def) can run
// only the newly added tail statements instead of re-executing
// everything already run in an earlier turn.
void ast_eval_from(LogoApp *app, AstPool *pool, int start_node) {
    for (int c = start_node; c >= 0; c = pool->nodes[c].next_sibling) {
        exec_statement(app, pool, c);
        if (app->throw_requested) eval_report_uncaught_throw(app);
        if (app->stop_requested) break;
    }
    // A STOP with no enclosing procedure call, mirroring eval_logo's
    // own top-level recovery -- otherwise it would stay set forever
    // and silently no-op every later top-level statement.
    if (app->stop_requested) app->stop_requested = FALSE;
    app->has_output_value = FALSE;
}

void ast_eval(LogoApp *app, AstPool *pool, int program_node) {
    ast_eval_from(app, pool, pool->nodes[program_node].first_child);
}
