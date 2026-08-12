#ifndef INTERPRETER_H
#define INTERPRETER_H

// interpreter.h
//
// Public interface to the Logo language core: running code (eval_logo),
// checking whether a REPL input is syntactically complete enough to run
// yet (is_input_complete), and writing to the history pane
// (append_output). Everything else in interpreter.c — tokenizing,
// expression parsing, the variable/procedure tables — is implementation
// detail private to that file, EXCEPT the handful of pure state-
// manipulation helpers below (turtle movement, variable lookup/set,
// RANDOM's own RNG) exposed specifically for eval.c (Stage 1's
// tree-walking AST evaluator — see docs/BYTECODE_VM_DESIGN.md) to call
// directly, rather than reimplementing this same logic fresh: both
// engines share the exact same turtle/variable state and behavior this
// way (WRAP/FENCE edge handling, scope-stack lookup order), which
// matters for diffing their output against each other during the
// migration. None of interpreter.c's actual *parsing* (tokenizing,
// eval_logo's own text-driven dispatch) is exposed — only the state
// underneath it.

#include "logo_types.h"

// Run a chunk of Logo source against app's interpreter state.
void eval_logo(LogoApp *app, const char *code);

// True once `text`'s brackets are balanced and every TO has a matching
// END, i.e. it's ready to be run rather than needing another input line.
gboolean is_input_complete(const char *text);

// Append text to the history pane and scroll it into view.
void append_output(LogoApp *app, const char *text);

// Build a Logo-source rendering of every currently-defined procedure
// (each as TO ... END), readable back in by LOAD or File > Open. Returns
// a newly g_malloc'd string the caller must g_free.
char *serialize_procedures(LogoApp *app);

// Reset `t` to the default turtle state: home position, heading 0, pen
// down, default color/width. Used for turtle 0 at startup (ui.c) and any
// turtle TELL creates on first use.
void init_turtle(LogoApp *app, Turtle *t);

// Async-signal-safe: only sets a flag (checked everywhere eval_logo's
// loops already check stop_requested/throw_requested) asking any
// currently-running script to stop at the next opportunity, no matter
// how deeply nested in procedure calls/loops/busy-waits it is. Call
// from a SIGINT handler (see main.c) to give Ctrl+C in the terminal a
// way to interrupt a script that's run away -- eval_logo running
// synchronously means there's otherwise no way to get its attention
// short of killing the whole process.
void request_interrupt(void);

// --- Shared state helpers for eval.c (see the file comment above) ---

// The current turtle (app->turtles[app->current_turtle]).
Turtle *current_turtle(LogoApp *app);

// Move the current turtle directly to an absolute position, recording
// a line segment if the pen is down and applying the current edge mode
// (WRAP/FENCE/WINDOW).
void move_turtle_to(LogoApp *app, double new_x, double new_y);

// Queues a redraw, then drains pending GLib main-context events
// non-blockingly -- call before reading any continuously-updated
// state (mouse position/button, joystick) so a tight WHILE loop built
// around nothing but reads of it still visibly tracks changes instead
// of starving GTK's own event controllers. NULL-safe/no-op headless.
void refresh_before_read(LogoApp *app);

// Record a line segment in the current turtle's pen color/width, if its
// pen is down -- without moving the turtle. Used directly by ARC,
// which draws around the turtle rather than moving it (move_turtle_to
// itself uses this internally for every ordinary move).
void record_line(LogoApp *app, double x1, double y1, double x2, double y2);

// Move the current turtle by `distance` along its current heading.
void move_turtle_forward(LogoApp *app, double distance);

// The canvas center -- where HOME/CLEAR/CS send the turtle.
double home_x(LogoApp *app);
double home_y(LogoApp *app);

// Look up a variable by name, searching `ss`'s own scope stack
// (innermost active scope outward) before falling back to app's
// globals (NULL if never MAKE'n). `ss` is app_scope_stack(app) for
// eval_logo/ast_eval, or vm_scope_stack(vm) (vm.c) for the bytecode VM
// -- see ScopeStack's own comment in logo_types.h for why there are two.
Variable *find_var(LogoApp *app, ScopeStack ss, const char *name);

// Bind `name` to a number/word/list value, creating it (as a global) if
// it doesn't already exist as a local in `ss`'s own current scope or a
// global. set_var_list just copies the head index -- safe aliasing,
// since list nodes (see ListNode in logo_types.h) are never mutated
// after being built.
void set_var(LogoApp *app, ScopeStack ss, const char *name, double value);
void set_var_word(LogoApp *app, ScopeStack ss, const char *name, const char *word);
void set_var_list(LogoApp *app, ScopeStack ss, const char *name, int list_head);

// Bind `name` to an array -- unlike set_var_list, a genuine alias to
// *mutable* shared storage: MAKE "b :a then SETITEM 1 :b 99 also
// changes what :a sees, since both variables now point at the same
// list_pool cells. Arrays are this language's one deliberate exception
// to every other value being immutable/copy-on-build.
void set_var_array(LogoApp *app, ScopeStack ss, const char *name, int start, int length);

// app's own scope stack (app->scopes/app->scope_depth, capped at
// MAX_SCOPE_DEPTH) wrapped as a ScopeStack -- what eval_logo/ast_eval
// pass to every function above; vm.c has its own analogous
// vm_scope_stack (vm.c-internal, not exposed here).
ScopeStack app_scope_stack(LogoApp *app);

// RANDOM n's own RNG (seeded from the current time on first use).
double random_below(double n);

// RERANDOM: reseeds random_below's own RNG to a fixed, reproducible
// value instead of the clock.
void logo_rerandom(void);

// Allocates a fresh node from app->list_pool (see ListNode in
// logo_types.h), or -1 if the pool is full. list_node_copy copies an
// existing node's payload (type/number/word/sublist_head) into a
// freshly allocated one, for building a new top-level list spine
// without mutating (or aliasing into) whatever list `src_idx` came
// from -- neither function touches interpreter.c's private Value type
// at all, just plain ListNode/int, which is what makes them safe to
// expose the same way as the turtle/variable helpers above.
int list_alloc_node(LogoApp *app);
int list_node_copy(LogoApp *app, int src_idx);

// Finds a stored SETPROP entry by plist name + property key (case-
// insensitive), or -1 if none. app->plist_entries/plist_entry_count
// (see PlistEntry in logo_types.h) are plain public fields already --
// this is the one bit of lookup logic worth sharing rather than
// re-deriving, same reasoning as find_var above.
int find_plist_entry(LogoApp *app, const char *plist_name, const char *key);

// The index of an unused slot in app->file_channels (see FileChannel
// in logo_types.h), or -1 if all MAX_OPEN_FILES are already open. Used
// by OPENREAD/OPENWRITE/OPENAPPEND.
int find_free_file_channel(LogoApp *app);

#endif // INTERPRETER_H
