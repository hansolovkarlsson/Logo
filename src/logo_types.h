#ifndef LOGO_TYPES_H
#define LOGO_TYPES_H

// logo_types.h
//
// Shared data structures and limits for the Logo interpreter: the turtle,
// drawn line segments, user-defined procedures, variables, and the overall
// LogoApp struct (interpreter state plus the GTK widgets that display it),
// passed around by both interpreter.c and ui.c.

#include <gtk/gtk.h>

// The canvas's logical size at startup (LogoApp.canvas_width/height —
// see below) — SETCANVASSIZE changes it at runtime, matching ui.c's
// drawing_area size request. MIN/MAX_CANVAS_SIZE bound what
// SETCANVASSIZE will accept: small enough that a canvas is never
// degenerate, and capped so a full ARGB32 raster (CANVAS_WIDTH *
// CANVAS_HEIGHT * 4 bytes, see LogoApp.fill_raster) can't balloon into
// a very large allocation from one command.
#define DEFAULT_CANVAS_WIDTH 500.0
#define DEFAULT_CANVAS_HEIGHT 500.0
#define MIN_CANVAS_SIZE 50.0
#define MAX_CANVAS_SIZE 4000.0

// SETPENWIDTH/SETPW's own clamp bounds -- shared here (rather than
// staying a private #define in interpreter.c) so eval.c's own
// do_setpenwidth can use the exact same limits, not a hand-copied
// duplicate that could drift.
#define MIN_PEN_WIDTH 0.5
#define MAX_PEN_WIDTH 20.0

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
// SEND's prototype-chain walk (see resolve_message in interpreter.c):
// an object's "prototype" property points at its parent, so a cyclic
// or absurdly long chain is bounded by this instead of spinning
// forever -- same bounded-loop-as-safety-net precedent as
// MAX_WHILE_ITERATIONS, just a far smaller cap since a real prototype
// hierarchy is never this deep.
#define MAX_PROTOTYPE_CHAIN_DEPTH 20
#define MAX_OPEN_FILES 16
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
    // Which procedure this scope belongs to (set by call_procedure) —
    // read by BACKTRACE/BT to print the current call stack; otherwise
    // unused by ordinary variable lookup.
    char proc_name[32];
} Scope;

// Which scope array a variable lookup/push/pop should operate against —
// find_var/find_or_create_var/set_var*/eval_local_declare/
// eval_push_scope_for_call all take one of these instead of reaching
// into app->scopes/app->scope_depth directly, so the bytecode VM can
// use its own, separate, much deeper scope stack (Vm.scopes/
// Vm.scope_depth, see vm.h) while eval_logo/ast_eval keep using
// LogoApp's own (app_scope_stack, interpreter.h) exactly as before —
// one shared implementation, two independent storage arrays, so the
// two engines still can't drift on scoping *semantics* even though
// their storage capacity now differs. `scope_depth` is a pointer since
// eval_push_scope_for_call must increment it (every other user of this
// struct only reads through it). `capacity` is each stack's own
// recursion-too-deep ceiling — MAX_SCOPE_DEPTH for app's, a much larger
// VM-only constant for the VM's own.
typedef struct {
    Scope *scopes;
    int *scope_depth;
    int capacity;
} ScopeStack;

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

// One entry of LogoApp.file_channels (OPENREAD/OPENWRITE/OPENAPPEND/
// CLOSE/FILEPRINT/READLINE/EOF?) — a channel number is just its index
// into that array. Deliberately whole-file, not a real streaming
// libc FILE*, so file I/O stays built on the same GLib functions
// (g_file_get_contents/g_file_set_contents) LOAD/SAVE already use: a
// read channel loads the entire file up front and serves READLINE out
// of read_buffer/read_pos; a write channel accumulates everything
// written into write_buffer and only actually touches disk when CLOSE
// flushes it (matching Terrapin's own CLOSE warning: "if an output
// stream is not closed, you may lose data").
typedef enum {
    FILE_CHANNEL_CLOSED,
    FILE_CHANNEL_READ,
    FILE_CHANNEL_WRITE,
} FileChannelMode;

typedef struct {
    FileChannelMode mode;
    char *read_buffer;   // READ: whole file content, owned, g_free'd on CLOSE
    size_t read_pos;     // READ: byte offset of the next unread character
    GString *write_buffer; // WRITE: accumulated content, owned, freed on CLOSE
    char path[512];      // WRITE: where to flush write_buffer on CLOSE
} FileChannel;

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

    // SETSPEED's own setting, in seconds -- 0 (the default) means
    // instant/no delay, unchanged from every prior release. When
    // nonzero, the VM (vm.c's OP_MOTION_DELAY, emitted by compiler.c
    // right after FD/BK/RT/LT/SETXY/SETHEADING/SETH/SETX/SETY/HOME/ARC)
    // suspends for this many seconds after each such command, the same
    // real-timer mechanism WAIT itself uses, so the window stays
    // responsive and redraws between steps instead of freezing. See
    // SPEED for the paired getter. Only the bytecode VM (bin/logo's own
    // live engine) implements the actual throttling; eval_logo/ast_eval
    // parse and store this value but never suspend, matching how
    // VM-only features (LAUNCH/AWAIT/YIELD) have been handled since
    // Phase 6.
    double turtle_speed_delay;

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
    FileChannel file_channels[MAX_OPEN_FILES]; // see OPENREAD/OPENWRITE/OPENAPPEND/CLOSE
    int plist_entry_count;

    Scope scopes[MAX_SCOPE_DEPTH]; // one per active (possibly recursive) call
    int scope_depth;

    // How many PAUSEs are currently active, innermost last. Each PAUSE
    // busy-waits (see WAIT's own technique) until CONTINUE/CO drops this
    // back below the level it captured on entry -- see PAUSE/CONTINUE in
    // interpreter.c for the exact protocol.
    int pause_depth;

    // WAITKEY's protocol with the entry box's key-press handler (see
    // on_entry_key_pressed in ui.c): WAITKEY sets waiting_for_key before
    // busy-waiting (WAIT's own technique again), which tells the entry
    // box to intercept the *next* keypress -- whatever it is, including
    // Return/Up/Down, which would otherwise submit or navigate history
    // -- into pending_key instead of its normal handling, and set
    // key_ready to unblock WAITKEY's loop. Plain data, not a callback:
    // always FALSE/unset in headless tests, where there's no entry box
    // to ever set key_ready, so WAITKEY is a silent no-op there instead
    // of busy-waiting forever (same convention as PAUSE's request_redraw
    // check).
    gboolean waiting_for_key;
    gboolean key_ready;
    char pending_key[32];

    // INPUT's own version of the same protocol: unlike WAITKEY, ordinary
    // typing must still work normally (composing the line to submit),
    // so the entry box only special-cases Return/KP_Enter while
    // waiting_for_input is set -- capturing the whole entry text into
    // pending_input and setting input_ready, instead of running it as a
    // command (INPUT's answer is raw text, not Logo source). Same
    // headless-no-op convention as WAITKEY.
    gboolean waiting_for_input;
    gboolean input_ready;
    char pending_input[512];

    // Mouse position/button state (MOUSEPOS/MOUSEX/MOUSEY/BUTTON?) --
    // updated by GTK motion/click controllers on drawing_area (see
    // logo_activate in ui.c), in the same pixel coordinate space
    // SETXY/POS already use (origin top-left, no pause needed at all:
    // unlike WAITKEY/INPUT, this is a passive state query, not
    // something that waits on an event). Those controllers only fire
    // when GTK's main loop actually gets to run an iteration, though --
    // eval_logo's read side (the operators themselves, in
    // interpreter.c) is what forces that to happen on every read via
    // refresh_before_read (queues a redraw, then drains -- the redraw
    // matters too, or a turtle-moving command run earlier in the same
    // WHILE iteration would never actually get painted until the whole
    // loop ends), or a WHILE loop built around nothing but these would
    // never see them update at all. Always 0/FALSE in
    // headless tests -- there are no GTK controllers there to ever
    // update them, same as any other GUI-only state, just without a
    // callback to check first since reading plain data is always safe.
    double mouse_x;
    double mouse_y;
    gboolean mouse_button_down;

    // Joystick/game-controller state (JOYSTICK?/JOYSTICKAXIS/
    // JOYSTICKBUTTON?), Phase 4's one genuinely new dependency (SDL2,
    // linked alongside GTK -- see the Makefile). interpreter.c never
    // touches SDL directly, matching how it stays GTK/Cairo-free too:
    // these three callbacks (set by ui.c's logo_activate, NULL in
    // headless tests) poll fresh state (SDL_JoystickUpdate) each call
    // rather than this being plain continuously-updated data, since
    // nothing else in the app pumps SDL's own event queue on a timer.
    // joystick_handle is an opaque SDL_Joystick* (void* here so this
    // header never needs an SDL include) -- NULL until a joystick is
    // found, lazily opened on first call so a controller plugged in
    // after startup still gets picked up.
    void *joystick_handle;
    gboolean (*joystick_connected)(struct LogoApp *app);
    double (*joystick_axis)(struct LogoApp *app, int axis);
    gboolean (*joystick_button)(struct LogoApp *app, int button);

    // Sound effects/playback (TONE/PLAYSOUND/STOPSOUND) -- Phase 4's
    // other planned addition, but no new library needed: SDL2 (already
    // linked above for the joystick) has its own audio device queue,
    // which is all a synthesized tone or a WAV file playback needs.
    // Fire-and-forget like the joystick/mouse queries above, not a
    // pausing wait like WAIT/PAUSE -- a sound effect keeps playing in
    // the background while the script (and any WHILE-driven animation)
    // keeps running, the way a real game engine's sound effects do.
    // audio_device is an opaque SDL_AudioDeviceID (a plain integer, so
    // no SDL include needed here), 0 meaning "not yet opened" (SDL
    // reserves 0 as its own "invalid device" value) -- lazily opened on
    // first use, same as joystick_handle above.
    guint32 audio_device;
    gboolean (*play_tone)(struct LogoApp *app, double frequency, double seconds);
    gboolean (*play_sound_file)(struct LogoApp *app, const char *path);
    void (*stop_sound)(struct LogoApp *app);

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

    // The canvas's current logical size (SETCANVASSIZE/CANVASSIZE),
    // starting at DEFAULT_CANVAS_WIDTH/HEIGHT. Read directly by
    // interpreter.c (WRAP/FENCE boundary checks, turtle homing) and by
    // ui.c (fill_raster's dimensions, LOADPIC's scale target, and the
    // real drawing_area widget's size via resize_canvas below).
    double canvas_width;
    double canvas_height;

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
    // The horizontal split between drawing_area and the REPL box —
    // stored so resize_canvas below can move the divider to match a new
    // SETCANVASSIZE width (drawing_area's own size_request alone won't
    // grow the paned's start-child allocation).
    GtkWidget *paned;
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

    // Set by ui.c's logo_activate: SETCANVASSIZE's real implementation --
    // resizes drawing_area/paned to match the new canvas_width/height
    // (already updated by the time this is called) and drops bg_image
    // (a LOADPIC'd background no longer matches the new size, and unlike
    // fill_raster there's no lazy-invalidation check for it elsewhere).
    // fill_raster itself needs no attention here: bake_pending_fills
    // already detects SETCANVASSIZE's raster_op_count reset the same way
    // it detects CLEAR's, and rebuilds it at whatever canvas_width/height
    // now is. NULL in tests, same convention as load_background_image.
    void (*resize_canvas)(struct LogoApp *app, double width, double height);
} LogoApp;

#endif // LOGO_TYPES_H
