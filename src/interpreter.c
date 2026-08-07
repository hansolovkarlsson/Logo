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

// Where CLEAR and HOME send the turtle back to -- the canvas's current
// center, not a fixed point, now that SETCANVASSIZE can change
// canvas_width/height at runtime.
static double home_x(LogoApp *app) { return app->canvas_width / 2.0; }
static double home_y(LogoApp *app) { return app->canvas_height / 2.0; }

// The turtle currently being controlled by FD/RT/SETXY/etc. — see TELL.
static Turtle* current_turtle(LogoApp *app) {
    return &app->turtles[app->current_turtle];
}

// Reset `t` to the default turtle state: home position, heading 0, pen
// down, default color/width, visible.
void init_turtle(LogoApp *app, Turtle *t) {
    *t = (Turtle){
        .x = home_x(app), .y = home_y(app), .angle = 0, .pen_down = 1,
        .pen_r = 0.1, .pen_g = 0.1, .pen_b = 0.1, .pen_width = 2.0,
        .visible = 1, .sprite_index = -1, .sprite_frame = 0,
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

// Wrap a coordinate into [0, size) — used by WRAP mode.
static double wrap_coord(double v, double size) {
    double wrapped = fmod(v, size);
    if (wrapped < 0) wrapped += size;
    return wrapped;
}

// Move the current turtle directly to an absolute position, recording a
// line segment along the way if the pen is down. Applies the current
// edge mode (see WRAP/FENCE/WINDOW): WINDOW (the default) does nothing
// extra here, same as before edge modes existed.
static void move_turtle_to(LogoApp *app, double new_x, double new_y) {
    Turtle *t = current_turtle(app);

    if (app->edge_mode == EDGE_WRAP) {
        double wrapped_x = wrap_coord(new_x, app->canvas_width);
        double wrapped_y = wrap_coord(new_y, app->canvas_height);
        if (wrapped_x != new_x || wrapped_y != new_y) {
            // The pen lifts across the wrap itself -- no line drawn for
            // this jump, rather than a diagonal line teleporting across
            // the canvas from one edge to the other.
            t->x = wrapped_x;
            t->y = wrapped_y;
            return;
        }
    } else if (app->edge_mode == EDGE_FENCE) {
        if (new_x < 0 || new_x > app->canvas_width || new_y < 0 || new_y > app->canvas_height) {
            new_x = clamp_range(new_x, 0, app->canvas_width);
            new_y = clamp_range(new_y, 0, app->canvas_height);
            append_output(app, "FENCE: turtle stopped at the canvas edge\n");
        }
    }

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

// Appends one procedure's "TO name :params\n<body>\nEND\n" rendering to
// `out` — the shared text SAVE (serialize_procedures, below) writes for
// every procedure to a file, and SHOW writes to the history pane for
// just the one procedure asked for.
static void append_procedure_text(GString *out, Procedure *proc) {
    g_string_append(out, "TO ");
    g_string_append(out, proc->name);
    for (int p = 0; p < proc->param_count; p++) {
        g_string_append_c(out, ' ');
        g_string_append(out, proc->param_names[p]); // already includes leading ':'
    }
    g_string_append_c(out, '\n');
    g_string_append(out, proc->body);
    g_string_append(out, "\nEND\n");
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

// Set a variable to an array (MAKE "name ARRAY n / another array
// variable's value), same binding rules as set_var. Unlike
// set_var_list, this is a genuine alias to *mutable* shared storage:
// MAKE "b :a then SETITEM 1 :b 99 also changes what :a sees, since both
// variables now point at the same list_pool cells -- arrays are a
// deliberate, documented exception to this language's otherwise
// immutable values (see the ARRAY comment in parse_factor).
static void set_var_array(LogoApp *app, const char *name, int start, int length) {
    Variable *v = find_or_create_var(app, name);
    if (v != NULL) {
        v->type = VALUE_ARRAY;
        v->list_head = start;
        v->number = length;
    }
}

// Find a stored SETPROP entry by plist name + property key (case-
// insensitive, same convention as find_var), or -1 if no such property
// has been set. Used by SETPROP (to know whether to overwrite an
// existing entry or allocate a new one), GETPROP, and REMOVEPROP.
static int find_plist_entry(LogoApp *app, const char *plist_name, const char *key) {
    for (int i = 0; i < app->plist_entry_count; i++) {
        if (strcasecmp(app->plist_entries[i].plist_name, plist_name) == 0 &&
            strcasecmp(app->plist_entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
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
    double number;   // type == VALUE_ARRAY: the array's length instead
    char word[512];
    int list_head; // type == VALUE_LIST: index into app->list_pool, -1 = empty
                    // type == VALUE_ARRAY: start index of `number` contiguous cells
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

static Value array_value(int start, int length) {
    Value v = {0};
    v.type = VALUE_ARRAY;
    v.list_head = start;
    v.number = length;
    return v;
}

// TRUE/FALSE, as words rather than a distinct value type -- see the
// TRUE/FALSE keyword literals and the type predicates (WORD?/LIST?/
// NUMBER?/EMPTY?/MEMBER?) in parse_factor, and is_truthy below.
static Value bool_value(gboolean b) {
    return word_value(b ? "TRUE" : "FALSE");
}

// A value's truthiness for IF/WHILE and parse_comparison's no-relational-
// -operator fallback: a number is true iff nonzero, a list iff non-empty,
// and a word iff it's non-empty AND not literally the word FALSE (case-
// insensitive, like every other keyword match here) -- the one special
// case, so that WORD?/LIST?/NUMBER?/EMPTY?/MEMBER? (which now return
// TRUE/FALSE words instead of 1/0) still work correctly in IF/WHILE,
// e.g. IF EMPTY? :x [...] must not run just because the word "FALSE"
// happens to be non-empty text.
static gboolean is_truthy(Value v) {
    if (v.type == VALUE_LIST) return v.list_head != -1;
    if (v.type == VALUE_WORD) return v.word[0] != '\0' && strcasecmp(v.word, "FALSE") != 0;
    // Falls through here for VALUE_NUMBER and VALUE_ARRAY alike: a
    // number is true iff nonzero, and an array's `number` field holds
    // its length (see array_value) -- always >= 1 by construction (see
    // ARRAY's size check in parse_factor), so this is always true for a
    // real array without needing its own branch.
    return v.number != 0;
}

// Stash/recover a Value in LogoApp's plain output_* fields (see the
// comment there) — used by OUTPUT to hand a value up to whichever
// call_procedure is waiting for it, without LogoApp needing to know
// about this file's private Value type.
static void store_output_value(LogoApp *app, Value v) {
    app->output_type = v.type;
    app->output_number = v.number;
    snprintf(app->output_word, sizeof(app->output_word), "%s", v.word);
    app->output_list_head = v.list_head;
    app->has_output_value = TRUE;
}

static Value load_output_value(LogoApp *app) {
    Value v = {0};
    v.type = app->output_type;
    v.number = app->output_number;
    snprintf(v.word, sizeof(v.word), "%s", app->output_word);
    v.list_head = app->output_list_head;
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

// Forward-declared: list_elements_to_text below recurses into a nested
// LIST_ELEM_LIST element via this, and array_elements_to_text (also
// below) shares the same per-node rendering for its own cells.
static void list_elements_to_text(LogoApp *app, int head, char *out, size_t out_size);

// Append one list/array cell's rendering to `out` -- shared by
// list_elements_to_text (walks a chain via node->next) and
// array_elements_to_text (walks a contiguous index range instead), so
// the two storage shapes still render each element identically.
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
        append_node_text(app, &app->list_pool[idx], out, out_size);
    }
}

// Render an array's `length` cells (starting at `start`) into `out`,
// space-separated and enclosed in { } -- unlike a list, an array's own
// braces show even as PRINT's top-level value, since real Logo prints
// arrays distinctly from lists to keep the two concrete types visually
// distinguishable.
static void array_elements_to_text(LogoApp *app, int start, int length, char *out, size_t out_size) {
    out[0] = '\0';
    strncat(out, "{", out_size - strlen(out) - 1);
    for (int i = 0; i < length; i++) {
        if (i > 0) strncat(out, " ", out_size - strlen(out) - 1);
        append_node_text(app, &app->list_pool[start + i], out, out_size);
    }
    strncat(out, "}", out_size - strlen(out) - 1);
}

// Render a Value as text: a word as-is, a number formatted the same way
// PRINT shows it, a list via list_elements_to_text above, an array via
// array_elements_to_text.
static void value_to_text(LogoApp *app, const Value *v, char *out, size_t out_size) {
    if (v->type == VALUE_WORD) {
        snprintf(out, out_size, "%s", v->word);
    } else if (v->type == VALUE_LIST) {
        list_elements_to_text(app, v->list_head, out, out_size);
    } else if (v->type == VALUE_ARRAY) {
        array_elements_to_text(app, v->list_head, (int)v->number, out, out_size);
    } else {
        snprintf(out, out_size, "%g", v->number);
    }
}

// Like value_to_text, but always brackets a list value — for when the
// rendered text is about to be *re-parsed* as Logo source rather than
// shown to a person (MAP/FILTER/FOREACH/REDUCE substituting an element
// into a template, which might itself be a sublist). PRINT/RUN want
// value_to_text's "never bracket the top-level value" convention; this
// is for the opposite case, where a substituted list needs to look like
// literal [...] syntax so parse_factor's own `[` branch reconstructs it
// as one value again, instead of spilling its elements as separate
// tokens into the surrounding template.
static void value_to_source_text(LogoApp *app, const Value *v, char *out, size_t out_size) {
    if (v->type == VALUE_LIST) {
        char inner[512];
        list_elements_to_text(app, v->list_head, inner, sizeof(inner));
        snprintf(out, out_size, "[%s]", inner);
    } else {
        value_to_text(app, v, out, out_size);
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

// Build `out` by copying `template_text` token by token, replacing any
// token that's exactly `placeholder` with `replacement` — used by MAP/
// FILTER/FOREACH/REDUCE's `[template with ? in it]` argument. Only a
// *standalone* placeholder token is replaced (never part of a longer
// word), same "only at a token boundary" rule comments/quoting already
// use elsewhere in this file.
static void substitute_placeholder(const char *template_text, const char *placeholder, const char *replacement, char *out, size_t out_size) {
    out[0] = '\0';
    const char *p = template_text;
    gboolean first = TRUE;
    while (*p) {
        p = skip_whitespace(p);
        if (!*p) break;
        char token[512] = {0};
        int n = 0;
        if (sscanf(p, "%511s%n", token, &n) != 1 || n == 0) break;
        if (!first) strncat(out, " ", out_size - strlen(out) - 1);
        first = FALSE;
        strncat(out, strcmp(token, placeholder) == 0 ? replacement : token, out_size - strlen(out) - 1);
        p += n;
    }
}

// Reported when a list-construction operator runs out of pool space —
// same "loud error, not silent corruption" policy as every other fixed
// buffer here.
static Value list_pool_exhausted_error(LogoApp *app) {
    append_output(app, "list storage full, list operation ignored\n");
    return word_value("");
}

static Value parse_expr(LogoApp *app, const char **ptr);
static double parse_condition(LogoApp *app, const char **ptr);
static inline __attribute__((always_inline)) Value call_procedure(LogoApp *app, Procedure *proc, double *arg_vals, gboolean *did_output);

// Substitute `element` for every "?" in `template_text`, then parse the
// result as an expression — the operator form MAP/REDUCE share (FOREACH
// is the same idea but runs the substituted text as a command via
// eval_logo instead, since it's for side effects, not a value).
static Value apply_template_expr(LogoApp *app, const char *template_text, Value element) {
    char el_text[512], expr_text[512];
    value_to_source_text(app, &element, el_text, sizeof(el_text));
    substitute_placeholder(template_text, "?", el_text, expr_text, sizeof(expr_text));
    const char *expr_ptr = expr_text;
    return parse_expr(app, &expr_ptr);
}

// Same substitution as apply_template_expr, but parses the result as a
// *condition* (comparisons, AND/OR/NOT) instead of a plain expression —
// FILTER's template is a predicate (e.g. [? > 2]), and parse_expr alone
// doesn't understand comparison operators at all: it would silently stop
// at the leading number and ignore the rest, making every element look
// truthy.
static double apply_template_condition(LogoApp *app, const char *template_text, Value element) {
    char el_text[512], expr_text[512];
    value_to_source_text(app, &element, el_text, sizeof(el_text));
    substitute_placeholder(template_text, "?", el_text, expr_text, sizeof(expr_text));
    const char *expr_ptr = expr_text;
    return parse_condition(app, &expr_ptr);
}

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

// Fills out[0..2] with a list's first three elements' numeric values
// (each coerced the same way any other numeric context coerces a
// word/number/list element) and returns TRUE only if the list has
// *exactly* three elements — CROSS's "same length" check, since a 3D
// cross product is only defined for 3-element vectors.
static gboolean list_as_three_numbers(LogoApp *app, Value v, double out[3]) {
    if (v.type != VALUE_LIST) return FALSE;
    int n = 0;
    for (int idx = v.list_head; idx != -1; idx = app->list_pool[idx].next, n++) {
        if (n < 3) out[n] = value_to_number(list_node_to_value(&app->list_pool[idx]));
    }
    return n == 3;
}

// Same as list_as_three_numbers, but for a 2-element [x y] point -- used
// by DISTANCE/TOWARDS, which both work in the same [x y] point
// convention POS already returns.
static gboolean list_as_two_numbers(LogoApp *app, Value v, double out[2]) {
    if (v.type != VALUE_LIST) return FALSE;
    int n = 0;
    for (int idx = v.list_head; idx != -1; idx = app->list_pool[idx].next, n++) {
        if (n < 2) out[n] = value_to_number(list_node_to_value(&app->list_pool[idx]));
    }
    return n == 2;
}

// Tokenizes `text` by whitespace into a list of words -- the shared
// core of PARSE (any value's printed text) and TEXT (a procedure's own
// raw source body). Pure text split, not bracket- or quote-aware: a
// "quoted word or a [bracketed block] in the source keeps its
// punctuation as literal characters glued onto whichever token it's
// part of, rather than being reinterpreted the way eval_logo's own
// reader would. Sets *out_head and returns TRUE, or returns FALSE if
// the pool fills up partway through.
static gboolean list_tokenize_words(LogoApp *app, const char *text, int *out_head) {
    int head = -1;
    int *next_slot = &head;
    const char *p = text;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        int node = list_alloc_node(app);
        if (node < 0) return FALSE;
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
    return TRUE;
}

// FLATTEN: recursively collects every leaf (number/word) reachable from
// `list_head`'s chain into one flat chain, appended via *next_slot --
// only meaningful now that lists really nest (see the ListNode comment
// above). A sublist's own container node is discarded; only its leaves
// survive, in order, at the same level as everything else. Returns
// FALSE if the pool fills up partway through (the caller reports the
// error; whatever was appended before the failure is simply abandoned).
static gboolean list_flatten_into(LogoApp *app, int list_head, int **next_slot) {
    for (int idx = list_head; idx != -1; idx = app->list_pool[idx].next) {
        ListNode *node = &app->list_pool[idx];
        if (node->type == LIST_ELEM_LIST) {
            if (!list_flatten_into(app, node->sublist_head, next_slot)) return FALSE;
        } else {
            int new_idx = list_alloc_node(app);
            if (new_idx < 0) return FALSE;
            app->list_pool[new_idx] = *node;
            app->list_pool[new_idx].next = -1;
            **next_slot = new_idx;
            *next_slot = &app->list_pool[new_idx].next;
        }
    }
    return TRUE;
}

// SUBST: rebuilds `list_head`'s chain, replacing every element equal to
// `old_val` (compared the same way MEMBER?/values_equal already do, so
// a whole matching sublist substitutes as one unit too, not just a
// leaf) with `new_val`. A non-matching sublist is recursed into and
// rebuilt in place, so a nested occurrence substitutes without
// disturbing the rest of that sublist. Sets *out_head and returns TRUE,
// or returns FALSE if the pool fills up partway through.
static gboolean list_subst_into(LogoApp *app, int list_head, Value old_val, Value new_val, int *out_head) {
    int head = -1;
    int *next_slot = &head;
    for (int idx = list_head; idx != -1; idx = app->list_pool[idx].next) {
        ListNode *node = &app->list_pool[idx];
        Value elem = list_node_to_value(node);
        int new_idx;
        if (values_equal(app, elem, old_val)) {
            new_idx = list_node_from_value(app, new_val);
        } else if (node->type == LIST_ELEM_LIST) {
            int sub_head;
            if (!list_subst_into(app, node->sublist_head, old_val, new_val, &sub_head)) return FALSE;
            new_idx = list_alloc_node(app);
            if (new_idx >= 0) {
                app->list_pool[new_idx].type = LIST_ELEM_LIST;
                app->list_pool[new_idx].sublist_head = sub_head;
            }
        } else {
            new_idx = list_node_from_value(app, elem);
        }
        if (new_idx < 0) return FALSE;
        app->list_pool[new_idx].next = -1;
        *next_slot = new_idx;
        next_slot = &app->list_pool[new_idx].next;
    }
    *out_head = head;
    return TRUE;
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
    if (consume_keyword(ptr, "TAN")) {
        return number_value(tan(value_to_number(parse_factor(app, ptr)) * M_PI / 180.0));
    }
    if (consume_keyword(ptr, "ASIN")) {
        return number_value(asin(value_to_number(parse_factor(app, ptr))) * 180.0 / M_PI);
    }
    if (consume_keyword(ptr, "ACOS")) {
        return number_value(acos(value_to_number(parse_factor(app, ptr))) * 180.0 / M_PI);
    }
    if (consume_keyword(ptr, "LOG")) {
        return number_value(log10(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "LN")) {
        return number_value(log(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "EXP")) {
        return number_value(exp(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "RANDOM")) {
        return number_value(random_below(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "ROUND")) {
        return number_value(round(value_to_number(parse_factor(app, ptr))));
    }
    if (consume_keyword(ptr, "INT")) {
        return number_value(trunc(value_to_number(parse_factor(app, ptr))));
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
        if (arg.type == VALUE_ARRAY) {
            return number_value(arg.number); // an array's `number` holds its length
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
    // ARRAY size — a fixed-size, directly-indexed sibling to lists:
    // `size` contiguous cells in app->list_pool (each defaulting to an
    // empty list, matching real Logo's ARRAY), reached by index math
    // (list_pool[start + i]) instead of walking a next-pointer chain --
    // that's the whole reason to reach for ARRAY over a list, since
    // ITEM on a list is an O(n) walk. Unlike every other value in this
    // language, an array is *mutable*: SETITEM changes a cell in place,
    // and MAKE "b :a aliases the same cells :a has rather than copying
    // them, so SETITEM through either variable is visible from both
    // (see set_var_array). This can't be passed into a user-defined
    // procedure's parameter today -- like lists and words, an argument
    // is always coerced to a plain number at the call boundary (see
    // docs/ROADMAP.md's Robustness section).
    if (consume_keyword(ptr, "ARRAY")) {
        int size = (int)value_to_number(parse_factor(app, ptr));
        if (size < 1) {
            append_output(app, "ARRAY: size must be at least 1\n");
            return word_value("");
        }
        int start = app->list_pool_count;
        for (int i = 0; i < size; i++) {
            int idx = list_alloc_node(app);
            if (idx < 0) return list_pool_exhausted_error(app);
            app->list_pool[idx].type = LIST_ELEM_LIST;
            app->list_pool[idx].sublist_head = -1;
            app->list_pool[idx].next = -1; // unused for arrays -- kept tidy, not chain-walked
        }
        return array_value(start, size);
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
        if (thing.type == VALUE_ARRAY) {
            // Direct index math, not a chain walk -- the whole point of
            // an array over a list (see the ARRAY comment below).
            if (index < 1 || index > (int)thing.number) {
                append_output(app, "ITEM: index out of range\n");
                return word_value("");
            }
            return list_node_to_value(&app->list_pool[thing.list_head + (index - 1)]);
        }
        // A bare number counts as a one-element list -- only index 1 is valid.
        if (index == 1) return thing;
        append_output(app, "ITEM: index out of range\n");
        return word_value("");
    }
    if (consume_keyword(ptr, "PICK")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) {
            int count = 0;
            for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next) count++;
            if (count == 0) {
                append_output(app, "PICK: empty list\n");
                return word_value("");
            }
            int target = (int)random_below(count);
            int i = 0;
            for (int idx = arg.list_head; idx != -1; idx = app->list_pool[idx].next, i++) {
                if (i == target) return list_node_to_value(&app->list_pool[idx]);
            }
        }
        if (arg.type == VALUE_WORD) {
            size_t len = strlen(arg.word);
            if (len == 0) {
                append_output(app, "PICK: empty word\n");
                return word_value("");
            }
            char ch[2] = {arg.word[(int)random_below((double)len)], '\0'};
            return word_value(ch);
        }
        if (arg.type == VALUE_ARRAY) {
            int idx = (int)random_below(arg.number);
            return list_node_to_value(&app->list_pool[arg.list_head + idx]);
        }
        return arg; // a bare number counts as a one-element list
    }
    if (consume_keyword(ptr, "MEMBER?")) {
        Value thing = parse_factor(app, ptr);
        Value container = parse_factor(app, ptr);
        if (container.type == VALUE_LIST) {
            for (int idx = container.list_head; idx != -1; idx = app->list_pool[idx].next) {
                if (values_equal(app, thing, list_node_to_value(&app->list_pool[idx]))) {
                    return bool_value(TRUE);
                }
            }
            return bool_value(FALSE);
        }
        if (container.type == VALUE_WORD) {
            // A word's "elements" are its characters -- membership is
            // substring containment, so MEMBER? "ell "hello is true too,
            // not just single-character checks.
            char thing_text[512];
            value_to_text(app, &thing, thing_text, sizeof(thing_text));
            return bool_value(strstr(container.word, thing_text) != NULL);
        }
        // A bare number counts as a one-element list.
        return bool_value(values_equal(app, thing, container));
    }
    if (consume_keyword(ptr, "EMPTY?")) {
        Value arg = parse_factor(app, ptr);
        if (arg.type == VALUE_LIST) return bool_value(arg.list_head == -1);
        if (arg.type == VALUE_WORD) return bool_value(arg.word[0] == '\0');
        return bool_value(FALSE); // a number is never "empty"
    }
    if (consume_keyword(ptr, "WORD?")) {
        Value arg = parse_factor(app, ptr);
        return bool_value(arg.type == VALUE_WORD);
    }
    if (consume_keyword(ptr, "LIST?")) {
        Value arg = parse_factor(app, ptr);
        return bool_value(arg.type == VALUE_LIST);
    }
    if (consume_keyword(ptr, "NUMBER?")) {
        Value arg = parse_factor(app, ptr);
        return bool_value(arg.type == VALUE_NUMBER);
    }
    if (consume_keyword(ptr, "ARRAY?")) {
        Value arg = parse_factor(app, ptr);
        return bool_value(arg.type == VALUE_ARRAY);
    }
    if (consume_keyword(ptr, "MAP")) {
        Value template_val = parse_factor(app, ptr);
        Value list_val = parse_factor(app, ptr);
        char template_text[512];
        value_to_text(app, &template_val, template_text, sizeof(template_text));

        int iter_head = (list_val.type == VALUE_LIST) ? list_val.list_head : list_node_from_value(app, list_val);
        int new_head = -1;
        int *next_slot = &new_head;
        for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
            Value result = apply_template_expr(app, template_text, list_node_to_value(&app->list_pool[idx]));
            int node = list_node_from_value(app, result);
            if (node < 0) return list_pool_exhausted_error(app);
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
        return list_value(new_head);
    }
    if (consume_keyword(ptr, "FILTER")) {
        Value template_val = parse_factor(app, ptr);
        Value list_val = parse_factor(app, ptr);
        char template_text[512];
        value_to_text(app, &template_val, template_text, sizeof(template_text));

        int iter_head = (list_val.type == VALUE_LIST) ? list_val.list_head : list_node_from_value(app, list_val);
        int new_head = -1;
        int *next_slot = &new_head;
        for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
            double keep = apply_template_condition(app, template_text, list_node_to_value(&app->list_pool[idx]));
            if (keep != 0) {
                // Keep the original element as-is, not the template's
                // (boolean) result.
                int node = list_node_copy(app, idx);
                if (node < 0) return list_pool_exhausted_error(app);
                *next_slot = node;
                next_slot = &app->list_pool[node].next;
            }
        }
        return list_value(new_head);
    }
    if (consume_keyword(ptr, "REDUCE")) {
        // Folds left-to-right, seeding the accumulator with the list's
        // own first element (no separate start-value argument) -- the
        // template uses ?1 for the accumulator so far and ?2 for the
        // current element, e.g. REDUCE [?1 + ?2] [1 2 3 4] sums to 10.
        Value template_val = parse_factor(app, ptr);
        Value list_val = parse_factor(app, ptr);
        char template_text[512];
        value_to_text(app, &template_val, template_text, sizeof(template_text));

        int iter_head = (list_val.type == VALUE_LIST) ? list_val.list_head : list_node_from_value(app, list_val);
        if (iter_head == -1) return number_value(0); // nothing to reduce

        Value acc = list_node_to_value(&app->list_pool[iter_head]);
        for (int idx = app->list_pool[iter_head].next; idx != -1; idx = app->list_pool[idx].next) {
            char acc_text[512], el_text[512], after_1[512], expr_text[512];
            Value el = list_node_to_value(&app->list_pool[idx]);
            value_to_source_text(app, &acc, acc_text, sizeof(acc_text));
            value_to_source_text(app, &el, el_text, sizeof(el_text));
            substitute_placeholder(template_text, "?1", acc_text, after_1, sizeof(after_1));
            substitute_placeholder(after_1, "?2", el_text, expr_text, sizeof(expr_text));
            const char *expr_ptr = expr_text;
            acc = parse_expr(app, &expr_ptr);
        }
        return acc;
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
    if (consume_keyword(ptr, "FLATTEN")) {
        Value arg = parse_factor(app, ptr);
        int input_head = (arg.type == VALUE_LIST) ? arg.list_head : list_node_from_value(app, arg);
        if (arg.type != VALUE_LIST && input_head < 0) return list_pool_exhausted_error(app);
        int head = -1;
        int *next_slot = &head;
        if (!list_flatten_into(app, input_head, &next_slot)) return list_pool_exhausted_error(app);
        return list_value(head);
    }
    if (consume_keyword(ptr, "PARSE")) {
        // Tokenizes any value's printed text (same rendering PRINT uses)
        // by whitespace into a list of words -- the reverse of what
        // PRINT/value_to_text already does for a plain word. A bracket
        // in the source text is just another non-space character here
        // (PARSE doesn't reconstruct nested lists), so parsing a list
        // that itself contains a sublist yields literal "[..."/"...]"-
        // shaped word tokens rather than a real nested list back out.
        Value arg = parse_factor(app, ptr);
        char text[512];
        value_to_text(app, &arg, text, sizeof(text));
        int head;
        if (!list_tokenize_words(app, text, &head)) return list_pool_exhausted_error(app);
        return list_value(head);
    }
    if (consume_keyword(ptr, "TEXT")) {
        // The read-as-data complement to SHOW: rather than printing a
        // procedure's own TO ... END definition, outputs its raw body
        // text tokenized into a flat list of words -- same whitespace
        // tokenizing PARSE does above, not a full re-parse of Logo
        // syntax, so a "quoted word or a [bracketed block] in the body
        // keeps its punctuation as literal characters within a token
        // rather than being reinterpreted as eval_logo itself would.
        char name_buf[64] = {0};
        int name_bytes = 0;
        if (sscanf(*ptr, "%63s%n", name_buf, &name_bytes) == 1 && name_buf[0] == '"') {
            *ptr += name_bytes;
            Procedure *proc = find_procedure(app, name_buf + 1);
            if (proc == NULL) {
                append_output(app, "TEXT: no such procedure \"");
                append_output(app, name_buf + 1);
                append_output(app, "\n");
                return list_value(-1);
            }
            int head;
            if (!list_tokenize_words(app, proc->body, &head)) return list_pool_exhausted_error(app);
            return list_value(head);
        }
        append_output(app, "TEXT: expected a \"name\n");
        return list_value(-1);
    }
    if (consume_keyword(ptr, "SUBST")) {
        Value old_val = parse_factor(app, ptr);
        Value new_val = parse_factor(app, ptr);
        Value thing = parse_factor(app, ptr);
        if (thing.type != VALUE_LIST) {
            return values_equal(app, thing, old_val) ? new_val : thing;
        }
        int head;
        if (!list_subst_into(app, thing.list_head, old_val, new_val, &head)) return list_pool_exhausted_error(app);
        return list_value(head);
    }
    if (consume_keyword(ptr, "DOT")) {
        Value a = parse_factor(app, ptr);
        Value b = parse_factor(app, ptr);
        if (a.type != VALUE_LIST || b.type != VALUE_LIST) {
            append_output(app, "DOT: expected two lists\n");
            return number_value(0);
        }
        double sum = 0;
        int idx_a = a.list_head, idx_b = b.list_head;
        while (idx_a != -1 && idx_b != -1) {
            sum += value_to_number(list_node_to_value(&app->list_pool[idx_a]))
                 * value_to_number(list_node_to_value(&app->list_pool[idx_b]));
            idx_a = app->list_pool[idx_a].next;
            idx_b = app->list_pool[idx_b].next;
        }
        if (idx_a != -1 || idx_b != -1) {
            append_output(app, "DOT: lists must be the same length\n");
            return number_value(0);
        }
        return number_value(sum);
    }
    if (consume_keyword(ptr, "CROSS")) {
        Value a = parse_factor(app, ptr);
        Value b = parse_factor(app, ptr);
        double av[3], bv[3];
        if (!list_as_three_numbers(app, a, av) || !list_as_three_numbers(app, b, bv)) {
            append_output(app, "CROSS: expected two 3-element lists\n");
            return list_value(-1);
        }
        double cx = av[1] * bv[2] - av[2] * bv[1];
        double cy = av[2] * bv[0] - av[0] * bv[2];
        double cz = av[0] * bv[1] - av[1] * bv[0];
        int n0 = list_node_from_value(app, number_value(cx));
        int n1 = list_node_from_value(app, number_value(cy));
        int n2 = list_node_from_value(app, number_value(cz));
        if (n0 < 0 || n1 < 0 || n2 < 0) return list_pool_exhausted_error(app);
        app->list_pool[n0].next = n1;
        app->list_pool[n1].next = n2;
        return list_value(n0);
    }
    if (consume_keyword(ptr, "TRUE")) {
        return word_value("TRUE");
    }
    if (consume_keyword(ptr, "FALSE")) {
        return word_value("FALSE");
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
    if (consume_keyword(ptr, "CANVASSIZE")) {
        // [width height], same 2-element-list convention as POS.
        return list_wrap_pair(app, number_value(app->canvas_width), number_value(app->canvas_height));
    }
    if (consume_keyword(ptr, "GETX")) {
        return number_value(current_turtle(app)->x);
    }
    if (consume_keyword(ptr, "GETY")) {
        return number_value(current_turtle(app)->y);
    }
    if (consume_keyword(ptr, "WHO")) {
        return number_value(app->current_turtle);
    }
    if (consume_keyword(ptr, "PROCEDURES")) {
        int head = -1;
        int *next_slot = &head;
        for (int i = 0; i < app->proc_count; i++) {
            int node = list_node_from_value(app, word_value(app->procedures[i].name));
            if (node < 0) return list_pool_exhausted_error(app);
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
        return list_value(head);
    }
    if (consume_keyword(ptr, "NAMES")) {
        int head = -1;
        int *next_slot = &head;
        for (int i = 0; i < app->var_count; i++) {
            int node = list_node_from_value(app, word_value(app->variables[i].name));
            if (node < 0) return list_pool_exhausted_error(app);
            *next_slot = node;
            next_slot = &app->list_pool[node].next;
        }
        return list_value(head);
    }
    if (consume_keyword(ptr, "DISTANCE")) {
        // Plain distance between two arbitrary [x y] points -- not tied
        // to the turtle's own position, unlike TOWARDS below. Pass POS
        // as one of the two points for "distance from here".
        Value a = parse_factor(app, ptr);
        Value b = parse_factor(app, ptr);
        double av[2], bv[2];
        if (!list_as_two_numbers(app, a, av) || !list_as_two_numbers(app, b, bv)) {
            append_output(app, "DISTANCE: expected two 2-element lists\n");
            return number_value(0);
        }
        double dx = bv[0] - av[0];
        double dy = bv[1] - av[1];
        return number_value(sqrt(dx * dx + dy * dy));
    }
    if (consume_keyword(ptr, "TOWARDS")) {
        // The heading (same convention as HEADING/SETHEADING/RT/LT) to
        // face directly from the turtle's current position toward
        // point, derived directly from move_turtle_forward's own
        // dx/dy-vs-heading formula so SETHEADING TOWARDS point then
        // FORWARD DISTANCE POS point actually walks straight to point.
        // Unlike HEADING (a live, unbounded accumulator), this is a
        // freshly computed compass bearing, so it's normalized to
        // [0, 360) the way most Logo dialects report TOWARDS.
        Value p = parse_factor(app, ptr);
        double pv[2];
        if (!list_as_two_numbers(app, p, pv)) {
            append_output(app, "TOWARDS: expected a 2-element list\n");
            return number_value(0);
        }
        double dx = pv[0] - current_turtle(app)->x;
        double dy = pv[1] - current_turtle(app)->y;
        double heading = atan2(dx, -dy) * 180.0 / M_PI;
        if (heading < 0) heading += 360.0;
        return number_value(heading);
    }
    if (consume_keyword(ptr, "THING")) {
        // The reflective, computed-name sibling of :name — :name only
        // ever takes a literal identifier written right there in the
        // source, while THING takes any expression that evaluates to a
        // word, e.g. THING WORD "item :n.
        Value name_val = parse_factor(app, ptr);
        char name_text[512];
        value_to_text(app, &name_val, name_text, sizeof(name_text));
        Variable *v = find_var(app, name_text);
        if (v != NULL) {
            if (v->type == VALUE_WORD) return word_value(v->word);
            if (v->type == VALUE_LIST) return list_value(v->list_head);
            if (v->type == VALUE_ARRAY) return array_value(v->list_head, (int)v->number);
            return number_value(v->number);
        }
        return number_value(0);
    }
    if (consume_keyword(ptr, "GETPROP")) {
        Value name_val = parse_factor(app, ptr);
        Value key_val = parse_factor(app, ptr);
        char name_text[32], key_text[32];
        value_to_text(app, &name_val, name_text, sizeof(name_text));
        value_to_text(app, &key_val, key_text, sizeof(key_text));
        int idx = find_plist_entry(app, name_text, key_text);
        if (idx < 0) return list_value(-1); // no such property: the empty list
        PlistEntry *e = &app->plist_entries[idx];
        if (e->type == VALUE_WORD) return word_value(e->word);
        if (e->type == VALUE_LIST) return list_value(e->list_head);
        if (e->type == VALUE_ARRAY) return array_value(e->list_head, (int)e->number);
        return number_value(e->number);
    }
    if (consume_keyword(ptr, "PROPLIST")) {
        Value name_val = parse_factor(app, ptr);
        char name_text[32];
        value_to_text(app, &name_val, name_text, sizeof(name_text));
        int head = -1;
        int *next_slot = &head;
        for (int i = 0; i < app->plist_entry_count; i++) {
            PlistEntry *e = &app->plist_entries[i];
            if (strcasecmp(e->plist_name, name_text) != 0) continue;
            int key_node = list_node_from_value(app, word_value(e->key));
            if (key_node < 0) return list_pool_exhausted_error(app);
            *next_slot = key_node;
            next_slot = &app->list_pool[key_node].next;
            Value val;
            if (e->type == VALUE_WORD) val = word_value(e->word);
            else if (e->type == VALUE_LIST) val = list_value(e->list_head);
            else if (e->type == VALUE_ARRAY) val = array_value(e->list_head, (int)e->number);
            else val = number_value(e->number);
            int val_node = list_node_from_value(app, val);
            if (val_node < 0) return list_pool_exhausted_error(app);
            *next_slot = val_node;
            next_slot = &app->list_pool[val_node].next;
        }
        return list_value(head);
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
            if (v->type == VALUE_ARRAY) return array_value(v->list_head, (int)v->number);
            return number_value(v->number);
        }
        return number_value(0);
    }

    // A user-defined procedure used as a value-producing operator (its
    // body calls OUTPUT) -- e.g. PRINT double 5, MAKE "x double 5. Only
    // reached here, after every built-in operator above, since a
    // same-named built-in always wins (same precedence rule the ordinary
    // top-level procedure-call dispatch in eval_logo already has for
    // command names). Calling a procedure that never outputs anything
    // this way is a reported error rather than a silent 0 or empty word,
    // matching this project's "loud, not silent" precedent for other
    // structural mismatches (WORD on a list, FPUT/LPUT mixing types).
    {
        const char *lookahead = skip_whitespace(*ptr);
        char name[32] = {0};
        int n = 0;
        if (sscanf(lookahead, "%31s%n", name, &n) == 1) {
            Procedure *proc = find_procedure(app, name);
            if (proc != NULL) {
                *ptr = lookahead + n;
                double arg_vals[MAX_PARAMS];
                for (int p = 0; p < proc->param_count; p++) {
                    arg_vals[p] = value_to_number(parse_expr(app, ptr));
                }
                gboolean did_output = FALSE;
                Value result = call_procedure(app, proc, arg_vals, &did_output);
                if (!did_output) {
                    append_output(app, name);
                    append_output(app, ": didn't output a value\n");
                    return word_value("");
                }
                return result;
            }
        }
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

    return is_truthy(left);
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

// Push a fresh scope binding proc's parameters to arg_vals, run its
// body, then pop the scope — shared by an ordinary procedure call,
// APPLY, and the value-producing procedure-call operator in
// parse_factor, which source their argument values differently (parsed
// positionally from the command line, taken from a list, or parsed as
// an expression's operand) but bind, run, and unwind exactly the same
// way. Forced inline: extracting this added an extra stack frame to
// every level of procedure-call recursion (there used to be none —
// eval_logo just called itself directly), which alone overflowed the
// stack at the existing, documented 200-call cap under
// AddressSanitizer. Inlining folds it back into eval_logo's own frame,
// restoring the original per-level stack cost.
//
// Returns whatever OUTPUT (if any) set inside the procedure's body, and
// reports through *did_output whether one actually ran — a bare STOP,
// or the body just running to completion, means the procedure never
// output anything. Callers that only want the side effects (the
// ordinary statement-call site, APPLY) pass NULL and ignore it; the
// operator call site (a procedure used as a value inside an expression)
// checks it and reports an error if nothing was output.
static inline __attribute__((always_inline)) Value call_procedure(LogoApp *app, Procedure *proc, double *arg_vals, gboolean *did_output) {
    if (app->scope_depth >= MAX_SCOPE_DEPTH) {
        append_output(app, "Recursion too deep, call ignored\n");
        if (did_output != NULL) *did_output = FALSE;
        return number_value(0);
    }
    // Bind parameters as locals in a fresh scope so they shadow
    // same-named variables from outer calls or globals, then run the
    // unmodified procedure body.
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

    // OUTPUT/STOP's stop_requested is caught right here: this procedure
    // call is exactly the boundary it's meant to unwind to, so it's
    // cleared before returning rather than left to keep propagating into
    // whatever called *this* procedure.
    gboolean produced = app->has_output_value;
    Value result = produced ? load_output_value(app) : number_value(0);
    app->stop_requested = FALSE;
    app->has_output_value = FALSE;
    app->scope_depth--;
    if (did_output != NULL) *did_output = produced;
    return result;
}

// Tokenize and execute one chunk of Logo source (a REPL line, a
// procedure body, or a REPEAT/IF/WHILE block), recursing for nested
// blocks and procedure calls. Stops dead as soon as OUTPUT/STOP sets
// stop_requested, or THROW sets throw_requested — including partway
// through this exact call, if this invocation *is* the one running the
// OUTPUT/STOP/THROW statement itself — which is what lets either unwind
// up through however many nested REPEAT/IF/WHILE eval_logo calls sit
// between there and wherever is actually meant to catch it
// (call_procedure catches stop_requested at its own boundary; CATCH is
// what catches throw_requested, if its tag matches).
//
// eval_depth tracks real call nesting (every eval_logo call, not just
// procedure calls, unlike scope_depth) so that once it unwinds all the
// way back to the genuine outermost call with throw_requested still
// set — meaning nothing anywhere caught it — this reports the
// "uncaught throw" error and clears the flag itself, rather than
// leaving it set forever and silently turning every later top-level
// command into a no-op.
void eval_logo(LogoApp *app, const char *code) {
    const char *ptr = code;
    app->eval_depth++;

    while (*ptr != '\0' && !app->stop_requested) {
        if (app->throw_requested) {
            // Only the true outermost eval_logo call (this script's own
            // top level -- not some inner REPEAT/IF/WHILE block or
            // nested procedure/CATCH call) recovers from an uncaught
            // THROW here; any other frame just breaks immediately, the
            // same as before, so the throw keeps unwinding up to
            // whichever CATCH (or this outermost recovery) actually
            // stops it. Recovering rather than only breaking is what
            // lets the rest of a loaded script's top-level commands
            // keep running after an uncaught THROW, instead of the
            // report effectively aborting everything that follows it.
            if (app->eval_depth > 1) break;
            append_output(app, "THROW: no CATCH found for \"");
            append_output(app, app->throw_tag);
            append_output(app, "\n");
            app->throw_requested = FALSE;
        }

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
        // 1d'. LOADPIC "path — load an image file as the canvas
        // background. Needs real image decoding (gdk-pixbuf, any format
        // it supports: PNG/JPEG/GIF/BMP/...), so — unlike LOAD/SAVE
        // above, which are plain text file I/O interpreter.c does
        // itself — this goes through a GTK-side callback (same pattern
        // as CLEARTEXT's clear_history), silently doing nothing in
        // headless tests where there's no callback to decode with.
        else if (strcasecmp(token, "LOADPIC") == 0) {
            char path_buf[512] = {0};
            if (sscanf(ptr, "%511s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
                ptr += read_bytes;
                if (app->load_background_image != NULL && !app->load_background_image(app, path_buf + 1)) {
                    append_output(app, "LOADPIC: could not load \"");
                    append_output(app, path_buf + 1);
                    append_output(app, "\n");
                }
            } else {
                append_output(app, "LOADPIC: expected a \"path\n");
            }
        }
        // 1d''. SAVEPIC "path — save the canvas (background image,
        // drawing, turtles) as a PNG, LOADPIC's reverse. Same
        // GTK-callback pattern as LOADPIC, for the same reason (needs
        // real image encoding).
        else if (strcasecmp(token, "SAVEPIC") == 0) {
            char path_buf[512] = {0};
            if (sscanf(ptr, "%511s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
                ptr += read_bytes;
                if (app->save_canvas_image != NULL && !app->save_canvas_image(app, path_buf + 1)) {
                    append_output(app, "SAVEPIC: could not save \"");
                    append_output(app, path_buf + 1);
                    append_output(app, "\n");
                }
            } else {
                append_output(app, "SAVEPIC: expected a \"path\n");
            }
        }
        // 1e. SHOW "name — print a user-defined procedure's own TO ...
        // END definition back to the history pane, reusing the exact
        // same rendering SAVE writes to a file (append_procedure_text).
        else if (strcasecmp(token, "SHOW") == 0) {
            char name_buf[64] = {0};
            if (sscanf(ptr, "%63s%n", name_buf, &read_bytes) == 1 && name_buf[0] == '"') {
                ptr += read_bytes;
                Procedure *proc = find_procedure(app, name_buf + 1);
                if (proc != NULL) {
                    GString *text = g_string_new(NULL);
                    append_procedure_text(text, proc);
                    append_output(app, text->str);
                    g_string_free(text, TRUE);
                } else {
                    append_output(app, "SHOW: no such procedure \"");
                    append_output(app, name_buf + 1);
                    append_output(app, "\n");
                }
            } else {
                append_output(app, "SHOW: expected a \"name\n");
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
                    if (app->stop_requested || app->throw_requested) break; // OUTPUT/STOP/THROW inside the block escapes the loop
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
                    if (app->stop_requested || app->throw_requested) break; // OUTPUT/STOP/THROW inside the block escapes the loop
                    const char *cptr = cond_text;
                    cond = parse_condition(app, &cptr);
                    iterations++;
                }
            } else {
                append_output(app, "WHILE: expected [ block ]\n");
            }
        }
        // 2c. FOR LOOPS: FOR [var start limit step] [block] — step is
        // optional and defaults to +1 (or -1 if limit < start), same as
        // Berkeley Logo. Unlike REPEAT (count only) or WHILE (manual
        // increment), the loop variable itself is set via MAKE's own
        // set_var each iteration, so it reads back as an ordinary :var
        // inside the block — same non-scoping behavior as REPEAT/WHILE's
        // blocks (no push_scope), so MAKE-ing over an outer variable of
        // the same name has the same effect it always would.
        else if (strcasecmp(token, "FOR") == 0) {
            char header[256] = {0};
            const char *after_header = extract_block(ptr, header, sizeof(header));

            if (after_header != NULL) {
                ptr = after_header;
                char varname[64] = {0};
                const char *hptr = header;
                int header_bytes = 0;

                if (sscanf(hptr, "%63s%n", varname, &header_bytes) == 1) {
                    hptr += header_bytes;
                    double start = value_to_number(parse_expr(app, &hptr));
                    double limit = value_to_number(parse_expr(app, &hptr));
                    hptr = skip_whitespace(hptr);
                    double step;
                    if (*hptr != '\0') {
                        step = value_to_number(parse_expr(app, &hptr));
                    } else {
                        step = (limit >= start) ? 1 : -1;
                    }

                    char block_body[4096] = {0};
                    const char *after_block = extract_block(ptr, block_body, sizeof(block_body));

                    if (after_block != NULL) {
                        ptr = after_block;
                        if (step == 0) {
                            append_output(app, "FOR: step must not be 0\n");
                        } else {
                            int iterations = 0;
                            for (double i = start; (step > 0) ? (i <= limit) : (i >= limit); i += step) {
                                if (iterations >= MAX_WHILE_ITERATIONS) {
                                    append_output(app, "FOR: stopped after too many iterations\n");
                                    break;
                                }
                                set_var(app, varname, i);
                                eval_logo(app, block_body);
                                if (app->stop_requested || app->throw_requested) break; // OUTPUT/STOP/THROW inside the block escapes the loop
                                iterations++;
                            }
                        }
                    } else {
                        append_output(app, "FOR: expected [ block ]\n");
                    }
                } else {
                    append_output(app, "FOR: expected [ var start limit step ]\n");
                }
            } else {
                append_output(app, "FOR: expected [ var start limit step ]\n");
            }
        }
        // 2d. FOREVER LOOPS: FOREVER [block] — an infinite loop, only
        // escaped via STOP (or OUTPUT/THROW) inside the block; useful for
        // a game-loop-style script. Capped by the same iteration ceiling
        // as WHILE so a script that forgets its own STOP doesn't hang.
        else if (strcasecmp(token, "FOREVER") == 0) {
            char block_body[4096] = {0};
            const char *after_block = extract_block(ptr, block_body, sizeof(block_body));

            if (after_block != NULL) {
                ptr = after_block;
                int iterations = 0;
                for (;;) {
                    if (iterations >= MAX_WHILE_ITERATIONS) {
                        append_output(app, "FOREVER: stopped after too many iterations\n");
                        break;
                    }
                    eval_logo(app, block_body);
                    if (app->stop_requested || app->throw_requested) break; // OUTPUT/STOP/THROW inside the block escapes the loop
                    iterations++;
                }
            } else {
                append_output(app, "FOREVER: expected [ block ]\n");
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
        else if (strcasecmp(token, "SETX") == 0) {
            double x = value_to_number(parse_expr(app, &ptr));
            move_turtle_to(app, x, current_turtle(app)->y);
        }
        else if (strcasecmp(token, "SETY") == 0) {
            double y = value_to_number(parse_expr(app, &ptr));
            move_turtle_to(app, current_turtle(app)->x, y);
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
                        init_turtle(app, &app->turtles[i]);
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
                } else if (val.type == VALUE_ARRAY) {
                    set_var_array(app, varname + 1, val.list_head, (int)val.number);
                } else {
                    set_var(app, varname + 1, val.number);
                }
            } else {
                append_output(app, "MAKE: expected a \"name\n");
            }
        }
        // 3b'. Property lists: named plistname/propname/value records,
        // separate from ordinary variables (see PlistEntry in
        // logo_types.h). Unlike MAKE's varname, plistname/propname here
        // are each any expression evaluating to a word (matching THING's
        // convention, not requiring a literal "word), so e.g. SETPROP WORD
        // "turtle :n "color "red works.
        else if (strcasecmp(token, "SETPROP") == 0) {
            Value name_val = parse_expr(app, &ptr);
            Value key_val = parse_expr(app, &ptr);
            Value new_val = parse_expr(app, &ptr);
            char name_text[32], key_text[32];
            value_to_text(app, &name_val, name_text, sizeof(name_text));
            value_to_text(app, &key_val, key_text, sizeof(key_text));
            int idx = find_plist_entry(app, name_text, key_text);
            if (idx < 0) {
                if (app->plist_entry_count >= MAX_PLIST_ENTRIES) {
                    append_output(app, "SETPROP: too many properties defined, not set\n");
                    idx = -1;
                } else {
                    idx = app->plist_entry_count++;
                    snprintf(app->plist_entries[idx].plist_name, sizeof(app->plist_entries[idx].plist_name), "%s", name_text);
                    snprintf(app->plist_entries[idx].key, sizeof(app->plist_entries[idx].key), "%s", key_text);
                }
            }
            if (idx >= 0) {
                PlistEntry *e = &app->plist_entries[idx];
                e->type = new_val.type;
                e->number = new_val.number;
                snprintf(e->word, sizeof(e->word), "%s", new_val.word);
                e->list_head = new_val.list_head;
            }
        }
        else if (strcasecmp(token, "REMOVEPROP") == 0) {
            Value name_val = parse_expr(app, &ptr);
            Value key_val = parse_expr(app, &ptr);
            char name_text[32], key_text[32];
            value_to_text(app, &name_val, name_text, sizeof(name_text));
            value_to_text(app, &key_val, key_text, sizeof(key_text));
            int idx = find_plist_entry(app, name_text, key_text);
            // Order among a plist's remaining entries isn't documented as
            // stable in any Logo dialect, so a swap-with-last removal
            // (rather than shifting everything down) is fine here.
            if (idx >= 0) {
                app->plist_entries[idx] = app->plist_entries[--app->plist_entry_count];
            }
        }
        // 3a'''. SETITEM index array value — the one in-place mutation
        // in this language: overwrites the index'th cell (1-indexed,
        // same convention as ITEM) of an array in place. Every other
        // value here is immutable/copy-on-build; arrays are the
        // deliberate exception (see the ARRAY comment in parse_factor).
        else if (strcasecmp(token, "SETITEM") == 0) {
            int index = (int)value_to_number(parse_expr(app, &ptr));
            Value array_val = parse_expr(app, &ptr);
            Value new_val = parse_expr(app, &ptr);
            if (array_val.type != VALUE_ARRAY) {
                append_output(app, "SETITEM: expected an array\n");
            } else if (index < 1 || index > (int)array_val.number) {
                append_output(app, "SETITEM: index out of range\n");
            } else if (new_val.type == VALUE_ARRAY) {
                append_output(app, "SETITEM: can't store an array inside an array\n");
            } else {
                ListNode *node = &app->list_pool[array_val.list_head + (index - 1)];
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
        }
        // 3a''''. FILLARRAY array value — fill every slot of an array
        // with one value in a single call, the same per-cell assignment
        // SETITEM does, looped over every index instead of just one.
        else if (strcasecmp(token, "FILLARRAY") == 0) {
            Value array_val = parse_expr(app, &ptr);
            Value new_val = parse_expr(app, &ptr);
            if (array_val.type != VALUE_ARRAY) {
                append_output(app, "FILLARRAY: expected an array\n");
            } else if (new_val.type == VALUE_ARRAY) {
                append_output(app, "FILLARRAY: can't store an array inside an array\n");
            } else {
                for (int i = 0; i < (int)array_val.number; i++) {
                    ListNode *node = &app->list_pool[array_val.list_head + i];
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
        // 3b''. OUTPUT expr / STOP — end the current procedure call, the
        // way a real `return` does: OUTPUT also hands `expr` back as
        // that call's value (see the procedure-call operator in
        // parse_factor, below), while a bare STOP ends it with no value
        // at all. Both work through stop_requested (see eval_logo's own
        // loop condition above and call_procedure's comment) rather than
        // unwinding the C call stack directly, so either can appear
        // anywhere inside the procedure — including nested inside any
        // number of REPEAT/IF/WHILE/FOR/FOREVER blocks — and still stop
        // the whole call, not just the block it's textually inside.
        //
        // OUTPUT requires an active procedure call to hand its value
        // back to (store_output_value has nowhere to put it otherwise),
        // but STOP has no such requirement: unlike OUTPUT, it's also the
        // only way to escape a top-level FOREVER (there's no condition
        // to fall false, and there's no enclosing call for STOP to end)
        // — so STOP is legal anywhere, including directly at the REPL,
        // and just ends whatever's currently running, caught either by
        // call_procedure (inside a procedure) or by eval_logo's own
        // eval_depth-reaching-0 recovery below (at the true top level).
        else if (strcasecmp(token, "OUTPUT") == 0 || strcasecmp(token, "OP") == 0) {
            if (app->scope_depth <= 0) {
                append_output(app, "OUTPUT: can only be used inside a procedure\n");
            } else {
                Value val = parse_expr(app, &ptr);
                store_output_value(app, val);
                app->stop_requested = TRUE;
            }
        }
        else if (strcasecmp(token, "STOP") == 0) {
            app->stop_requested = TRUE;
        }
        // 3b'''. THROW tag / CATCH tag [instructions] — structured
        // non-local exit, tag-matched rather than always stopping
        // exactly one procedure call the way STOP/OUTPUT do: THROW can
        // unwind past any number of nested procedure calls (and, unlike
        // STOP/OUTPUT, isn't restricted to running inside one at all —
        // both commonly run at the top level too) to reach whichever
        // enclosing CATCH has a matching tag, reporting `THROW: no CATCH
        // found for "tag` if none does. This only catches explicit
        // THROWs the Logo program itself writes -- unlike real Logo's
        // reserved "ERROR" tag, it doesn't intercept this interpreter's
        // own error messages (see docs/LANGUAGE.md).
        else if (strcasecmp(token, "THROW") == 0) {
            Value tag_val = parse_expr(app, &ptr);
            value_to_text(app, &tag_val, app->throw_tag, sizeof(app->throw_tag));
            app->throw_requested = TRUE;
        }
        else if (strcasecmp(token, "CATCH") == 0) {
            Value tag_val = parse_expr(app, &ptr);
            char tag_text[64];
            value_to_text(app, &tag_val, tag_text, sizeof(tag_text));

            char block_body[4096];
            const char *after_block = extract_block(ptr, block_body, sizeof(block_body));
            if (after_block != NULL) {
                ptr = after_block;
                eval_logo(app, block_body);
                if (app->throw_requested && strcasecmp(app->throw_tag, tag_text) == 0) {
                    app->throw_requested = FALSE; // this CATCH's tag matched -- caught
                }
                // A non-matching throw_requested is deliberately left set
                // so it keeps propagating past this CATCH too, toward
                // whichever ancestor CATCH (if any) does match.
            } else {
                append_output(app, "CATCH: expected [ block ]\n");
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
            app->label_count = 0;
            app->raster_op_count = 0;
            for (int i = 0; i < app->turtle_count; i++) {
                app->turtles[i].x = home_x(app);
                app->turtles[i].y = home_y(app);
                app->turtles[i].angle = 0;
            }
        }
        // SETCANVASSIZE width height — resizes the canvas at runtime
        // (Terrapin's SETEXTENT, renamed for clarity). Same reset as
        // CLEAR/CS just above (the old drawing/raster don't fit a
        // different size, so there's nothing sensible to preserve), plus
        // the actual resize: interpreter.c only updates canvas_width/
        // height and requests it (staying Cairo/GTK-free, same
        // convention as every other GUI-side effect); ui.c's
        // resize_canvas callback resizes the real widget and drops
        // bg_image, which doesn't fit the new size either. fill_raster
        // needs no attention here — bake_pending_fills already detects
        // CLEAR's raster_op_count reset and rebuilds it at whatever
        // canvas_width/height currently is, so resetting raster_op_count
        // is enough to cover it too. NULL in tests, same convention as
        // LOADPIC.
        else if (strcasecmp(token, "SETCANVASSIZE") == 0) {
            double width = value_to_number(parse_expr(app, &ptr));
            double height = value_to_number(parse_expr(app, &ptr));
            if (width < MIN_CANVAS_SIZE || width > MAX_CANVAS_SIZE ||
                height < MIN_CANVAS_SIZE || height > MAX_CANVAS_SIZE) {
                char msg[96];
                snprintf(msg, sizeof(msg), "SETCANVASSIZE: width and height must be %d-%d\n",
                         (int)MIN_CANVAS_SIZE, (int)MAX_CANVAS_SIZE);
                append_output(app, msg);
            } else {
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
        }
        // CLEAN -- CLEAR's drawing-erase half only, leaving every
        // turtle's position/heading alone (the real Berkeley Logo
        // distinction between CLEAN and CLEAR/CS).
        else if (strcasecmp(token, "CLEAN") == 0) {
            app->line_count = 0;
            app->label_count = 0;
            app->raster_op_count = 0;
        }
        // CLEARTEXT -- clears the history pane specifically, separate
        // from CLEAR/CLEAN's canvas-only effect. A no-op in tests
        // (clear_history is NULL there -- no text view to clear).
        else if (strcasecmp(token, "CLEARTEXT") == 0 || strcasecmp(token, "CT") == 0) {
            if (app->clear_history != NULL) app->clear_history(app);
        }
        else if (strcasecmp(token, "HOME") == 0) {
            move_turtle_to(app, home_x(app), home_y(app));
            current_turtle(app)->angle = 0;
        }
        else if (strcasecmp(token, "PENUP") == 0 || strcasecmp(token, "PU") == 0) {
            current_turtle(app)->pen_down = 0;
        }
        else if (strcasecmp(token, "PENDOWN") == 0 || strcasecmp(token, "PD") == 0) {
            current_turtle(app)->pen_down = 1;
        }
        else if (strcasecmp(token, "HIDETURTLE") == 0 || strcasecmp(token, "HT") == 0) {
            current_turtle(app)->visible = 0;
        }
        else if (strcasecmp(token, "SHOWTURTLE") == 0 || strcasecmp(token, "ST") == 0) {
            current_turtle(app)->visible = 1;
        }
        // 3c''''. WRAP/FENCE/WINDOW — what happens when a move would cross
        // the canvas edge (see move_turtle_to). A single canvas-wide
        // setting, not per-turtle; unaffected by CLEAR, same as pen
        // color/width.
        else if (strcasecmp(token, "WRAP") == 0) {
            app->edge_mode = EDGE_WRAP;
        }
        else if (strcasecmp(token, "FENCE") == 0) {
            app->edge_mode = EDGE_FENCE;
        }
        else if (strcasecmp(token, "WINDOW") == 0) {
            app->edge_mode = EDGE_WINDOW;
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
        // 3c'''''. LABEL text — draws text at the turtle's current
        // position, in its current pen color. Pure data (position,
        // color, text) recorded here; ui.c's draw_scene does the actual
        // Cairo text rendering, keeping this file free of any GTK/Cairo
        // dependency, same as lines[] already works for FD/etc.
        else if (strcasecmp(token, "LABEL") == 0) {
            Value val = parse_expr(app, &ptr);
            if (app->label_count < MAX_LABELS) {
                Turtle *t = current_turtle(app);
                Label *label = &app->labels[app->label_count++];
                label->x = t->x;
                label->y = t->y;
                label->r = t->pen_r;
                label->g = t->pen_g;
                label->b = t->pen_b;
                value_to_text(app, &val, label->text, sizeof(label->text));
            }
        }
        // 3c''''''. FILL — flood-fills the region containing the turtle,
        // bounded by whatever lines are drawn as of this exact moment,
        // with the turtle's current pen color. Same "record plain data,
        // let ui.c do the actual Cairo/rasterizing work" split as
        // LABEL; line_count_at_call freezes the boundary so a line
        // drawn after this FILL can't retroactively change what it
        // filled (see ui.c's bake_pending_fills).
        else if (strcasecmp(token, "FILL") == 0) {
            if (app->raster_op_count < MAX_RASTER_OPS) {
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
        }
        // 3c''''''a. ERASERECT w h — paints a w-by-h rectangle centered
        // on the turtle in the background color, same call-time-frozen
        // treatment as FILL just above (and the same reason: sharing
        // RasterOp/bake_pending_fills means a line drawn after this
        // ERASERECT can't retroactively change what got erased either).
        else if (strcasecmp(token, "ERASERECT") == 0) {
            double w = value_to_number(parse_expr(app, &ptr));
            double h = value_to_number(parse_expr(app, &ptr));
            if (app->raster_op_count < MAX_RASTER_OPS) {
                Turtle *t = current_turtle(app);
                RasterOp *op = &app->raster_ops[app->raster_op_count++];
                op->kind = RASTER_OP_ERASE_RECT;
                op->x = t->x;
                op->y = t->y;
                op->w = w;
                op->h = h;
                op->line_count_at_call = app->line_count;
            }
        }
        // 3c''''''b. LOADSPRITE "name "path — decodes an image file as a
        // named turtle shape (SETSPRITE assigns it; STAMPSPRITE bakes a
        // permanent copy onto the canvas). Needs real image decoding, so
        // — like LOADPIC — this goes through a GTK-side callback,
        // silently doing nothing in headless tests. Equivalent to
        // LOADSPRITESHEET with a 1x1 grid (the whole image is frame 0).
        else if (strcasecmp(token, "LOADSPRITE") == 0) {
            char name_buf[64] = {0};
            char path_buf[512] = {0};
            if (sscanf(ptr, "%63s%n", name_buf, &read_bytes) == 1 && name_buf[0] == '"') {
                ptr += read_bytes;
                if (sscanf(ptr, "%511s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
                    ptr += read_bytes;
                    if (app->load_sprite_image != NULL && !app->load_sprite_image(app, name_buf + 1, path_buf + 1, 1, 1)) {
                        append_output(app, "LOADSPRITE: could not load \"");
                        append_output(app, path_buf + 1);
                        append_output(app, "\n");
                    }
                } else {
                    append_output(app, "LOADSPRITE: expected a \"path\n");
                }
            } else {
                append_output(app, "LOADSPRITE: expected a \"name\n");
            }
        }
        // 3c''''''b'. LOADSPRITESHEET "name "path cols rows — like
        // LOADSPRITE, but slices the loaded image into a cols-by-rows
        // grid of equal-size frames (a sprite-sheet blit): SETSPRITEFRAME
        // then picks which cell of that grid the turtle currently shows,
        // 0-indexed row-major (frame 0 is the top-left cell). Same
        // GTK-callback/headless-no-op convention as LOADSPRITE.
        else if (strcasecmp(token, "LOADSPRITESHEET") == 0) {
            char name_buf[64] = {0};
            char path_buf[512] = {0};
            if (sscanf(ptr, "%63s%n", name_buf, &read_bytes) == 1 && name_buf[0] == '"') {
                ptr += read_bytes;
                if (sscanf(ptr, "%511s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
                    ptr += read_bytes;
                    double cols = value_to_number(parse_expr(app, &ptr));
                    double rows = value_to_number(parse_expr(app, &ptr));
                    if (cols < 1 || rows < 1) {
                        append_output(app, "LOADSPRITESHEET: cols and rows must be at least 1\n");
                    } else if (app->load_sprite_image != NULL &&
                               !app->load_sprite_image(app, name_buf + 1, path_buf + 1, (int)cols, (int)rows)) {
                        append_output(app, "LOADSPRITESHEET: could not load \"");
                        append_output(app, path_buf + 1);
                        append_output(app, "\n");
                    }
                } else {
                    append_output(app, "LOADSPRITESHEET: expected a \"path\n");
                }
            } else {
                append_output(app, "LOADSPRITESHEET: expected a \"name\n");
            }
        }
        // 3c''''''c. SETSPRITE "name — assigns a previously LOADSPRITE'd
        // (or LOADSPRITESHEET'd) shape to the current turtle, replacing
        // its default triangle wherever it's drawn (live on the canvas,
        // or a later STAMPSPRITE), and resetting its active frame back to
        // 0 (a freshly assigned shape always starts at its first frame).
        // SETSPRITE "NONE resets it back to the default triangle. A plain
        // name lookup against sprite_names/sprite_count -- both plain
        // data, safe for this file to read directly even though only
        // ui.c's LOADSPRITE callback ever populates them (see
        // logo_types.h).
        else if (strcasecmp(token, "SETSPRITE") == 0) {
            char name_buf[64] = {0};
            if (sscanf(ptr, "%63s%n", name_buf, &read_bytes) == 1 && name_buf[0] == '"') {
                ptr += read_bytes;
                if (strcasecmp(name_buf + 1, "NONE") == 0) {
                    current_turtle(app)->sprite_index = -1;
                    current_turtle(app)->sprite_frame = 0;
                } else {
                    int idx = -1;
                    for (int i = 0; i < app->sprite_count; i++) {
                        if (strcasecmp(app->sprite_names[i], name_buf + 1) == 0) { idx = i; break; }
                    }
                    if (idx < 0) {
                        append_output(app, "SETSPRITE: no such sprite \"");
                        append_output(app, name_buf + 1);
                        append_output(app, "\n");
                    } else {
                        current_turtle(app)->sprite_index = idx;
                        current_turtle(app)->sprite_frame = 0;
                    }
                }
            } else {
                append_output(app, "SETSPRITE: expected a \"name\n");
            }
        }
        // 3c''''''c'. SETSPRITEFRAME n — picks which cell of the current
        // turtle's sprite-sheet grid (LOADSPRITESHEET's cols/rows) is
        // shown, 0-indexed row-major. Requires a sprite already assigned
        // via SETSPRITE (not the default triangle), and a frame within
        // that sprite's grid.
        else if (strcasecmp(token, "SETSPRITEFRAME") == 0) {
            double n = value_to_number(parse_expr(app, &ptr));
            Turtle *t = current_turtle(app);
            if (t->sprite_index < 0) {
                append_output(app, "SETSPRITEFRAME: no sprite set (use SETSPRITE first)\n");
            } else {
                int frame_count = app->sprite_frame_cols[t->sprite_index] * app->sprite_frame_rows[t->sprite_index];
                if ((int)n < 0 || (int)n >= frame_count) {
                    append_output(app, "SETSPRITEFRAME: frame out of range\n");
                } else {
                    t->sprite_frame = (int)n;
                }
            }
        }
        // 3c''''''c''. ANIMATESPRITE delay frames — advances the current
        // turtle's sprite-sheet frame by 1 (wrapping around its grid),
        // `frames` times, pausing `delay` seconds and requesting a redraw
        // between each advance -- the recurring version of WAIT's own
        // redraw-and-drain-events technique just below, so each
        // intermediate frame is actually visible on screen rather than
        // only the final one once this command (and the rest of the
        // script) finishes. Live-only, same as SETSPRITEFRAME: doesn't
        // touch the canvas raster, so it has nothing to do with
        // STAMPSPRITE unless the caller stamps explicitly in between.
        // Requires a sprite already assigned via SETSPRITE, same
        // requirement (and error message) as SETSPRITEFRAME.
        else if (strcasecmp(token, "ANIMATESPRITE") == 0) {
            double delay = value_to_number(parse_expr(app, &ptr));
            int frames = (int)value_to_number(parse_expr(app, &ptr));
            Turtle *t = current_turtle(app);
            if (t->sprite_index < 0) {
                append_output(app, "ANIMATESPRITE: no sprite set (use SETSPRITE first)\n");
            } else {
                int frame_count = app->sprite_frame_cols[t->sprite_index] * app->sprite_frame_rows[t->sprite_index];
                for (int i = 0; i < frames; i++) {
                    t->sprite_frame = (t->sprite_frame + 1) % frame_count;
                    if (app->request_redraw != NULL) {
                        app->request_redraw(app);
                    }
                    if (delay > 0) {
                        gint64 end_time = g_get_monotonic_time() + (gint64)(delay * G_USEC_PER_SEC);
                        while (g_get_monotonic_time() < end_time) {
                            while (g_main_context_iteration(NULL, FALSE)) {
                                // drain pending events without blocking
                            }
                            g_usleep(1000);
                        }
                    }
                }
            }
        }
        // 3c''''''d. STAMPSPRITE — bakes a permanent copy of the current
        // turtle's shape (its SETSPRITE image/frame, or the default
        // triangle) onto the canvas at its current position/heading. Same
        // call-time-frozen RasterOp treatment as FILL/ERASERECT just
        // above, and the same silent no-op if the raster op table is
        // already full (matching FILL's own convention).
        else if (strcasecmp(token, "STAMPSPRITE") == 0) {
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
        // 3c'''''''. WAIT expr — pauses for expr seconds (this
        // interpreter's own unit choice; real Logo's WAIT counts 60ths
        // of a second) before continuing, without freezing the window:
        // sleeps in small slices, draining the GLib main context's
        // pending events between each one, rather than one long blocking
        // sleep. Calls request_redraw first -- a whole LOADed file runs
        // as one eval_logo call, and the canvas only actually redraws
        // once that returns, so without explicitly queueing one here,
        // there'd be nothing yet for the draining below to paint, and
        // whatever was drawn before this WAIT would stay invisible until
        // the entire script finished. request_redraw is a plain
        // function-pointer callback (same pattern as output_sink) so
        // this file still never calls GTK/Cairo directly; it's NULL
        // (skipped) in the headless test binary, where there's no
        // window to draw anyway.
        else if (strcasecmp(token, "WAIT") == 0) {
            double seconds = value_to_number(parse_expr(app, &ptr));
            if (seconds > 0) {
                if (app->request_redraw != NULL) {
                    app->request_redraw(app);
                }
                gint64 end_time = g_get_monotonic_time() + (gint64)(seconds * G_USEC_PER_SEC);
                while (g_get_monotonic_time() < end_time) {
                    while (g_main_context_iteration(NULL, FALSE)) {
                        // drain pending events without blocking
                    }
                    g_usleep(1000);
                }
            }
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
        // 3d'. TYPE <expr> — same as PRINT, but without the trailing
        // newline, so several TYPEs (or a TYPE followed by a PRINT) can
        // build up one line of output piece by piece.
        else if (strcasecmp(token, "TYPE") == 0) {
            Value val = parse_expr(app, &ptr);
            char line[512];
            value_to_text(app, &val, line, sizeof(line));
            append_output(app, line);
        }
        // 3e. RUN thing — executes a stored word/list as Logo source,
        // exactly as if it had been typed directly: the thing that makes
        // a list double as a deferred program, not just data. Capped by
        // run_depth rather than scope_depth, since RUN doesn't push a
        // scope of its own — run code shares the caller's scope — but
        // still needs a guard against a self-referential RUN (e.g. MAKE
        // "x [RUN :x] / RUN :x) blowing the C call stack.
        else if (strcasecmp(token, "RUN") == 0) {
            Value val = parse_expr(app, &ptr);
            if (app->run_depth >= MAX_RUN_DEPTH) {
                append_output(app, "RUN: too deeply nested, ignored\n");
            } else {
                char code_text[4096];
                value_to_text(app, &val, code_text, sizeof(code_text));
                app->run_depth++;
                eval_logo(app, code_text);
                app->run_depth--;
            }
        }
        // 3f. APPLY "name arglist — calls a procedure with arguments
        // taken from a list, instead of parsed positionally from the
        // command line. A non-list argument is treated as a one-element
        // list, same convention list operators use.
        else if (strcasecmp(token, "APPLY") == 0) {
            Value name_val = parse_expr(app, &ptr);
            Value list_val = parse_expr(app, &ptr);
            char name_text[64];
            value_to_text(app, &name_val, name_text, sizeof(name_text));
            Procedure *proc = find_procedure(app, name_text);
            if (proc == NULL) {
                append_output(app, "APPLY: no such procedure \"");
                append_output(app, name_text);
                append_output(app, "\n");
            } else {
                double arg_vals[MAX_PARAMS];
                int count = 0;
                if (list_val.type == VALUE_LIST) {
                    for (int idx = list_val.list_head; idx != -1 && count < MAX_PARAMS; idx = app->list_pool[idx].next) {
                        arg_vals[count++] = value_to_number(list_node_to_value(&app->list_pool[idx]));
                    }
                } else {
                    arg_vals[count++] = value_to_number(list_val);
                }
                if (count != proc->param_count) {
                    append_output(app, "APPLY: wrong number of inputs for procedure \"");
                    append_output(app, name_text);
                    append_output(app, "\n");
                } else {
                    call_procedure(app, proc, arg_vals, NULL);
                }
            }
        }
        // 3g. FOREACH [template with ?] list — runs the template (as a
        // command, for side effects — MAP/FILTER/REDUCE are the
        // value-producing operator forms) once per element, substituting
        // ? for that element each time.
        else if (strcasecmp(token, "FOREACH") == 0) {
            Value template_val = parse_expr(app, &ptr);
            Value list_val = parse_expr(app, &ptr);
            char template_text[512];
            value_to_text(app, &template_val, template_text, sizeof(template_text));

            int iter_head = (list_val.type == VALUE_LIST) ? list_val.list_head : list_node_from_value(app, list_val);
            for (int idx = iter_head; idx != -1; idx = app->list_pool[idx].next) {
                Value el = list_node_to_value(&app->list_pool[idx]);
                char el_text[512], code_text[512];
                value_to_source_text(app, &el, el_text, sizeof(el_text));
                substitute_placeholder(template_text, "?", el_text, code_text, sizeof(code_text));
                eval_logo(app, code_text);
                if (app->stop_requested || app->throw_requested) break; // OUTPUT/STOP/THROW inside the template escapes the loop
            }
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
                call_procedure(app, proc, arg_vals, NULL);
            } else {
                append_output(app, "I don't know how to ");
                append_output(app, token);
                append_output(app, "\n");
            }
        }
    }

    app->eval_depth--;
    if (app->eval_depth == 0 && app->throw_requested) {
        append_output(app, "THROW: no CATCH found for \"");
        append_output(app, app->throw_tag);
        append_output(app, "\n");
        app->throw_requested = FALSE;
    }
    // A STOP with no enclosing procedure call (call_procedure resets it
    // at its own boundary otherwise -- see its comment) reaches here
    // instead: the true outermost call, once it's unwound all the way
    // back up, is what quietly clears it, the same way the recovery
    // above does for an uncaught THROW -- just silently, since a bare
    // top-level STOP (e.g. ending a FOREVER typed directly at the REPL,
    // where there's no condition to fall false and nothing else to end)
    // is ordinary control flow, not an error to report.
    if (app->eval_depth == 0 && app->stop_requested) {
        app->stop_requested = FALSE;
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
        append_procedure_text(out, &app->procedures[i]);
        g_string_append_c(out, '\n');
    }
    return g_string_free(out, FALSE);
}
