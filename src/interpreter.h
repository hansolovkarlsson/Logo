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

// Move the current turtle by `distance` along its current heading.
void move_turtle_forward(LogoApp *app, double distance);

// The canvas center -- where HOME/CLEAR/CS send the turtle.
double home_x(LogoApp *app);
double home_y(LogoApp *app);

// Look up a variable by name, searching the innermost active scope
// outward before falling back to globals (NULL if never MAKE'n).
Variable *find_var(LogoApp *app, const char *name);

// Bind `name` to a number/word value, creating it (as a global) if it
// doesn't already exist as a local in the current scope or a global.
void set_var(LogoApp *app, const char *name, double value);
void set_var_word(LogoApp *app, const char *name, const char *word);

// RANDOM n's own RNG (seeded from the current time on first use).
double random_below(double n);

#endif // INTERPRETER_H
