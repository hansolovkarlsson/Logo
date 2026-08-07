#ifndef LOGO_TYPES_H
#define LOGO_TYPES_H

// logo_types.h
//
// Shared data structures and limits for the Logo interpreter: the turtle,
// drawn line segments, user-defined procedures, variables, and the overall
// LogoApp struct (interpreter state plus the GTK widgets that display it),
// passed around by both interpreter.c and ui.c.

#include <gtk/gtk.h>

// The canvas's logical size — matches ui.c's drawing_area size request.
// Shared by interpreter.c (WRAP/FENCE boundary checks, FILL's fill
// requests) and ui.c (the actual Cairo rendering/rasterizing).
#define CANVAS_WIDTH 500.0
#define CANVAS_HEIGHT 500.0

#define MAX_LINES 10000
#define MAX_LABELS 1000
#define MAX_RASTER_OPS 500
#define MAX_PROCEDURES 50
#define MAX_VARIABLES 100
#define MAX_WHILE_ITERATIONS 1000000
#define MAX_PARAMS 8
#define MAX_SCOPE_DEPTH 200
// Deliberately much lower than MAX_SCOPE_DEPTH: each nested RUN carries
// a 4KB local text buffer that ordinary procedure-call recursion
// doesn't, so it costs far more stack per level -- confirmed by
// AddressSanitizer overflowing around depth ~186 when this reused
// MAX_SCOPE_DEPTH (200). RUN self-reference is always a mistake in the
// Logo program, never a legitimate deep-recursion technique the way
// procedure recursion can be, so a low cap costs nothing real.
#define MAX_RUN_DEPTH 50
#define MAX_HISTORY 200
#define MAX_TURTLES 10
#define MAX_LIST_NODES 8192
#define MAX_PLIST_ENTRIES 200
#define MAX_TURTLE_SPRITES 20
// Every sprite is scaled into this fixed square box on load (LOADSPRITE)
// — matches how LOADPIC scales to the whole canvas, just at turtle
// scale. No per-sprite variable sizing for now; a turtle's shape is
// always SPRITE_SIZE x SPRITE_SIZE, same as its default triangle is
// always the same fixed size today.
#define SPRITE_SIZE 40.0

// What happens when a turtle's move would cross the canvas edge (see
// WRAP/FENCE/WINDOW in interpreter.c). EDGE_WINDOW is the zero value —
// deliberately, so it's the default: no boundary at all, matching the
// interpreter's behavior before this existed.
typedef enum {
    EDGE_WINDOW,
    EDGE_WRAP,
    EDGE_FENCE,
} EdgeMode;

// One drawn segment of the turtle's trail, in the pen color/width active
// when it was drawn (so a drawing can mix both across SETPENCOLOR and
// SETPENWIDTH calls).
typedef struct {
    double x1, y1, x2, y2;
    double r, g, b; // 0.0-1.0
    double width;
} LineSegment;

// One piece of text drawn via LABEL, at the turtle's position and pen
// color at the time — plain data, no Cairo/GTK type, so interpreter.c
// (which never touches GTK directly) can record these; ui.c's
// draw_scene is what actually renders them.
typedef struct {
    double x, y;
    double r, g, b;
    char text[256];
} Label;

// A canvas-mutating operation that needs to freeze its effect at the
// moment it's called, rather than being recomputed against whatever
// lines happen to exist the next time the canvas redraws: FILL (flood-
// fill the region containing (x, y) with color (r, g, b)) or ERASERECT
// (paint a w-by-h rectangle centered on (x, y) in the background
// color). Both are baked into LogoApp.fill_raster by ui.c's
// bake_pending_fills in the order they were called.
// line_count_at_call freezes how many of LogoApp.lines existed at that
// exact moment, so a fill's flood boundary (or an erase's overwrite)
// only ever sees the lines that existed then.
// RASTER_OP_STAMP (STAMPSPRITE) joins FILL/ERASE_RECT here for the same
// reason: a turtle's shape stamped onto the background needs to freeze
// at the exact moment STAMPSPRITE was called (position, heading, and
// which sprite), not be redrawn later against whatever the turtle's
// current state happens to be.
typedef enum {
    RASTER_OP_FILL,
    RASTER_OP_ERASE_RECT,
    RASTER_OP_STAMP,
} RasterOpKind;

typedef struct {
    RasterOpKind kind;
    double x, y;       // FILL: flood-fill seed point. ERASE_RECT/STAMP: center.
    double r, g, b;     // FILL only: fill color.
    double w, h;        // ERASE_RECT only: rectangle size.
    double angle;       // STAMP only: heading, same convention as Turtle.angle.
    int sprite_index;   // STAMP only: index into LogoApp.sprite_images, -1 = default triangle.
    int sprite_frame;   // STAMP only: which frame of that sprite's grid, same as Turtle.sprite_frame.
    int line_count_at_call;
} RasterOp;

// The turtle's position, heading, pen state, current pen color/width
// (color 0.0-1.0 per channel; set via SETPENCOLOR/SETPC and
// SETPENWIDTH/SETPW), and whether it's drawn at all (SHOWTURTLE/HIDETURTLE).
typedef struct {
    double x, y;
    double angle;
    int pen_down;
    double pen_r, pen_g, pen_b;
    double pen_width;
    int visible;
    // -1 = the default triangle; otherwise an index into
    // LogoApp.sprite_images, set by SETSPRITE (see LOADSPRITE/SETSPRITE/
    // STAMPSPRITE in interpreter.c).
    int sprite_index;
    // Which frame of that sprite's grid is active (SETSPRITEFRAME),
    // 0-indexed row-major. Always 0 for a plain (non-sheet) sprite, and
    // reset to 0 whenever SETSPRITE assigns a (possibly different) shape.
    int sprite_frame;
} Turtle;

// A user-defined procedure (TO ... END).
typedef struct {
    char name[32];
    char param_names[MAX_PARAMS][32]; // e.g. :SIZE :ANGLE ...
    int param_count;
    char body[8192];
} Procedure;

// A variable's value is a number, a word (single-token string, no
// spaces), a list, or an array — see the "Words & lists" and "Arrays"
// sections of docs/LANGUAGE.md. Arithmetic (parse_expr and everything
// built on it, in interpreter.c) coerces a word by reading the number
// its text starts with, falling back to 0 if it doesn't start with one
// at all (a list or array coerces to 0, having no meaningful numeric
// reading).
typedef enum {
    VALUE_NUMBER,
    VALUE_WORD,
    VALUE_LIST,
    VALUE_ARRAY,
} ValueType;

// A variable binding: a global (MAKE "name value / :name) or one entry in
// a procedure call's local Scope.
typedef struct {
    char name[32];
    ValueType type;
    double number;   // type == VALUE_ARRAY: the array's length instead
    char word[512];
    int list_head; // type == VALUE_LIST: index into LogoApp.list_pool, -1 = empty list
                    // type == VALUE_ARRAY: start index of `number` contiguous list_pool
                    // cells (see ARRAY in interpreter.c) -- direct index math, not a
                    // linked chain, which is the whole point of an array over a list.
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

// One key/value pair stored under a named property list (SETPROP/
// GETPROP/REMOVEPROP/PROPLIST) — a flat array scanned linearly by
// plist_name+key, the
// same fixed-pool style as Variable/Procedure above. Real Logo programs
// use property lists as small, sparse records, not index-heavy
// databases, so linear scan is the right tradeoff here, same reasoning
// as Variable's own table. Deliberately mirrors Variable's
// type/number/word/list_head layout (rather than reusing Variable
// itself) since a property is keyed by two names (plist + property),
// not one.
typedef struct {
    char plist_name[32];
    char key[32];
    ValueType type;
    double number;   // type == VALUE_ARRAY: the array's length instead
    char word[512];
    int list_head; // type == VALUE_LIST: index into list_pool
                    // type == VALUE_ARRAY: start index of `number` contiguous cells
} PlistEntry;

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

    Label labels[MAX_LABELS]; // see LABEL
    int label_count;

    RasterOp raster_ops[MAX_RASTER_OPS]; // see FILL, ERASERECT
    int raster_op_count;

    // ui.c's persisted raster bake of FILL/ERASERECT requests already
    // applied (see draw_scene / bake_pending_fills in ui.c) — owned
    // entirely by ui.c, NULL/0 in tests where nothing is ever drawn.
    // Kept here rather than as ui.c statics so it survives across draw
    // calls without any global state, and so a raster_op_count
    // regression (CLEAR resetting it back to 0) is detectable by
    // comparing against raster_ops_baked.
    cairo_surface_t *fill_raster;
    int raster_lines_baked;
    int raster_ops_baked;

    // LOADPIC's loaded background image, pre-scaled to the canvas's
    // exact size and pre-converted to Cairo's premultiplied ARGB32
    // format (see load_canvas_background_image in ui.c) — owned
    // entirely by ui.c, same convention as fill_raster; NULL until
    // LOADPIC succeeds, always NULL in tests (no gdk-pixbuf image
    // decoding without a real GTK app). draw_scene paints this as the
    // base layer, under fill_raster/lines/turtles, whenever it's set.
    cairo_surface_t *bg_image;

    // Named turtle shapes loaded by LOADSPRITE/LOADSPRITESHEET (see
    // load_named_sprite_image in ui.c), decoded at their native
    // resolution and pre-converted to premultiplied ARGB32, same
    // convention as bg_image. sprite_images entries are owned entirely
    // by ui.c and always NULL in tests; the rest are plain data, safe
    // for interpreter.c to read directly (SETSPRITE's name lookup,
    // SETSPRITEFRAME's range check) even though only ui.c ever
    // populates them. sprite_frame_cols/rows slice the loaded image
    // into an even grid of frames -- both 1 for a plain LOADSPRITE
    // (the whole image is "frame 0"), or whatever LOADSPRITESHEET was
    // given for a sprite-sheet blit; draw_turtle_shape in ui.c divides
    // the surface's native width/height by these to find each frame's
    // source rectangle at render time.
    char sprite_names[MAX_TURTLE_SPRITES][32];
    cairo_surface_t *sprite_images[MAX_TURTLE_SPRITES];
    int sprite_frame_cols[MAX_TURTLE_SPRITES];
    int sprite_frame_rows[MAX_TURTLE_SPRITES];
    int sprite_count;

    Procedure procedures[MAX_PROCEDURES];
    int proc_count;

    Variable variables[MAX_VARIABLES]; // globals
    int var_count;

    PlistEntry plist_entries[MAX_PLIST_ENTRIES]; // see SETPROP/GETPROP/REMOVEPROP/PROPLIST
    int plist_entry_count;

    Scope scopes[MAX_SCOPE_DEPTH]; // one per active (possibly recursive) call
    int scope_depth;

    // How many RUNs are currently nested (RUN doesn't push a Scope of its
    // own -- run code shares the caller's scope, unlike a procedure call
    // -- so this is separate from scope_depth, but capped at the same
    // MAX_SCOPE_DEPTH to guard against a self-referential RUN blowing
    // the C call stack, e.g. MAKE "x [RUN :x] / RUN :x).
    int run_depth;

    // Set by OUTPUT/STOP: eval_logo's own loop checks this and stops
    // dead as soon as it's set, so it unwinds up through however many
    // nested blocks (REPEAT/IF/WHILE bodies) sit between the OUTPUT/STOP
    // and the procedure call that's actually meant to stop -- reset back
    // to FALSE by call_procedure once it's actually caught the signal at
    // that boundary. has_output_value/output_* carry OUTPUT's value the
    // same way; a bare STOP leaves has_output_value FALSE. Stored as
    // plain fields (matching Variable's own type/number/word/list_head
    // layout) rather than interpreter.c's private Value type, since this
    // struct is also visible from ui.c.
    gboolean stop_requested;
    gboolean has_output_value;
    ValueType output_type;
    double output_number;
    char output_word[512];
    int output_list_head;

    // Set by THROW; CATCH "tag [...] clears it only if the thrown tag
    // matches its own. Deliberately a *separate* flag from
    // stop_requested, and NOT cleared by call_procedure the way
    // stop_requested is -- a THROW needs to unwind past any number of
    // procedure-call boundaries to reach a matching CATCH (possibly
    // several calls up), unlike STOP/OUTPUT, which always stop exactly
    // one (the immediately enclosing call). eval_depth tracks real
    // eval_logo call nesting (not scope_depth, which only counts
    // procedure calls) so eval_logo can tell when it's back at the true
    // outermost call and report an uncaught THROW instead of silently
    // leaving the flag set forever (which would make every later
    // top-level command a silent no-op).
    gboolean throw_requested;
    char throw_tag[64];
    int eval_depth;

    // Backing storage for every list value in the program (see ListNode
    // above): a bump allocator, never reclaimed — same "generously sized,
    // loud error if exceeded" policy as every other fixed buffer here.
    ListNode list_pool[MAX_LIST_NODES];
    int list_pool_count;

    // Canvas background color (0.0-1.0 per channel; set via
    // SETBACKGROUND/SETBG). A canvas-wide property, not the turtle's.
    double bg_r, bg_g, bg_b;

    // What happens when a turtle's move would cross the canvas edge (see
    // WRAP/FENCE/WINDOW) — a single canvas-wide setting, not per-turtle.
    // EDGE_WINDOW (the zero value, so it's the default) matches the
    // interpreter's original behavior: no boundary at all.
    EdgeMode edge_mode;

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

    // Set by ui.c's logo_activate to gtk_widget_queue_draw the canvas;
    // NULL in tests (there's no window to draw). WAIT calls this before
    // draining events -- without it, nothing is actually queued for that
    // draining to paint mid-script (a whole LOADed file runs as one
    // eval_logo call, and the canvas only redraws once that returns, so
    // WAIT alone wouldn't make any earlier drawing visible first).
    void (*request_redraw)(struct LogoApp *app);

    // Set by ui.c's logo_activate to empty the history pane's text
    // buffer; NULL in tests (there's no text view to clear). CLEARTEXT
    // calls this -- kept as a callback, same as output_sink/
    // request_redraw above, so interpreter.c stays free of any direct
    // GTK dependency.
    void (*clear_history)(struct LogoApp *app);

    // Set by ui.c's logo_activate: LOADPIC's real implementation, using
    // gdk-pixbuf to decode an image file (any format it supports) into
    // the new canvas background (see bg_image above). Returns FALSE on
    // failure (bad path, unrecognized format). NULL in tests -- there's
    // no gdk-pixbuf decoding without a real GTK app, so LOADPIC is a
    // silent no-op there, same convention as clear_history.
    gboolean (*load_background_image)(struct LogoApp *app, const char *path);

    // Set by ui.c's logo_activate: SAVEPIC's real implementation --
    // renders the current canvas (background image, drawing, turtles)
    // to a PNG file at `path`, reusing the same code the File > Export
    // as PNG menu action uses. Returns FALSE on failure. NULL in tests,
    // same convention as load_background_image.
    gboolean (*save_canvas_image)(struct LogoApp *app, const char *path);

    // Set by ui.c's logo_activate: LOADSPRITE/LOADSPRITESHEET's real
    // implementation -- decodes an image file at its native resolution
    // (unlike load_background_image, no scaling to a fixed size; frame
    // slicing happens at render time in ui.c's draw_turtle_shape) and
    // registers it under `name` in sprite_names/sprite_images
    // (overwriting an existing entry with that name, or taking a new
    // slot up to MAX_TURTLE_SPRITES), with the frame grid recorded in
    // sprite_frame_cols/rows. LOADSPRITE calls this with cols=rows=1
    // (a single-frame "sheet"); LOADSPRITESHEET passes whatever grid it
    // was given. Returns FALSE on failure (bad path, unrecognized
    // format, or the sprite table is full). NULL in tests, same
    // convention as load_background_image.
    gboolean (*load_sprite_image)(struct LogoApp *app, const char *name, const char *path, int cols, int rows);
} LogoApp;

#endif // LOGO_TYPES_H
