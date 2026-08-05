#ifndef LOGO_TYPES_H
#define LOGO_TYPES_H

// logo_types.h
//
// Shared data structures and limits for the Logo interpreter: the turtle,
// drawn line segments, user-defined procedures, variables, and the overall
// LogoApp struct (interpreter state plus the GTK widgets that display it),
// passed around by both interpreter.c and ui.c.

#include <gtk/gtk.h>

#define MAX_LINES 10000
#define MAX_PROCEDURES 50
#define MAX_VARIABLES 100
#define MAX_WHILE_ITERATIONS 1000000
#define MAX_PARAMS 8
#define MAX_SCOPE_DEPTH 200
#define MAX_HISTORY 200
#define MAX_TURTLES 10
#define MAX_LIST_NODES 8192

// One drawn segment of the turtle's trail, in the pen color/width active
// when it was drawn (so a drawing can mix both across SETPENCOLOR and
// SETPENWIDTH calls).
typedef struct {
    double x1, y1, x2, y2;
    double r, g, b; // 0.0-1.0
    double width;
} LineSegment;

// The turtle's position, heading, pen state, and current pen color/width
// (color 0.0-1.0 per channel; set via SETPENCOLOR/SETPC and
// SETPENWIDTH/SETPW).
typedef struct {
    double x, y;
    double angle;
    int pen_down;
    double pen_r, pen_g, pen_b;
    double pen_width;
} Turtle;

// A user-defined procedure (TO ... END).
typedef struct {
    char name[32];
    char param_names[MAX_PARAMS][32]; // e.g. :SIZE :ANGLE ...
    int param_count;
    char body[8192];
} Procedure;

// A variable's value is a number, a word (single-token string, no
// spaces), or a list — see the "Words & lists" section of
// docs/LANGUAGE.md. Arithmetic (parse_expr and everything built on it,
// in interpreter.c) coerces a word by reading the number its text starts
// with, falling back to 0 if it doesn't start with one at all (a list
// coerces to 0, having no meaningful numeric reading).
typedef enum {
    VALUE_NUMBER,
    VALUE_WORD,
    VALUE_LIST,
} ValueType;

// A variable binding: a global (MAKE "name value / :name) or one entry in
// a procedure call's local Scope.
typedef struct {
    char name[32];
    ValueType type;
    double number;
    char word[512];
    int list_head; // type == VALUE_LIST: index into LogoApp.list_pool, -1 = empty list
} Variable;

// One element of a Logo list ([a [b c] d]): a number, a word, or a
// nested sublist, linked to the next element of the same list — a
// persistent singly-linked cons cell in a fixed pool (LogoApp.list_pool)
// rather than a malloc'd tree, to stay consistent with this codebase's
// no-dynamic-allocation style. Nodes are never mutated after being
// built (the language has no in-place list-mutation op), which is what
// makes that safe: a list value can be freely aliased by index (MAKE "b
// :a just copies a head index), and building a new list (SENTENCE, LIST,
// FPUT, LPUT) only needs to allocate fresh top-level nodes for its own
// new shape, reusing any nested sublist's existing nodes by index
// instead of deep-copying them.
typedef enum {
    LIST_ELEM_NUMBER,
    LIST_ELEM_WORD,
    LIST_ELEM_LIST,
} ListElemType;

typedef struct {
    ListElemType type;
    double number;    // type == LIST_ELEM_NUMBER
    char word[512];    // type == LIST_ELEM_WORD
    int sublist_head;   // type == LIST_ELEM_LIST: index into list_pool, -1 = empty
    int next;             // index of the next node in this same list, -1 = end
} ListNode;

// A local scope pushed for one procedure call: its parameters bound to
// their argument values. :name lookups search the scope stack from the
// innermost call outward before falling back to the globals, so a
// parameter shadows any same-named variable from an outer call or global
// — see the "Variables & scoping" section of docs/LANGUAGE.md.
typedef struct {
    Variable vars[MAX_PARAMS];
    int count;
} Scope;

// All interpreter state plus the GTK widgets that display it.
typedef struct LogoApp {
    // Multiple turtles: turtles[0..turtle_count-1] all exist and are all
    // drawn; every FD/RT/SETXY/etc. command controls turtles[current_turtle]
    // (see TELL). Only turtle 0 exists until TELL creates more, so a
    // script that never uses TELL behaves exactly as if there were only
    // ever one turtle.
    Turtle turtles[MAX_TURTLES];
    int turtle_count;
    int current_turtle;

    LineSegment lines[MAX_LINES];
    int line_count;

    Procedure procedures[MAX_PROCEDURES];
    int proc_count;

    Variable variables[MAX_VARIABLES]; // globals
    int var_count;

    Scope scopes[MAX_SCOPE_DEPTH]; // one per active (possibly recursive) call
    int scope_depth;

    // Backing storage for every list value in the program (see ListNode
    // above): a bump allocator, never reclaimed — same "generously sized,
    // loud error if exceeded" policy as every other fixed buffer here.
    ListNode list_pool[MAX_LIST_NODES];
    int list_pool_count;

    // Canvas background color (0.0-1.0 per channel; set via
    // SETBACKGROUND/SETBG). A canvas-wide property, not the turtle's.
    double bg_r, bg_g, bg_b;

    // REPL command history (up/down arrow recall in the entry box).
    char history[MAX_HISTORY][8192];
    int history_count;
    int history_pos; // index currently shown; == history_count means "live" (not browsing)
    char history_draft[8192]; // in-progress text saved when browsing starts

    GtkWidget *window;
    GtkWidget *drawing_area;
    GtkWidget *text_view;
    GtkWidget *entry;

    GtkCssProvider *css_provider;
    int font_size;

    // Where append_output sends text: the real history pane in ui.c's
    // logo_activate, or a plain-buffer sink in tests, which keeps
    // interpreter.c free of any GTK *display* dependency and lets the
    // interpreter core be tested headlessly (see tests/test_interpreter.c).
    void (*output_sink)(struct LogoApp *app, const char *text);
} LogoApp;

#endif // LOGO_TYPES_H
