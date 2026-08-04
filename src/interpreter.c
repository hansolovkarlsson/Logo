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

// --- HELPER FUNCTIONS ---

// Advance past leading whitespace.
static const char* skip_whitespace(const char *str) {
    while (*str && isspace((unsigned char)*str)) str++;
    return str;
}

// Parse a bracketed [ ... ] block, honoring nested brackets, and copy its
// contents into buffer. Returns the position just past the closing ']',
// or NULL if str doesn't start with '['.
static const char* extract_block(const char *str, char *buffer, size_t buf_size) {
    str = skip_whitespace(str);
    if (*str != '[') return NULL;

    str++;
    int depth = 1;
    size_t idx = 0;

    while (*str && depth > 0) {
        if (*str == '[') depth++;
        else if (*str == ']') depth--;

        if (depth > 0) {
            if (idx < buf_size - 1) buffer[idx++] = *str;
            str++;
        }
    }

    buffer[idx] = '\0';
    if (*str == ']') str++;
    return str;
}

// Clamp a color channel to the valid 0.0-1.0 range Cairo expects.
static double clamp01(double v) {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

// Move the turtle by `distance` along its current heading, recording a
// line segment (in the turtle's current pen color) if the pen is down.
static void move_turtle_forward(LogoApp *app, double distance) {
    double rad = (app->turtle.angle - 90.0) * M_PI / 180.0;
    double new_x = app->turtle.x + distance * cos(rad);
    double new_y = app->turtle.y + distance * sin(rad);

    if (app->turtle.pen_down && app->line_count < MAX_LINES) {
        app->lines[app->line_count++] = (LineSegment){
            .x1 = app->turtle.x, .y1 = app->turtle.y,
            .x2 = new_x, .y2 = new_y,
            .r = app->turtle.pen_r, .g = app->turtle.pen_g, .b = app->turtle.pen_b,
        };
    }

    app->turtle.x = new_x;
    app->turtle.y = new_y;
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

// Look up a variable's value (case-insensitive). Searches the scope stack
// from the innermost active call outward, so a procedure's own parameter
// shadows a same-named variable from an outer call or a global, then
// falls back to the globals. 0 if the name isn't bound anywhere.
static double get_var(LogoApp *app, const char *name) {
    for (int s = app->scope_depth - 1; s >= 0; s--) {
        Scope *scope = &app->scopes[s];
        for (int i = 0; i < scope->count; i++) {
            if (strcasecmp(scope->vars[i].name, name) == 0) {
                return scope->vars[i].value;
            }
        }
    }
    for (int i = 0; i < app->var_count; i++) {
        if (strcasecmp(app->variables[i].name, name) == 0) {
            return app->variables[i].value;
        }
    }
    return 0;
}

// Set a variable's value. Updates the binding in the innermost scope (or
// outer call, or global) where `name` already exists — the same
// inside-out search as get_var, so MAKE on a procedure's own parameter
// name updates that parameter rather than creating an unrelated global.
// If `name` isn't bound anywhere yet, creates it as a new global.
static void set_var(LogoApp *app, const char *name, double value) {
    for (int s = app->scope_depth - 1; s >= 0; s--) {
        Scope *scope = &app->scopes[s];
        for (int i = 0; i < scope->count; i++) {
            if (strcasecmp(scope->vars[i].name, name) == 0) {
                scope->vars[i].value = value;
                return;
            }
        }
    }
    for (int i = 0; i < app->var_count; i++) {
        if (strcasecmp(app->variables[i].name, name) == 0) {
            app->variables[i].value = value;
            return;
        }
    }
    if (app->var_count < MAX_VARIABLES) {
        snprintf(app->variables[app->var_count].name, sizeof(app->variables[0].name), "%s", name);
        app->variables[app->var_count].value = value;
        app->var_count++;
    }
}

// --- EXPRESSION EVALUATION (numbers, :variables, + - * / (), comparisons) ---

static double parse_expr(LogoApp *app, const char **ptr);

// Parse a single value: a parenthesized expression, a unary +/-, a
// :variable reference, or a numeric literal.
static double parse_factor(LogoApp *app, const char **ptr) {
    *ptr = skip_whitespace(*ptr);

    if (**ptr == '(') {
        (*ptr)++;
        double val = parse_expr(app, ptr);
        *ptr = skip_whitespace(*ptr);
        if (**ptr == ')') (*ptr)++;
        return val;
    }
    if (**ptr == '-') {
        (*ptr)++;
        return -parse_factor(app, ptr);
    }
    if (**ptr == '+') {
        (*ptr)++;
        return parse_factor(app, ptr);
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
        return get_var(app, name);
    }

    char *end;
    double val = strtod(*ptr, &end);
    *ptr = end;
    return val;
}

// Parse a sequence of factors joined by * and /.
static double parse_term(LogoApp *app, const char **ptr) {
    double val = parse_factor(app, ptr);
    for (;;) {
        *ptr = skip_whitespace(*ptr);
        if (**ptr == '*') {
            (*ptr)++;
            val *= parse_factor(app, ptr);
        } else if (**ptr == '/') {
            (*ptr)++;
            double divisor = parse_factor(app, ptr);
            val = (divisor != 0) ? val / divisor : 0;
        } else {
            break;
        }
    }
    return val;
}

// Parse a sequence of terms joined by + and -. This is the entry point
// for any argument that expects a number.
static double parse_expr(LogoApp *app, const char **ptr) {
    double val = parse_term(app, ptr);
    for (;;) {
        *ptr = skip_whitespace(*ptr);
        if (**ptr == '+') {
            (*ptr)++;
            val += parse_term(app, ptr);
        } else if (**ptr == '-') {
            (*ptr)++;
            val -= parse_term(app, ptr);
        } else {
            break;
        }
    }
    return val;
}

// Parse a relational expression used by IF/IFELSE/WHILE, e.g. :X > 10,
// :X = :Y, :X <> 0. With no relational operator, falls back to the
// expression's truthiness (non-zero = true).
static double parse_condition(LogoApp *app, const char **ptr) {
    double left = parse_expr(app, ptr);
    *ptr = skip_whitespace(*ptr);

    if (**ptr == '<' || **ptr == '>' || **ptr == '=') {
        char op1 = **ptr;
        (*ptr)++;
        char op2 = '\0';
        if ((op1 == '<' && (**ptr == '=' || **ptr == '>')) || (op1 == '>' && **ptr == '=')) {
            op2 = **ptr;
            (*ptr)++;
        }
        double right = parse_expr(app, ptr);

        if (op1 == '=') return left == right;
        if (op1 == '<' && op2 == '=') return left <= right;
        if (op1 == '<' && op2 == '>') return left != right;
        if (op1 == '<') return left < right;
        if (op1 == '>' && op2 == '=') return left >= right;
        return left > right;
    }

    return left != 0;
}

// Append text to the history pane and scroll it into view.
void append_output(LogoApp *app, const char *text) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text, -1);

    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(app->text_view), mark, 0.0, FALSE, 0, 0);
    gtk_text_buffer_delete_mark(buffer, mark);
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

        char token[64] = {0};
        int read_bytes = 0;

        if (sscanf(ptr, "%63s%n", token, &read_bytes) != 1) break;
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

                // Zero or more parameters (each starts with ':')
                while (*ptr == ':' && (proc == NULL || proc->param_count < MAX_PARAMS)) {
                    char param_buf[32];
                    sscanf(ptr, "%31s%n", param_buf, &read_bytes);
                    ptr += read_bytes;
                    if (proc != NULL) {
                        snprintf(proc->param_names[proc->param_count], sizeof(proc->param_names[0]), "%s", param_buf);
                        proc->param_count++;
                    }
                    ptr = skip_whitespace(ptr);
                }

                // Extract body until END
                const char *end_ptr = strcasestr(ptr, "END");
                if (end_ptr != NULL) {
                    if (proc != NULL) {
                        size_t body_len = end_ptr - ptr;
                        if (body_len < sizeof(proc->body)) {
                            strncpy(proc->body, ptr, body_len);
                            proc->body[body_len] = '\0';
                        }
                    } else {
                        append_output(app, "Too many procedures defined, TO ignored\n");
                    }
                    ptr = end_ptr + 3; // Advance past "END"
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
                }
            }
        }
        // 1c. LOAD "path — read a file and run it as Logo source
        else if (strcasecmp(token, "LOAD") == 0) {
            char path_buf[256] = {0};
            if (sscanf(ptr, "%255s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
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
            }
        }
        // 1d. SAVE "path — write all defined procedures out as Logo source
        else if (strcasecmp(token, "SAVE") == 0) {
            char path_buf[256] = {0};
            if (sscanf(ptr, "%255s%n", path_buf, &read_bytes) == 1 && path_buf[0] == '"') {
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
            }
        }
        // 2. REPEAT LOOPS
        else if (strcasecmp(token, "REPEAT") == 0) {
            int count = (int)parse_expr(app, &ptr);
            char block_body[1024];
            ptr = extract_block(ptr, block_body, sizeof(block_body));

            if (ptr != NULL) {
                for (int i = 0; i < count; i++) {
                    eval_logo(app, block_body);
                }
            }
        }
        // 2b. WHILE LOOPS: WHILE <cond> [block]
        else if (strcasecmp(token, "WHILE") == 0) {
            const char *cond_start = ptr;
            double cond = parse_condition(app, &ptr);
            size_t cond_len = (size_t)(ptr - cond_start);

            char cond_text[256] = {0};
            if (cond_len >= sizeof(cond_text)) cond_len = sizeof(cond_text) - 1;
            memcpy(cond_text, cond_start, cond_len);
            cond_text[cond_len] = '\0';

            char block_body[1024] = {0};
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
            }
        }
        // 3. BASIC TURTLE COMMANDS
        else if (strcasecmp(token, "FORWARD") == 0 || strcasecmp(token, "FD") == 0) {
            double val = parse_expr(app, &ptr);
            move_turtle_forward(app, val);
        }
        else if (strcasecmp(token, "BACK") == 0 || strcasecmp(token, "BK") == 0) {
            double val = parse_expr(app, &ptr);
            move_turtle_forward(app, -val);
        }
        else if (strcasecmp(token, "RIGHT") == 0 || strcasecmp(token, "RT") == 0) {
            double val = parse_expr(app, &ptr);
            app->turtle.angle += val;
        }
        else if (strcasecmp(token, "LEFT") == 0 || strcasecmp(token, "LT") == 0) {
            double val = parse_expr(app, &ptr);
            app->turtle.angle -= val;
        }
        // 3b. VARIABLES: MAKE "name expr
        else if (strcasecmp(token, "MAKE") == 0) {
            char varname[64] = {0};
            if (sscanf(ptr, "%63s%n", varname, &read_bytes) == 1 && varname[0] == '"') {
                ptr += read_bytes;
                double val = parse_expr(app, &ptr);
                set_var(app, varname + 1, val);
            }
        }
        // 3c. CONDITIONALS: IF <cond> [block] (ELSE [block])   or   IFELSE <cond> [block] [block]
        else if (strcasecmp(token, "IF") == 0 || strcasecmp(token, "IFELSE") == 0) {
            double cond = parse_condition(app, &ptr);

            char true_body[1024] = {0};
            const char *after_true = extract_block(ptr, true_body, sizeof(true_body));

            if (after_true != NULL) {
                ptr = after_true;
                char false_body[1024] = {0};
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

                if (cond != 0) {
                    eval_logo(app, true_body);
                } else if (has_false) {
                    eval_logo(app, false_body);
                }
            }
        }
        else if (strcasecmp(token, "CLEAR") == 0 || strcasecmp(token, "CS") == 0) {
            app->line_count = 0;
            app->turtle.x = 250;
            app->turtle.y = 250;
            app->turtle.angle = 0;
        }
        else if (strcasecmp(token, "PENUP") == 0 || strcasecmp(token, "PU") == 0) {
            app->turtle.pen_down = 0;
        }
        else if (strcasecmp(token, "PENDOWN") == 0 || strcasecmp(token, "PD") == 0) {
            app->turtle.pen_down = 1;
        }
        // 3c'. SETPENCOLOR r g b — each channel 0-255, applies to lines drawn from now on
        else if (strcasecmp(token, "SETPENCOLOR") == 0 || strcasecmp(token, "SETPC") == 0) {
            double r = parse_expr(app, &ptr);
            double g = parse_expr(app, &ptr);
            double b = parse_expr(app, &ptr);
            app->turtle.pen_r = clamp01(r / 255.0);
            app->turtle.pen_g = clamp01(g / 255.0);
            app->turtle.pen_b = clamp01(b / 255.0);
        }
        // 3d. OUTPUT: PRINT "word   or   PRINT <expr>
        else if (strcasecmp(token, "PRINT") == 0 || strcasecmp(token, "PR") == 0) {
            ptr = skip_whitespace(ptr);
            char line[128];

            if (*ptr == '"') {
                ptr++;
                size_t i = 0;
                while (*ptr && !isspace((unsigned char)*ptr) && i < sizeof(line) - 1) {
                    line[i++] = *ptr++;
                }
                line[i] = '\0';
            } else {
                double val = parse_expr(app, &ptr);
                snprintf(line, sizeof(line), "%g", val);
            }

            append_output(app, line);
            append_output(app, "\n");
        }
        // 4. USER-DEFINED PROCEDURE CALL
        else {
            Procedure *proc = find_procedure(app, token);
            if (proc != NULL) {
                // Evaluate each argument in the *caller's* scope, before
                // pushing the callee's new one.
                double arg_vals[MAX_PARAMS];
                for (int p = 0; p < proc->param_count; p++) {
                    arg_vals[p] = parse_expr(app, &ptr);
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
                        scope->vars[p].value = arg_vals[p];
                    }
                    app->scope_depth++;

                    eval_logo(app, proc->body);

                    app->scope_depth--;
                }
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
