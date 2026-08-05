// interpreter.c
//
// The Logo language core: tokenizing and evaluating commands
// (eval_logo), the recursive-descent expression/condition parser used
// by every numeric argument, the procedure and variable symbol tables,
// and the REPL-input-completeness check the UI uses to decide when
// Enter should run a command versus just add a line.

#include "interpreter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

// --- HELPER FUNCTIONS ---

// Advance past leading whitespace and ';' line comments (a comment runs
// from ';' to the next newline or end of input). Called everywhere a
// token boundary is found — the main eval_logo loop, expression parsing,
// extract_block, list literals — so a comment is recognized wherever
// whitespace already is, with no separate comment-stripping pass needed.
// A ';' with no preceding whitespace (glued onto a word or "-literal) is
// just an ordinary character, not a comment marker, same as any language
// where a comment must start at a token boundary.
static const char* skip_whitespace(const char *str) {
    for (;;) {
        while (*str && isspace((unsigned char)*str)) str++;
        if (*str == ';') {
            while (*str && *str != '\n') str++;
            continue; // more whitespace, or another comment line, may follow
        }
        break;
    }
    return str;
}

// Parse a bracketed [ ... ] block, honoring nested brackets, and copy its
// contents into buffer. Returns the position just past the closing ']',
// or NULL if str doesn't start with '[', the block is never closed
// before the input ends, or its content doesn't fit in buf_size. The
// latter two used to be silently accepted with an unterminated/truncated
// buffer instead of failing — every caller already treats NULL as "report
// an error", so fixing this here fixes it everywhere at once.
static const char* extract_block(const char *str, char *buffer, size_t buf_size) {
    str = skip_whitespace(str);
    if (*str != '[') return NULL;

    str++;
    int depth = 1;
    size_t idx = 0;
    gboolean truncated = FALSE;

    while (*str && depth > 0) {
        if (*str == '[') depth++;
        else if (*str == ']') depth--;

        if (depth > 0) {
            if (idx < buf_size - 1) {
                buffer[idx++] = *str;
            } else {
                truncated = TRUE;
            }
            str++;
        }
    }

    if (depth > 0 || truncated) return NULL; // unterminated, or didn't fit

    buffer[idx] = '\0';
    if (*str == ']') str++;
    return str;
}

// Allocate a new node in app->list_pool. Returns -1 if the pool is
// exhausted (MAX_LIST_NODES) — same "cap it, report a loud error" policy
// as every other fixed buffer in this codebase; every caller treats -1
// as "report an error and abandon this list operation."
static int list_alloc_node(LogoApp *app) {
    if (app->list_pool_count >= MAX_LIST_NODES) return -1;
    return app->list_pool_count++;
}

// Parse a bracketed [ ... ] list literal starting at `str` (which must
// point at '['), building real ListNode chain entries in app->list_pool
// instead of flattening nested brackets into text (the old
// extract_word_list-based approach silently corrupted anything nested,
// e.g. mis-tokenizing [a [b c] d] into four space-split "words"). Each
// element is either a nested list (recursing on '[') or a whitespace-
// delimited word. Sets *out_head to the list's head node index (-1 for
// an empty list) and returns the position just past the closing ']', or
// NULL if the bracket is unterminated or the pool fills up.
static const char* parse_list_literal(LogoApp *app, const char *str, int *out_head) {
    str = skip_whitespace(str);
    if (*str != '[') return NULL;
    str++;

    int head = -1;
    int *next_slot = &head;

    for (;;) {
        str = skip_whitespace(str);
        if (*str == '\0') return NULL; // unterminated
        if (*str == ']') { str++; break; }

        int node_idx = list_alloc_node(app);
        if (node_idx < 0) return NULL; // pool exhausted

        if (*str == '[') {
            int sub_head;
            str = parse_list_literal(app, str, &sub_head);
            if (str == NULL) return NULL;
            app->list_pool[node_idx].type = LIST_ELEM_LIST;
            app->list_pool[node_idx].sublist_head = sub_head;
        } else {
            char word[512] = {0};
            size_t i = 0;
            while (*str && !isspace((unsigned char)*str) && *str != '[' && *str != ']' && i < sizeof(word) - 1) {
                word[i++] = *str;
                str++;
            }
            word[i] = '\0';
            app->list_pool[node_idx].type = LIST_ELEM_WORD;
            snprintf(app->list_pool[node_idx].word, sizeof(app->list_pool[node_idx].word), "%s", word);
        }
        app->list_pool[node_idx].next = -1;
        *next_slot = node_idx;
        next_slot = &app->list_pool[node_idx].next;
    }

    *out_head = head;
    return str;
}

// Clamp a color channel to the valid 0.0-1.0 range Cairo expects.
static double clamp01(double v) {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

// Clamp to an arbitrary [lo, hi] range, e.g. pen width.
static double clamp_range(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// MOD: remainder with the same sign as the divisor (so MOD of a
// negative number never comes out negative for a positive divisor) —
// the more commonly expected "mod" outside of C's own fmod, which takes
// the sign of the dividend instead.
static double mod_result(double a, double b) {
    if (b == 0) return 0;
    double r = fmod(a, b);
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

// RANDOM n: a random integer in [0, n). Seeds the C library's RNG from
// the current time on first use only — this is turtle-graphics
// randomness (spirals, scattering, games), not anything needing a
// cryptographic or reproducible sequence.
static double random_below(double n) {
    static gboolean seeded = FALSE;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = TRUE;
    }
    int limit = (int)n;
    if (limit <= 0) return 0;
    return rand() % limit;
}

#define MIN_PEN_WIDTH 0.5
#define MAX_PEN_WIDTH 20.0

// Where CLEAR and HOME send the turtle back to.
#define HOME_X 250.0
#define HOME_Y 250.0

// The turtle currently being controlled by FD/RT/SETXY/etc. — see TELL.
static Turtle* current_turtle(LogoApp *app) {
    return &app->turtles[app->current_turtle];
}

// Reset `t` to the default turtle state: home position, heading 0, pen
// down, default color/width.
void init_turtle(Turtle *t) {
    *t = (Turtle){
        .x = HOME_X, .y = HOME_Y, .angle = 0, .pen_down = 1,
        .pen_r = 0.1, .pen_g = 0.1, .pen_b = 0.1, .pen_width = 2.0,
    };
}

// Record a line segment in the current turtle's pen color/width, if its
// pen is down. Doesn't touch any turtle's position — used directly by
// ARC, which draws around the turtle without moving it.
static void record_line(LogoApp *app, double x1, double y1, double x2, double y2) {
    Turtle *t = current_turtle(app);
    if (t->pen_down && app->line_count < MAX_LINES) {
        app->lines[app->line_count++] = (LineSegment){
            .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2,
            .r = t->pen_r, .g = t->pen_g, .b = t->pen_b,
            .width = t->pen_width,
        };
    }
}

// Move the current turtle directly to an absolute position, recording a
// line segment along the way if the pen is down.
static void move_turtle_to(LogoApp *app, double new_x, double new_y) {
    Turtle *t = current_turtle(app);
    record_line(app, t->x, t->y, new_x, new_y);
    t->x = new_x;
    t->y = new_y;
}

// Move the current turtle by `distance` along its current heading.
static void move_turtle_forward(LogoApp *app, double distance) {
    double rad = (current_turtle(app)->angle - 90.0) * M_PI / 180.0;
    move_turtle_to(app,
                    current_turtle(app)->x + distance * cos(rad),
                    current_turtle(app)->y + distance * sin(rad));
}

// Look up a user-defined procedure by name (case-insensitive).
static Procedure* find_procedure(LogoApp *app, const char *name) {
    for (int i = 0; i < app->proc_count; i++) {
        if (strcasecmp(app->procedures[i].name, name) == 0) {
            return &app->procedures[i];
        }
    }
    return NULL;
}

// Find a variable binding by name (case-insensitive). Searches the scope
// stack from the innermost active call outward, so a procedure's own
// parameter shadows a same-named variable from an outer call or a
// global, then falls back to the globals. NULL if unbound anywhere.
static Variable* find_var(LogoApp *app, const char *name) {
    for (int s = app->scope_depth - 1; s >= 0; s--) {
        Scope *scope = &app->scopes[s];
        for (int i = 0; i < scope->count; i++) {
            if (strcasecmp(scope->vars[i].name, name) == 0) {
                return &scope->vars[i];
            }
        }
    }
    for (int i = 0; i < app->var_count; i++) {
        if (strcasecmp(app->variables[i].name, name) == 0) {
            return &app->variables[i];
        }
    }
    return NULL;
}

// Find an existing binding to update (same inside-out search as
// find_var), or create a new global if `name` isn't bound anywhere yet.
// NULL only if the global table is full and there's no existing binding.
static Variable* find_or_create_var(LogoApp *app, const char *name) {
    Variable *v = find_var(app, name);
    if (v != NULL) return v;
    if (app->var_count < MAX_VARIABLES) {
        Variable *nv = &app->variables[app->var_count++];
        snprintf(nv->name, sizeof(nv->name), "%s", name);
        return nv;
    }
    append_output(app, "MAKE: too many variables defined, not set\n");
    return NULL;
}

// Set a variable to a number (MAKE "name expr), creating it as a global
// if it's not already bound in some active scope.
static void set_var(LogoApp *app, const char *name, double value) {
    Variable *v = find_or_create_var(app, name);
    if (v != NULL) {
        v->type = VALUE_NUMBER;
        v->number = value;
    }
}

// Set a variable to a list (MAKE "name [...] / a list operator result),
// same binding rules as set_var. Just copies the head index — safe
// aliasing, since list nodes are never mutated after being built (see
// the ListNode comment in logo_types.h).
static void set_var_list(LogoApp *app, const char *name, int list_head) {
    Variable *v = find_or_create_var(app, name);
    if (v != NULL) {
        v->type = VALUE_LIST;
        v->list_head = list_head;
    }
}

// Set a variable to a word (MAKE "name "word), same binding rules as
// set_var.
static void set_var_word(LogoApp *app, const char *name, const char *word) {
    Variable *v = find_or_create_var(app, name);
    if (v != NULL) {
        v->type = VALUE_WORD;
        snprintf(v->word, sizeof(v->word), "%s", word);
    }
}

// --- EXPRESSION EVALUATION (numbers, words, lists, :variables, + - * / (), comparisons) ---
//
// A single Value type flows through the whole expression evaluator: every
// argument-parsing entry point (parse_expr, used directly by PRINT, MAKE,
// procedure-call arguments, every turtle command's numeric arguments, and
// parse_comparison) returns one. That's what lets a word or list work
// anywhere a number would, not just in PRINT/MAKE/comparisons specifically
// — FD FIRST :colors, MAKE "x FIRST :list + 1, PRINT WORD "a "b, and so on.

typedef struct {
    ValueType type;
    double number;
    char word[512];
    int list_head; // type == VALUE_LIST: index into app->list_pool, -1 = empty
} Value;

static Value number_value(double n) {
    Value v = {0};
    v.type = VALUE_NUMBER;
    v.number = n;
    v.list_head = -1;
    return v;
}

static Value word_value(const char *w) {
    Value v = {0};
    v.type = VALUE_WORD;
    snprintf(v.word, sizeof(v.word), "%s", w);
    v.list_head = -1;
    return v;
}

static Value list_value(int head) {
    Value v = {0};
    v.type = VALUE_LIST;
    v.list_head = head;
    return v;
}

// Coerce a Value to a number for arithmetic: a number as-is; a word by
// parsing its leading numeric text — so FIRST [100 50] behaves like the
// number 100 in FD FIRST :colors — falling back to 0 if it doesn't start
// with one at all (same "coerces rather than errors" rule as always); a
// list coerces to 0, having no meaningful numeric reading.
static double value_to_number(Value v) {
    if (v.type == VALUE_NUMBER) return v.number;
    if (v.type == VALUE_LIST) return 0;
    char *end;
    double n = strtod(v.word, &end);
    return (end != v.word) ? n : 0;
}

// Render a list's elements into `out`, space-separated, without
// enclosing brackets at this level — matching how a bracketed list
// literal has always printed at the top level (PRINT [a b] -> "a b").
// A nested LIST element recurses one level deeper, wrapped in its own
// brackets: that's the only place brackets appear in output, since the
// literal syntax's outer brackets were never part of the printed value.
static void list_elements_to_text(LogoApp *app, int head, char *out, size_t out_size) {
    out[0] = '\0';
    gboolean first = TRUE;
    for (int idx = head; idx != -1; idx = app->list_pool[idx].next) {
        if (!first) strncat(out, " ", out_size - strlen(out) - 1);
        first = FALSE;
        ListNode *node = &app->list_pool[idx];
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
}

// Render a Value as text: a word as-is, a number formatted the same way
// PRINT shows it, a list via list_elements_to_text above.
static void value_to_text(LogoApp *app, const Value *v, char *out, size_t out_size) {
    if (v->type == VALUE_WORD) {
        snprintf(out, out_size, "%s", v->word);
    } else if (v->type == VALUE_LIST) {
        list_elements_to_text(app, v->list_head, out, out_size);
    } else {
        snprintf(out, out_size, "%g", v->number);
    }
}

// Whether two values are "equal" the same way = already treats them:
// plain numeric equality if both sides are numbers, otherwise a
// case-insensitive text comparison — shared by parse_comparison's `=`/
// `<>` and by MEMBER? below, rather than duplicating the same rule.
static gboolean values_equal(LogoApp *app, Value a, Value b) {
    if (a.type == VALUE_NUMBER && b.type == VALUE_NUMBER) {
        return a.number == b.number;
    }
    char a_text[512], b_text[512];
    value_to_text(app, &a, a_text, sizeof(a_text));
    value_to_text(app, &b, b_text, sizeof(b_text));
    return strcasecmp(a_text, b_text) == 0;
}

// Store `v` as a single new list-element node — used when FPUT/LPUT wrap
// `thing`, and when LIST wraps a whole argument as one element. If v is
// itself a list, the new node just references its existing chain by
// index (no deep copy needed, since list nodes are never mutated after
// being built). Returns -1 if the pool is exhausted.
static int list_node_from_value(LogoApp *app, Value v) {
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

// Map a list element node back to a Value — used by FIRST/LAST to return
// whichever element they land on (which may itself be a list).
static Value list_node_to_value(const ListNode *node) {
    if (node->type == LIST_ELEM_NUMBER) return number_value(node->number);
    if (node->type == LIST_ELEM_LIST) return list_value(node->sublist_head);
    return word_value(node->word);
}

// Copy `src_idx`'s node payload (type/number/word/sublist_head) into a
// freshly allocated node — used to build a new top-level spine for
// SENTENCE/LPUT without mutating (or aliasing into) whatever existing
// list `src_idx` came from. A LIST_ELEM_LIST payload's sublist_head is
// copied as-is (structural sharing) rather than recursively copied,
// since nested sublists are never mutated either. Returns -1 if the pool
// is exhausted.
static int list_node_copy(LogoApp *app, int src_idx) {
    int idx = list_alloc_node(app);
    if (idx < 0) return -1;
    app->list_pool[idx] = app->list_pool[src_idx];
    app->list_pool[idx].next = -1;
    return idx;
}

// Reported when a list-construction operator runs out of pool space —
// same "loud error, not silent corruption" policy as every other fixed
// buffer here.
static Value list_pool_exhausted_error(LogoApp *app) {
    append_output(app, "list storage full, list operation ignored\n");
    return word_value("");
}

static Value parse_expr(LogoApp *app, const char **ptr);

// Peek the next whitespace-delimited word without consuming it unless it
// case-insensitively matches `keyword`, in which case `*ptr` is advanced
// past it. Used to recognize the NOT/AND/OR/FIRST/BUTFIRST/LAST/COUNT/
// FPUT/LPUT/WORD/SENTENCE/SE/LIST keywords.
static gboolean consume_keyword(const char **ptr, const char *keyword) {
    const char *lookahead = skip_whitespace(*ptr);
    char word[16] = {0};
    int n = 0;
    if (sscanf(lookahead, "%15s%n", word, &n) == 1 && strcasecmp(word, keyword) == 0) {
        *ptr = lookahead + n;
        return TRUE;
    }
    return FALSE;
}

// --- LIST OPERATORS & CONSTRUCTION (FIRST, BUTFIRST, LAST, COUNT, FPUT, LPUT, WORD, SENTENCE/SE, LIST) ---
//
// A "list" is a real (if flat-by-default) recursive structure now — see
// the ListNode/list_pool comment in logo_types.h — built and read via
// the parse_list_literal/list_node_* helpers above. See the "Words &
// lists" section of docs/LANGUAGE.md.

// WORD: concatenate two words directly, with no separating space — pure
// string concatenation (WORD "hello "world -> "helloworld). Only
// accepts word/number arguments; a list argument is a reported error
// (see the WORD branch in parse_factor) rather than silently
// stringifying its structure away.
static void list_word(const char *a, const char *b, char *out, size_t out_size) {
    snprintf(out, out_size, "%s%s", a, b);
}

// SENTENCE/SE: splice two values into one flat list — a LIST-typed
// argument contributes its own elements individually; a word/number
// argument contributes itself as one element. Builds a fresh top-level
// spine (copying each spliced-in node via list_node_copy) so the result
// doesn't alias into (or get corrupted by) whatever `a` and `b` came
// from.
static Value list_sentence(LogoApp *app, Value a, Value b) {
    int new_head = -1;
    int *next_slot = &new_head;
    Value parts[2] = {a, b};

    for (int p = 0; p < 2; p++) {
        if (parts[p].type == VALUE_LIST) {
            for (int idx = parts[p].list_head; idx != -1; idx = app->list_pool[idx].next) {
                int copy = list_node_copy(app, idx);
                if (copy < 0) return list_pool_exhausted_error(app);
                *next_slot = copy;
                next_slot = &app->list_pool[copy].next;
            }
        } else {
            int node = list_node_from_value(app, parts[p]);
            if (node < 0) return list_pool_exhausted_error(app);
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
    }
    return list_value(new_head);
}

// LIST: combine two values into a new 2-element list, each argument
// becoming exactly one element regardless of whether it's itself a list
// — unlike SENTENCE above, this never splices. This is the whole reason
// SENTENCE and LIST used to be the same operation before nested lists
// existed: there was no such thing yet as "wrap a list as a single
// element".
static Value list_wrap_pair(LogoApp *app, Value a, Value b) {
    int node_a = list_node_from_value(app, a);
    if (node_a < 0) return list_pool_exhausted_error(app);
    int node_b = list_node_from_value(app, b);
    if (node_b < 0) return list_pool_exhausted_error(app);
    app->list_pool[node_a].next = node_b;
    return list_value(node_a);
}

// FPUT: prepend `thing` as a new first element of `list`. If `list` is a
// word, this instead prepends `thing`'s text as new leading characters,
// producing a new word (FPUT "a "bc -> "abc") — a word is a sequence of
// characters, not one atomic element, matching how FIRST/LAST/BUTFIRST/
// COUNT read it (see above); `thing` being a list itself has no
// character-level meaning to splice in, so that's a reported error
// instead. A bare number still wraps as a one-element list first, same
// convention FIRST/LAST/BUTFIRST/COUNT use for one. Otherwise, just one
// new node: its `next` points straight at the existing chain, which is
// never touched.
static Value list_fput(LogoApp *app, Value thing, Value list) {
    if (list.type == VALUE_WORD) {
        if (thing.type == VALUE_LIST) {
            append_output(app, "FPUT: can't add a list to a word\n");
            return word_value("");
        }
        char thing_text[512];
        value_to_text(app, &thing, thing_text, sizeof(thing_text));
        Value result = word_value("");
        snprintf(result.word, sizeof(result.word), "%s%s", thing_text, list.word);
        return result;
    }

    int list_head = (list.type == VALUE_LIST) ? list.list_head : list_node_from_value(app, list);
    if (list.type != VALUE_LIST && list_head < 0) return list_pool_exhausted_error(app);

    int new_node = list_node_from_value(app, thing);
    if (new_node < 0) return list_pool_exhausted_error(app);
    app->list_pool[new_node].next = list_head;
    return list_value(new_node);
}

// LPUT: append `thing` as a new last element of `list` (same word/list/
// number handling as FPUT above, just appending instead of prepending).
// For a genuine list, appending means copying the whole spine — an
// existing node's `next` can't be repointed without corrupting whatever
// else already shares that chain — then attaching a fresh node for
// `thing` at the tail.
static Value list_lput(LogoApp *app, Value thing, Value list) {
    if (list.type == VALUE_WORD) {
        if (thing.type == VALUE_LIST) {
            append_output(app, "LPUT: can't add a list to a word\n");
            return word_value("");
        }
        char thing_text[512];
        value_to_text(app, &thing, thing_text, sizeof(thing_text));
        Value result = word_value("");
        snprintf(result.word, sizeof(result.word), "%s%s", list.word, thing_text);
        return result;
    }

    int src_head = (list.type == VALUE_LIST) ? list.list_head : list_node_from_value(app, list);
    if (list.type != VALUE_LIST && src_head < 0) return list_pool_exhausted_error(app);

    int new_head = -1;
    int *next_slot = &new_head;
    for (int idx = src_head; idx != -1; idx = app->list_pool[idx].next) {
        int copy = list_node_copy(app, idx);
        if (copy < 0) return list_pool_exhausted_error(app);
        *next_slot = copy;
        next_slot = &app->list_pool[copy].next;
    }

    int new_node = list_node_from_value(app, thing);
    if (new_node < 0) return list_pool_exhausted_error(app);
    *next_slot = new_node;
    return list_value(new_head);
}

// BUTLAST: everything except the last element of a list — the
// complement of BUTFIRST. Unlike BUTFIRST (which can just point at an
// existing suffix), this needs the *prefix* up to the last element,
// which means copying every node except the last one into a fresh
// chain: a singly-linked list has no way to walk backward from the end,
// so there's no existing sub-chain to just reference.
static Value list_butlast(LogoApp *app, Value arg) {
    int count = 0;
    for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
    if (count <= 1) return list_value(-1);

    int new_head = -1;
    int *next_slot = &new_head;
    int idx = arg.list_head;
    for (int i = 0; i < count - 1; i++) {
        int copy = list_node_copy(app, idx);
        if (copy < 0) return list_pool_exhausted_error(app);
        *next_slot = copy;
        next_slot = &app->list_pool[copy].next;
        idx = app->list_pool[idx].next;
    }
    return list_value(new_head);
}

// Parse a single value: a parenthesized expression, a unary +/-, a "word
// literal, a [list] literal, a list operator/constructor (FIRST,
// BUTFIRST, LAST, COUNT, FPUT, LPUT, WORD, SENTENCE/SE, LIST — each
// taking the next factor(s) as its argument, so they bind tighter than
// * / + - and nest freely: FIRST FPUT "a [b c]), a :variable (carrying
// whichever type it holds), or a numeric literal.
static Value parse_factor(LogoApp *app, const char **ptr) {
    *ptr = skip_whitespace(*ptr);

    if (**ptr == '(') {
        (*ptr)++;
        Value val = parse_expr(app, ptr);
        *ptr = skip_whitespace(*ptr);
        if (**ptr == ')') (*ptr)++;
        return val;
    }
    if (**ptr == '-') {
        (*ptr)++;
        return number_value(-value_to_number(parse_factor(app, ptr)));
    }
    if (**ptr == '+') {
        (*ptr)++;
        return number_value(value_to_number(parse_factor(app, ptr)));
    }
    if (consume_keyword(ptr, "MOD")) {
        double a = value_to_number(parse_factor(app, ptr));
        double b = value_to_number(parse_factor(app, ptr));
        return number_value(mod_result(a, b));
    }
    if (consume_keyword(ptr, "POWER")) {
        double a = value_to_number(parse_factor(app, ptr));
        double b = value_to_number(parse_factor(app, ptr));
        return number_value(pow(a, b));
    }
    if (consume_keyword(ptr, "SQRT")) {
        return number_value(sqrt(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "SIN")) {
        return number_value(sin(value_to_number(parse_factor(app, ptr)) * M_PI / 180.0));
    }
    if (consume_keyword(ptr, "COS")) {
        return number_value(cos(value_to_number(parse_factor(app, ptr)) * M_PI / 180.0));
    }
    if (consume_keyword(ptr, "ARCTAN")) {
        return number_value(atan(value_to_number(parse_factor(app, ptr))) * 180.0 / M_PI);
    }
    if (consume_keyword(ptr, "RANDOM")) {
        return number_value(random_below(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "ROUND")) {
        return number_value(round(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "ABS")) {
        return number_value(fabs(value_to_number(parse_factor(app, ptr))));
    }
    if (**ptr == '"') {
        (*ptr)++;
        char word[512] = {0};
        size_t i = 0;
        while (**ptr && !isspace((unsigned char)**ptr) && i < sizeof(word) - 1) {
            word[i++] = **ptr;
            (*ptr)++;
        }
        word[i] = '\0';
        return word_value(word);
    }
    if (**ptr == '[') {
        int head;
        const char *after = parse_list_literal(app, *ptr, &head);
        if (after != NULL) {
            *ptr = after;
            return list_value(head);
        }
        append_output(app, "[ list ]: missing closing ] or too long\n");
        return word_value("");
    }
    if (consume_keyword(ptr, "BUTFIRST")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) {
            return list_value(arg.list_head < 0 ? -1 : app->list_pool[arg.list_head].next);
        }
        if (arg.type == VALUE_WORD) {
            // A word is a sequence of characters, not one atomic element —
            // BUTFIRST "hello is "ello", same substring semantics as real
            // Logo (a list's BUTFIRST works on elements; a word's works on
            // characters).
            return word_value(arg.word[0] == '\0' ? "" : arg.word + 1);
        }
        return list_value(-1); // BUTFIRST of a bare number is empty
    }
    if (consume_keyword(ptr, "FIRST")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) {
            if (arg.list_head < 0) return word_value("");
            return list_node_to_value(&app->list_pool[arg.list_head]);
        }
        if (arg.type == VALUE_WORD) {
            if (arg.word[0] == '\0') return word_value("");
            char ch[2] = {arg.word[0], '\0'};
            return word_value(ch);
        }
        return arg; // FIRST of a bare number is itself
    }
    if (consume_keyword(ptr, "LAST")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) {
            if (arg.list_head < 0) return word_value("");
            int idx = arg.list_head;
            while (app->list_pool[idx].next != -1) idx = app->list_pool[idx].next;
            return list_node_to_value(&app->list_pool[idx]);
        }
        if (arg.type == VALUE_WORD) {
            size_t len = strlen(arg.word);
            if (len == 0) return word_value("");
            char ch[2] = {arg.word[len - 1], '\0'};
            return word_value(ch);
        }
        return arg; // LAST of a bare number is itself
    }
    if (consume_keyword(ptr, "COUNT")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) {
            int count = 0;
            for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
            return number_value(count);
        }
        if (arg.type == VALUE_WORD) {
            return number_value((double)strlen(arg.word));
        }
        return number_value(1); // a bare number counts as a one-element list
    }
    if (consume_keyword(ptr, "BUTLAST")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) {
            return list_butlast(app, arg);
        }
        if (arg.type == VALUE_WORD) {
            size_t len = strlen(arg.word);
            if (len == 0) return word_value("");
            Value result = word_value(arg.word);
            result.word[len - 1] = '\0';
            return result;
        }
        return list_value(-1); // BUTLAST of a bare number is empty
    }
    if (consume_keyword(ptr, "ITEM")) {
        int index = (int)value_to_number(parse_factor(app, ptr));
        Value thing = parse_factor(app, ptr);
        if (thing.type == VALUE_LIST) {
            int i = 1;
            for (int idx = thing.list_head; idx != -1; idx = app->list_pool[idx].next, i++) {
                if (i == index) return list_node_to_value(&app->list_pool[idx]);
            }
            append_output(app, "ITEM: index out of range\n");
            return word_value("");
        }
        if (thing.type == VALUE_WORD) {
            int len = (int)strlen(thing.word);
            if (index < 1 || index > len) {
                append_output(app, "ITEM: index out of range\n");
                return word_value("");
            }
            char ch[2] = {thing.word[index - 1], '\0'};
            return word_value(ch);
        }
        // A bare number counts as a one-element list -- only index 1 is valid.
        if (index == 1) return thing;
        append_output(app, "ITEM: index out of range\n");
        return word_value("");
    }
    if (consume_keyword(ptr, "MEMBER?")) {
        Value thing = parse_factor(app, ptr);
        Value container = parse_factor(app, ptr);
        if (container.type == VALUE_LIST) {
            for (int idx = container.list_head; idx != -1; idx = app->list_pool[idx].next) {
                if (values_equal(app, thing, list_node_to_value(&app->list_pool[idx]))) {
                    return number_value(1);
                }
            }
            return number_value(0);
        }
        if (container.type == VALUE_WORD) {
            // A word's "elements" are its characters -- membership is
            // substring containment, so MEMBER? "ell "hello is true too,
            // not just single-character checks.
            char thing_text[512];
            value_to_text(app, &thing, thing_text, sizeof(thing_text));
            return number_value(strstr(container.word, thing_text) != NULL ? 1 : 0);
        }
        // A bare number counts as a one-element list.
        return number_value(values_equal(app, thing, container) ? 1 : 0);
    }
    if (consume_keyword(ptr, "EMPTY?")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) return number_value(arg.list_head == -1 ? 1 : 0);
        if (arg.type == VALUE_WORD) return number_value(arg.word[0] == '\0' ? 1 : 0);
        return number_value(0); // a number is never "empty"
    }
    if (consume_keyword(ptr, "WORD?")) {
        Value arg = parse_factor(app, ptr);
        return number_value(arg.type == VALUE_WORD ? 1 : 0);
    }
    if (consume_keyword(ptr, "LIST?")) {
        Value arg = parse_factor(app, ptr);
        return number_value(arg.type == VALUE_LIST ? 1 : 0);
    }
    if (consume_keyword(ptr, "NUMBER?")) {
        Value arg = parse_factor(app, ptr);
        return number_value(arg.type == VALUE_NUMBER ? 1 : 0);
    }
    if (consume_keyword(ptr, "FPUT")) {
        Value thing = parse_factor(app, ptr);
        Value list = parse_factor(app, ptr);
        return list_fput(app, thing, list);
    }
    if (consume_keyword(ptr, "LPUT")) {
        Value thing = parse_factor(app, ptr);
        Value list = parse_factor(app, ptr);
        return list_lput(app, thing, list);
    }
    if (consume_keyword(ptr, "WORD")) {
        Value a = parse_factor(app, ptr);
        Value b = parse_factor(app, ptr);
        if (a.type == VALUE_LIST || b.type == VALUE_LIST) {
            append_output(app, "WORD: expected words, not a list\n");
            return word_value("");
        }
        char a_text[512], b_text[512];
        value_to_text(app, &a, a_text, sizeof(a_text));
        value_to_text(app, &b, b_text, sizeof(b_text));
        Value result = {0};
        result.type = VALUE_WORD;
        result.list_head = -1;
        list_word(a_text, b_text, result.word, sizeof(result.word));
        return result;
    }
    if (consume_keyword(ptr, "SENTENCE") || consume_keyword(ptr, "SE")) {
        Value a = parse_factor(app, ptr);
        Value b = parse_factor(app, ptr);
        return list_sentence(app, a, b);
    }
    if (consume_keyword(ptr, "LIST")) {
        Value a = parse_factor(app, ptr);
        Value b = parse_factor(app, ptr);
        return list_wrap_pair(app, a, b);
    }
    if (consume_keyword(ptr, "HEADING")) {
        // The raw stored angle, same convention SETHEADING/RT/LT already
        // use — not normalized to 0-360, since nothing else in the
        // interpreter normalizes it either (RT 720 just keeps adding).
        return number_value(current_turtle(app)->angle);
    }
    if (consume_keyword(ptr, "POS")) {
        // [x y] as a 2-element list, reusing LIST's wrap-as-one-element
        // logic since a raw x/y pair never needs splicing.
        return list_wrap_pair(app, number_value(current_turtle(app)->x), number_value(current_turtle(app)->y));
    }
    if (**ptr == ':') {
        (*ptr)++;
        char name[32] = {0};
        size_t i = 0;
        while (**ptr && (isalnum((unsigned char)**ptr) || **ptr == '_') && i < sizeof(name) - 1) {
            name[i++] = **ptr;
            (*ptr)++;
        }
        name[i] = '\0';
        Variable *v = find_var(app, name);
        if (v != NULL) {
            if (v->type == VALUE_WORD) return word_value(v->word);
            if (v->type == VALUE_LIST) return list_value(v->list_head);
            return number_value(v->number);
        }
        return number_value(0);
    }

    char *end;
    double val = strtod(*ptr, &end);
    *ptr = end;
    return number_value(val);
}

// Parse a sequence of factors joined by * and /. Always numeric: * and /
// coerce both sides via value_to_number, so the result is always a plain
// number even if one side was a word.
static Value parse_term(LogoApp *app, const char **ptr) {
    Value val = parse_factor(app, ptr);
    for (;;) {
        *ptr = skip_whitespace(*ptr);
        if (**ptr == '*') {
            (*ptr)++;
            val = number_value(value_to_number(val) * value_to_number(parse_factor(app, ptr)));
        } else if (**ptr == '/') {
            (*ptr)++;
            double divisor = value_to_number(parse_factor(app, ptr));
            val = number_value(divisor != 0 ? value_to_number(val) / divisor : 0);
        } else {
            break;
        }
    }
    return val;
}

// Parse a sequence of terms joined by + and -. This is the entry point
// for any argument throughout eval_logo — FD's distance, MAKE's value,
// PRINT's argument, procedure-call arguments, comparison operands, and
// every other turtle-command argument. A single factor with no +/-
// following it (a bare "word, [list], list operator/constructor, or
// :word-typed variable) passes through unchanged, still carrying its
// word-ness; + and - are always numeric and coerce both sides (see
// value_to_number) — that coercion is what lets FD FIRST :colors work:
// FIRST :colors alone stays a word, but the moment arithmetic touches it,
// it reads as the number its text starts with (or 0 if it doesn't).
static Value parse_expr(LogoApp *app, const char **ptr) {
    Value val = parse_term(app, ptr);
    for (;;) {
        *ptr = skip_whitespace(*ptr);
        if (**ptr == '+') {
            (*ptr)++;
            val = number_value(value_to_number(val) + value_to_number(parse_term(app, ptr)));
        } else if (**ptr == '-') {
            (*ptr)++;
            val = number_value(value_to_number(val) - value_to_number(parse_term(app, ptr)));
        } else {
            break;
        }
    }
    return val;
}

// Parse a single relational comparison, e.g. :X > 10, :X = :Y, :X = "hi.
// With no relational operator, falls back to the operand's truthiness
// (non-zero number, or non-empty word = true). The base case for
// parse_condition, below.
static double parse_comparison(LogoApp *app, const char **ptr) {
    Value left = parse_expr(app, ptr);
    *ptr = skip_whitespace(*ptr);

    if (**ptr == '<' || **ptr == '>' || **ptr == '=') {
        char op1 = **ptr;
        (*ptr)++;
        char op2 = '\0';
        if ((op1 == '<' && (**ptr == '=' || **ptr == '>')) || (op1 == '>' && **ptr == '=')) {
            op2 = **ptr;
            (*ptr)++;
        }
        Value right = parse_expr(app, ptr);

        if (left.type != VALUE_NUMBER || right.type != VALUE_NUMBER) {
            // Words/lists only support = and <>; a text comparison for
            // the rest wouldn't be meaningful, so they just report
            // unequal.
            int equal = values_equal(app, left, right);
            if (op1 == '=') return equal;
            if (op1 == '<' && op2 == '>') return !equal;
            return 0;
        }

        if (op1 == '=') return left.number == right.number;
        if (op1 == '<' && op2 == '=') return left.number <= right.number;
        if (op1 == '<' && op2 == '>') return left.number != right.number;
        if (op1 == '<') return left.number < right.number;
        if (op1 == '>' && op2 == '=') return left.number >= right.number;
        return left.number > right.number;
    }

    if (left.type == VALUE_LIST) return left.list_head != -1;
    return left.type == VALUE_WORD ? (left.word[0] != '\0') : (left.number != 0);
}

// NOT <bool> | comparison — NOT binds tighter than AND/OR and can nest
// (NOT NOT ...).
static double parse_bool_not(LogoApp *app, const char **ptr) {
    if (consume_keyword(ptr, "NOT")) {
        return parse_bool_not(app, ptr) == 0 ? 1 : 0;
    }
    return parse_comparison(app, ptr);
}

// <bool> (AND <bool>)* — AND binds tighter than OR.
static double parse_bool_and(LogoApp *app, const char **ptr) {
    double val = parse_bool_not(app, ptr);
    while (consume_keyword(ptr, "AND")) {
        double rhs = parse_bool_not(app, ptr);
        val = (val != 0 && rhs != 0) ? 1 : 0;
    }
    return val;
}

// Parse a full boolean condition used by IF/IFELSE/WHILE: comparisons
// combined with NOT/AND/OR, e.g. :X > 0 AND NOT :Y = 0. No parentheses
// for grouping — clauses combine strictly left to right within each
// precedence level (NOT tightest, then AND, then OR).
static double parse_condition(LogoApp *app, const char **ptr) {
    double val = parse_bool_and(app, ptr);
    while (consume_keyword(ptr, "OR")) {
        double rhs = parse_bool_and(app, ptr);
        val = (val != 0 || rhs != 0) ? 1 : 0;
    }
    return val;
}

// Send text to app->output_sink (the real history pane in the GTK app;
// a plain buffer in tests). A no-op if no sink is set.
void append_output(LogoApp *app, const char *text) {
    if (app->output_sink != NULL) {
        app->output_sink(app, text);
    }
}

// --- RECURSIVE INTERPRETER ---

// Tokenize and execute one chunk of Logo source (a REPL line, a
// procedure body, or a REPEAT/IF/WHILE block), recursing for nested
// blocks and procedure calls.
void eval_logo(LogoApp *app, const char *code) {
    const char *ptr = code;

    while (*ptr != '\0') {
        ptr = skip_whitespace(ptr);
        if (*ptr == '\0') break;

        char token[128] = {0};
        int read_bytes = 0;

        if (sscanf(ptr, "%127s%n", token, &read_bytes) != 1) break;
        ptr += read_bytes;

        // 1. PROCEDURE DEFINITION: TO <NAME> [:PARAM ...] ... END
        if (strcasecmp(token, "TO") == 0) {
            char name_buf[32] = {0};

            if (sscanf(ptr, "%31s%n", name_buf, &read_bytes) == 1) {
                ptr += read_bytes;
                ptr = skip_whitespace(ptr);

                // Redefining an existing procedure overwrites it in place
                // (so fixing and re-typing a TO just works); otherwise
                // claim the next free slot if there's room.
                Procedure *proc = find_procedure(app, name_buf);
                if (proc == NULL && app->proc_count < MAX_PROCEDURES) {
                    proc = &app->procedures[app->proc_count++];
                }
                if (proc != NULL) {
                    memset(proc, 0, sizeof(Procedure));
                    snprintf(proc->name, sizeof(proc->name), "%s", name_buf);
                }

                // Zero or more parameters (each starts with ':'). Always
                // consumes every :param token, even past MAX_PARAMS —
                // otherwise excess ones would be left dangling right where
                // the body is about to be captured from, corrupting it
                // (see the "too many parameters" report below instead).
                gboolean too_many_params = FALSE;
                while (*ptr == ':') {
                    char param_buf[32];
                    sscanf(ptr, "%31s%n", param_buf, &read_bytes);
                    ptr += read_bytes;
                    if (proc != NULL) {
                        if (proc->param_count < MAX_PARAMS) {
                            snprintf(proc->param_names[proc->param_count], sizeof(proc->param_names[0]), "%s", param_buf);
                            proc->param_count++;
                        } else {
                            too_many_params = TRUE;
                        }
                    }
                    ptr = skip_whitespace(ptr);
                }
                if (too_many_params) {
                    append_output(app, "TO ");
                    append_output(app, name_buf);
                    append_output(app, ": too many parameters, extra parameters ignored\n");
                }

                // Extract body until END
                const char *end_ptr = strcasestr(ptr, "END");
                if (end_ptr != NULL) {
                    if (proc != NULL) {
                        size_t body_len = end_ptr - ptr;
                        if (body_len < sizeof(proc->body)) {
                            strncpy(proc->body, ptr, body_len);
                            proc->body[body_len] = '\0';
                        } else {
                            append_output(app, "TO ");
                            append_output(app, name_buf);
                            append_output(app, ": procedure body too long, not defined\n");
                        }
                    } else {
                        append_output(app, "Too many procedures defined, TO ignored\n");
                    }
                    ptr = end_ptr + 3; // Advance past "END"
                } else {
                    append_output(app, "TO ");
                    append_output(app, name_buf);
                    append_output(app, ": missing END\n");
                    ptr += strlen(ptr); // stop, rather than running the dangling body as top-level commands
                }
            }
        }
        // 1b. PROCEDURE DELETION: ERASE "name
        else if (strcasecmp(token, "ERASE") == 0) {
            char name_buf[64] = {0};
            if (sscanf(ptr, "%63s%n", name_buf, &read_bytes) == 1 && name_buf[0] == '"') {
                ptr += read_bytes;
                Procedure *proc = find_procedure(app, name_buf + 1);
                if (proc != NULL) {
                    int index = (int)(proc - app->procedures);
                    for (int i = index; i < app->proc_count - 1; i++) {
                        app->procedures[i] = app->procedures[i + 1];
                    }
                    app->proc_count--;
                } else {
                    append_output(app, "ERASE: no such procedure \"");
                    append_output(app, name_buf + 1);
                    append_output(app, "\n");
                }
            } else {
                append_output(app, "ERASE: expected a \"name\n");
            }
        }
        // 1c. LOAD "path — read a file and run it as Logo source
        else if (strcasecmp(token, "LOAD") == 0) {
            char path_buf[512] = {0};
            if (sscanf(ptr, "%511s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
                ptr += read_bytes;

                char *contents = NULL;
                GError *error = NULL;
                if (g_file_get_contents(path_buf + 1, &contents, NULL, &error)) {
                    eval_logo(app, contents);
                    g_free(contents);
                } else {
                    append_output(app, "LOAD: could not read file\n");
                    g_error_free(error);
                }
            } else {
                append_output(app, "LOAD: expected a \"path\n");
            }
        }
        // 1d. SAVE "path — write all defined procedures out as Logo source
        else if (strcasecmp(token, "SAVE") == 0) {
            char path_buf[512] = {0};
            if (sscanf(ptr, "%511s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
                ptr += read_bytes;

                char *content = serialize_procedures(app);
                GError *error = NULL;
                if (g_file_set_contents(path_buf + 1, content, -1, &error)) {
                    append_output(app, "Saved ");
                    append_output(app, path_buf + 1);
                    append_output(app, "\n");
                } else {
                    append_output(app, "SAVE: could not write file\n");
                    g_error_free(error);
                }
                g_free(content);
            } else {
                append_output(app, "SAVE: expected a \"path\n");
            }
        }
        // 2. REPEAT LOOPS
        else if (strcasecmp(token, "REPEAT") == 0) {
            int count = (int)value_to_number(parse_expr(app, &ptr));
            char block_body[4096];
            const char *after_block = extract_block(ptr, block_body, sizeof(block_body));

            if (after_block != NULL) {
                ptr = after_block;
                for (int i = 0; i < count; i++) {
                    eval_logo(app, block_body);
                }
            } else {
                append_output(app, "REPEAT: expected [ block ]\n");
            }
        }
        // 2b. WHILE LOOPS: WHILE <cond> [block]
        else if (strcasecmp(token, "WHILE") == 0) {
            const char *cond_start = ptr;
            double cond = parse_condition(app, &ptr);
            size_t cond_len = (size_t)(ptr - cond_start);

            char cond_text[1024] = {0};
            if (cond_len >= sizeof(cond_text)) cond_len = sizeof(cond_text) - 1;
            memcpy(cond_text, cond_start, cond_len);
            cond_text[cond_len] = '\0';

            char block_body[4096] = {0};
            const char *after_block = extract_block(ptr, block_body, sizeof(block_body));

            if (after_block != NULL) {
                ptr = after_block;
                int iterations = 0;
                while (cond != 0) {
                    if (iterations >= MAX_WHILE_ITERATIONS) {
                        append_output(app, "WHILE: stopped after too many iterations\n");
                        break;
                    }
                    eval_logo(app, block_body);
                    const char *cptr = cond_text;
                    cond = parse_condition(app, &cptr);
                    iterations++;
                }
            } else {
                append_output(app, "WHILE: expected [ block ]\n");
            }
        }
        // 3. BASIC TURTLE COMMANDS
        else if (strcasecmp(token, "FORWARD") == 0 || strcasecmp(token, "FD") == 0) {
            double val = value_to_number(parse_expr(app, &ptr));
            move_turtle_forward(app, val);
        }
        else if (strcasecmp(token, "BACK") == 0 || strcasecmp(token, "BK") == 0) {
            double val = value_to_number(parse_expr(app, &ptr));
            move_turtle_forward(app, -val);
        }
        else if (strcasecmp(token, "RIGHT") == 0 || strcasecmp(token, "RT") == 0) {
            double val = value_to_number(parse_expr(app, &ptr));
            current_turtle(app)->angle += val;
        }
        else if (strcasecmp(token, "LEFT") == 0 || strcasecmp(token, "LT") == 0) {
            double val = value_to_number(parse_expr(app, &ptr));
            current_turtle(app)->angle -= val;
        }
        else if (strcasecmp(token, "SETXY") == 0) {
            double x = value_to_number(parse_expr(app, &ptr));
            double y = value_to_number(parse_expr(app, &ptr));
            move_turtle_to(app, x, y);
        }
        else if (strcasecmp(token, "SETHEADING") == 0 || strcasecmp(token, "SETH") == 0) {
            current_turtle(app)->angle = value_to_number(parse_expr(app, &ptr));
        }
        // 3a'. ARC angle radius — draws a circle of `radius` centered ON
        // the turtle, starting at its current heading and sweeping
        // through `angle` degrees. The turtle itself doesn't move.
        else if (strcasecmp(token, "ARC") == 0) {
            double angle_deg = value_to_number(parse_expr(app, &ptr));
            double radius = value_to_number(parse_expr(app, &ptr));

            double center_x = current_turtle(app)->x;
            double center_y = current_turtle(app)->y;
            double start_heading = current_turtle(app)->angle;

            int segments = (int)clamp_range(fabs(angle_deg) / 5.0, 8, 360);
            double step = angle_deg / segments;

            for (int i = 0; i < segments; i++) {
                double rad0 = (start_heading + step * i - 90.0) * M_PI / 180.0;
                double rad1 = (start_heading + step * (i + 1) - 90.0) * M_PI / 180.0;
                record_line(app,
                            center_x + radius * cos(rad0), center_y + radius * sin(rad0),
                            center_x + radius * cos(rad1), center_y + radius * sin(rad1));
            }
        }
        // 3a''. TELL n — switch which turtle FD/RT/etc. control, creating
        // it (at the default state) the first time it's addressed.
        else if (strcasecmp(token, "TELL") == 0) {
            int index = (int)value_to_number(parse_expr(app, &ptr));
            if (index < 0 || index >= MAX_TURTLES) {
                char msg[64];
                snprintf(msg, sizeof(msg), "TELL: turtle index must be 0-%d\n", MAX_TURTLES - 1);
                append_output(app, msg);
            } else {
                if (index >= app->turtle_count) {
                    for (int i = app->turtle_count; i <= index; i++) {
                        init_turtle(&app->turtles[i]);
                    }
                    app->turtle_count = index + 1;
                }
                app->current_turtle = index;
            }
        }
        // 3b. VARIABLES: MAKE "name expr — expr is any expression, word,
        // or list, so this one call handles MAKE "name "word, MAKE "name
        // [some words], MAKE "name :other, and every list operator/
        // constructor, in addition to plain numeric expressions.
        else if (strcasecmp(token, "MAKE") == 0) {
            char varname[64] = {0};
            if (sscanf(ptr, "%63s%n", varname, &read_bytes) == 1 && varname[0] == '"') {
                ptr += read_bytes;
                Value val = parse_expr(app, &ptr);
                if (val.type == VALUE_WORD) {
                    set_var_word(app, varname + 1, val.word);
                } else if (val.type == VALUE_LIST) {
                    set_var_list(app, varname + 1, val.list_head);
                } else {
                    set_var(app, varname + 1, val.number);
                }
            } else {
                append_output(app, "MAKE: expected a \"name\n");
            }
        }
        // 3b'. LOCAL "name — a variable scoped to the current call,
        // without being a parameter (see Variables & scoping below).
        // Only valid inside an active procedure call; the scope's vars
        // array shares its fixed capacity (MAX_PARAMS) with real
        // parameters, so a procedure with many parameters may not have
        // room left for additional LOCALs.
        else if (strcasecmp(token, "LOCAL") == 0) {
            char varname[64] = {0};
            if (sscanf(ptr, "%63s%n", varname, &read_bytes) == 1 && varname[0] == '"') {
                ptr += read_bytes;
                if (app->scope_depth <= 0) {
                    append_output(app, "LOCAL: can only be used inside a procedure\n");
                } else {
                    Scope *scope = &app->scopes[app->scope_depth - 1];
                    gboolean already_local = FALSE;
                    for (int i = 0; i < scope->count; i++) {
                        if (strcasecmp(scope->vars[i].name, varname + 1) == 0) {
                            already_local = TRUE;
                            break;
                        }
                    }
                    if (!already_local) {
                        if (scope->count < MAX_PARAMS) {
                            Variable *v = &scope->vars[scope->count++];
                            snprintf(v->name, sizeof(v->name), "%s", varname + 1);
                            v->type = VALUE_NUMBER;
                            v->number = 0;
                        } else {
                            append_output(app, "LOCAL: too many local variables\n");
                        }
                    }
                }
            } else {
                append_output(app, "LOCAL: expected a \"name\n");
            }
        }
        // 3c. CONDITIONALS: IF <cond> [block] (ELSE [block])   or   IFELSE <cond> [block] [block]
        else if (strcasecmp(token, "IF") == 0 || strcasecmp(token, "IFELSE") == 0) {
            double cond = parse_condition(app, &ptr);

            char true_body[4096] = {0};
            const char *after_true = extract_block(ptr, true_body, sizeof(true_body));

            if (after_true != NULL) {
                ptr = after_true;
                char false_body[4096] = {0};
                int has_false = 0;

                const char *lookahead = skip_whitespace(ptr);
                char maybe_else[16] = {0};
                int else_bytes = 0;
                if (sscanf(lookahead, "%15s%n", maybe_else, &else_bytes) == 1 &&
                    strcasecmp(maybe_else, "ELSE") == 0) {
                    const char *after_false = extract_block(skip_whitespace(lookahead + else_bytes),
                                                              false_body, sizeof(false_body));
                    if (after_false != NULL) {
                        ptr = after_false;
                        has_false = 1;
                    }
                } else {
                    const char *after_false = extract_block(lookahead, false_body, sizeof(false_body));
                    if (after_false != NULL) {
                        ptr = after_false;
                        has_false = 1;
                    }
                }

                if (strcasecmp(token, "IFELSE") == 0 && !has_false) {
                    append_output(app, "IFELSE: expected two [ block ]s\n");
                } else if (cond != 0) {
                    eval_logo(app, true_body);
                } else if (has_false) {
                    eval_logo(app, false_body);
                }
            } else {
                append_output(app, token);
                append_output(app, ": expected [ block ]\n");
            }
        }
        else if (strcasecmp(token, "CLEAR") == 0 || strcasecmp(token, "CS") == 0) {
            app->line_count = 0;
            for (int i = 0; i < app->turtle_count; i++) {
                app->turtles[i].x = HOME_X;
                app->turtles[i].y = HOME_Y;
                app->turtles[i].angle = 0;
            }
        }
        else if (strcasecmp(token, "HOME") == 0) {
            move_turtle_to(app, HOME_X, HOME_Y);
            current_turtle(app)->angle = 0;
        }
        else if (strcasecmp(token, "PENUP") == 0 || strcasecmp(token, "PU") == 0) {
            current_turtle(app)->pen_down = 0;
        }
        else if (strcasecmp(token, "PENDOWN") == 0 || strcasecmp(token, "PD") == 0) {
            current_turtle(app)->pen_down = 1;
        }
        // 3c'. SETPENCOLOR r g b — each channel 0-255, applies to lines drawn from now on
        else if (strcasecmp(token, "SETPENCOLOR") == 0 || strcasecmp(token, "SETPC") == 0) {
            double r = value_to_number(parse_expr(app, &ptr));
            double g = value_to_number(parse_expr(app, &ptr));
            double b = value_to_number(parse_expr(app, &ptr));
            Turtle *t = current_turtle(app);
            t->pen_r = clamp01(r / 255.0);
            t->pen_g = clamp01(g / 255.0);
            t->pen_b = clamp01(b / 255.0);
        }
        // 3c''a. SETPENWIDTH width — clamped to [0.5, 20], applies to lines drawn from now on
        else if (strcasecmp(token, "SETPENWIDTH") == 0 || strcasecmp(token, "SETPW") == 0) {
            double width = value_to_number(parse_expr(app, &ptr));
            current_turtle(app)->pen_width = clamp_range(width, MIN_PEN_WIDTH, MAX_PEN_WIDTH);
        }
        // 3c''. SETBACKGROUND r g b — each channel 0-255, the canvas's background color
        else if (strcasecmp(token, "SETBACKGROUND") == 0 || strcasecmp(token, "SETBG") == 0) {
            double r = value_to_number(parse_expr(app, &ptr));
            double g = value_to_number(parse_expr(app, &ptr));
            double b = value_to_number(parse_expr(app, &ptr));
            app->bg_r = clamp01(r / 255.0);
            app->bg_g = clamp01(g / 255.0);
            app->bg_b = clamp01(b / 255.0);
        }
        // 3d. OUTPUT: PRINT <expr> — expr is any expression, word, or
        // list, exactly like MAKE's above (PRINT "word, PRINT [list of
        // words], PRINT FIRST :colors, PRINT 1 + 2, ...).
        else if (strcasecmp(token, "PRINT") == 0 || strcasecmp(token, "PR") == 0) {
            Value val = parse_expr(app, &ptr);
            char line[512];
            value_to_text(app, &val, line, sizeof(line));
            append_output(app, line);
            append_output(app, "\n");
        }
        // 4. USER-DEFINED PROCEDURE CALL
        else {
            Procedure *proc = find_procedure(app, token);
            if (proc != NULL) {
                // Evaluate each argument in the *caller's* scope, before
                // pushing the callee's new one. Parameters stay purely
                // numeric (a word argument coerces the same as anywhere
                // else arithmetic touches one — see value_to_number).
                double arg_vals[MAX_PARAMS];
                for (int p = 0; p < proc->param_count; p++) {
                    arg_vals[p] = value_to_number(parse_expr(app, &ptr));
                }

                if (app->scope_depth >= MAX_SCOPE_DEPTH) {
                    append_output(app, "Recursion too deep, call ignored\n");
                } else {
                    // Bind parameters as locals in a fresh scope so they
                    // shadow same-named variables from outer calls or
                    // globals, then run the unmodified procedure body.
                    Scope *scope = &app->scopes[app->scope_depth];
                    scope->count = proc->param_count;
                    for (int p = 0; p < proc->param_count; p++) {
                        // param_names are stored with their leading ':'; strip it.
                        snprintf(scope->vars[p].name, sizeof(scope->vars[p].name), "%s", proc->param_names[p] + 1);
                        scope->vars[p].type = VALUE_NUMBER;
                        scope->vars[p].number = arg_vals[p];
                    }
                    app->scope_depth++;

                    eval_logo(app, proc->body);

                    app->scope_depth--;
                }
            } else {
                append_output(app, "I don't know how to ");
                append_output(app, token);
                append_output(app, "\n");
            }
        }
    }
}

// --- REPL INPUT COMPLETENESS ---

// An input is ready to run once its brackets are balanced and every TO has
// a matching END — otherwise Enter should just add a line and keep going.
gboolean is_input_complete(const char *text) {
    int bracket_depth = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '[') bracket_depth++;
        else if (*p == ']') bracket_depth--;
    }
    if (bracket_depth > 0) return FALSE;

    int to_count = 0, end_count = 0;
    const char *p = text;
    while (*p) {
        p = skip_whitespace(p);
        if (*p == '\0') break;
        char word[64] = {0};
        int n = 0;
        if (sscanf(p, "%63s%n", word, &n) != 1 || n == 0) break;
        if (strcasecmp(word, "TO") == 0) to_count++;
        else if (strcasecmp(word, "END") == 0) end_count++;
        p += n;
    }
    return to_count <= end_count;
}

// --- SAVING ---

// Build a Logo-source rendering of every currently-defined procedure
// (each as TO ... END), readable back in by LOAD or File > Open.
char *serialize_procedures(LogoApp *app) {
    GString *out = g_string_new(NULL);
    for (int i = 0; i < app->proc_count; i++) {
        Procedure *proc = &app->procedures[i];
        g_string_append(out, "TO ");
        g_string_append(out, proc->name);
        for (int p = 0; p < proc->param_count; p++) {
            g_string_append_c(out, ' ');
            g_string_append(out, proc->param_names[p]); // already includes leading ':'
        }
        g_string_append_c(out, '\n');
        g_string_append(out, proc->body);
        g_string_append(out, "\nEND\n\n");
    }
    return g_string_free(out, FALSE);
}
