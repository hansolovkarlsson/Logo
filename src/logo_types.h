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
    char body[2048];
} Procedure;

// A variable's value is either a number or a word (single-token string,
// no spaces) — see the "Words" section of docs/LANGUAGE.md. Arithmetic
// (parse_expr and everything built on it) only ever deals in numbers; a
// word-typed variable read in a numeric context is just 0.
typedef enum {
    VALUE_NUMBER,
    VALUE_WORD,
} ValueType;

// A variable binding: a global (MAKE "name value / :name) or one entry in
// a procedure call's local Scope.
typedef struct {
    char name[32];
    ValueType type;
    double number;
    char word[128];
} Variable;

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

    // Canvas background color (0.0-1.0 per channel; set via
    // SETBACKGROUND/SETBG). A canvas-wide property, not the turtle's.
    double bg_r, bg_g, bg_b;

    // REPL command history (up/down arrow recall in the entry box).
    char history[MAX_HISTORY][2048];
    int history_count;
    int history_pos; // index currently shown; == history_count means "live" (not browsing)
    char history_draft[2048]; // in-progress text saved when browsing starts

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
