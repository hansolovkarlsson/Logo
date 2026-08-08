#ifndef INTERPRETER_H
#define INTERPRETER_H

// interpreter.h
//
// Public interface to the Logo language core: running code (eval_logo),
// checking whether a REPL input is syntactically complete enough to run
// yet (is_input_complete), and writing to the history pane
// (append_output). Everything else in interpreter.c — tokenizing,
// expression parsing, the variable/procedure tables — is implementation
// detail private to that file.

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

#endif // INTERPRETER_H
