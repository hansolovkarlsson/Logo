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

#endif // INTERPRETER_H
