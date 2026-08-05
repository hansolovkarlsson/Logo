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

// Clamp to an arbitrary [lo, hi] range, e.g. pen width.
static double clamp_range(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

#define MIN_PEN_WIDTH 0.5
#define MAX_PEN_WIDTH 20.0

// Where CLEAR and HOME send the turtle back to.
#define HOME_X 250.0
#define HOME_Y 250.0

// The turtle currently being controlled by FD/RT/SETXY/etc. — see TELL.
static Turtle* current_turtle(LogoApp *app) {
    return &app->turtles[app->current_turtle];
}

// Reset `t` to the default turtle state: home position, heading 0, pen
// down, default color/width.
void init_turtle(Turtle *t) {
    *t = (Turtle){
        .x = HOME_X, .y = HOME_Y, .angle = 0, .pen_down = 1,
        .pen_r = 0.1, .pen_g = 0.1, .pen_b = 0.1, .pen_width = 2.0,
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

// Move the current turtle directly to an absolute position, recording a
// line segment along the way if the pen is down.
static void move_turtle_to(LogoApp *app, double new_x, double new_y) {
    Turtle *t = current_turtle(app);
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
    return NULL;
}

// Look up a variable's numeric value; 0 if it's unbound or word-typed.
// Used throughout arithmetic (parse_factor's :name case), which only
// ever deals in numbers.
static double get_var(LogoApp *app, const char *name) {
    Variable *v = find_var(app, name);
    return (v != NULL && v->type == VALUE_NUMBER) ? v->number : 0;
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

// Set a variable to a word (MAKE "name "word), same binding rules as
// set_var.
static void set_var_word(LogoApp *app, const char *name, const char *word) {
    Variable *v = find_or_create_var(app, name);
    if (v != NULL) {
        v->type = VALUE_WORD;
        snprintf(v->word, sizeof(v->word), "%s", word);
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

// One side of a comparison: a number or a word. Only parse_comparison
// deals in this — arithmetic (parse_expr and below) stays pure-number.
typedef struct {
    gboolean is_word;
    double number;
    char word[128];
} Operand;

// Parse one comparison operand: a "word literal, a :variable (carrying
// whichever type it holds), or a numeric expression.
static Operand parse_operand(LogoApp *app, const char **ptr) {
    *ptr = skip_whitespace(*ptr);
    Operand result = {0};

    if (**ptr == '"') {
        (*ptr)++;
        size_t i = 0;
        while (**ptr && !isspace((unsigned char)**ptr) && i < sizeof(result.word) - 1) {
            result.word[i++] = **ptr;
            (*ptr)++;
        }
        result.word[i] = '\0';
        result.is_word = TRUE;
        return result;
    }

    if (**ptr == ':') {
        const char *lookahead = *ptr + 1;
        char name[32] = {0};
        size_t i = 0;
        while (lookahead[i] && (isalnum((unsigned char)lookahead[i]) || lookahead[i] == '_') && i < sizeof(name) - 1) {
            name[i] = lookahead[i];
            i++;
        }
        name[i] = '\0';

        Variable *v = find_var(app, name);
        if (v != NULL && v->type == VALUE_WORD) {
            *ptr = lookahead + i;
            snprintf(result.word, sizeof(result.word), "%s", v->word);
            result.is_word = TRUE;
            return result;
        }
        // Numeric variable (or unbound, which reads as 0): fall through
        // to normal expression parsing, same as any other numeric term.
    }

    result.number = parse_expr(app, ptr);
    return result;
}

// Render an operand as text for word comparison: a word as-is, a number
// formatted the same way PRINT would show it.
static void operand_to_text(const Operand *v, char *out, size_t out_size) {
    if (v->is_word) {
        snprintf(out, out_size, "%s", v->word);
    } else {
        snprintf(out, out_size, "%g", v->number);
    }
}

// Parse a single relational comparison, e.g. :X > 10, :X = :Y, :X = "hi.
// With no relational operator, falls back to the operand's truthiness
// (non-zero number, or non-empty word = true). The base case for
// parse_condition, below.
static double parse_comparison(LogoApp *app, const char **ptr) {
    Operand left = parse_operand(app, ptr);
    *ptr = skip_whitespace(*ptr);

    if (**ptr == '<' || **ptr == '>' || **ptr == '=') {
        char op1 = **ptr;
        (*ptr)++;
        char op2 = '\0';
        if ((op1 == '<' && (**ptr == '=' || **ptr == '>')) || (op1 == '>' && **ptr == '=')) {
            op2 = **ptr;
            (*ptr)++;
        }
        Operand right = parse_operand(app, ptr);

        if (left.is_word || right.is_word) {
            // Words only support = and <>; a text comparison for the
            // rest wouldn't be meaningful, so they just report unequal.
            char left_text[128], right_text[128];
            operand_to_text(&left, left_text, sizeof(left_text));
            operand_to_text(&right, right_text, sizeof(right_text));
            int equal = strcasecmp(left_text, right_text) == 0;
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

    return left.is_word ? (left.word[0] != '\0') : (left.number != 0);
}

// Peek the next whitespace-delimited word without consuming it unless it
// case-insensitively matches `keyword`, in which case `*ptr` is advanced
// past it. Used to recognize the NOT/AND/OR keywords.
static gboolean consume_keyword(const char **ptr, const char *keyword) {
    const char *lookahead = skip_whitespace(*ptr);
    char word[8] = {0};
    int n = 0;
    if (sscanf(lookahead, "%7s%n", word, &n) == 1 && strcasecmp(word, keyword) == 0) {
        *ptr = lookahead + n;
        return TRUE;
    }
    return FALSE;
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
            } else {
                append_output(app, "LOAD: expected a \"path\n");
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
            } else {
                append_output(app, "SAVE: expected a \"path\n");
            }
        }
        // 2. REPEAT LOOPS
        else if (strcasecmp(token, "REPEAT") == 0) {
            int count = (int)parse_expr(app, &ptr);
            char block_body[1024];
            const char *after_block = extract_block(ptr, block_body, sizeof(block_body));

            if (after_block != NULL) {
                ptr = after_block;
                for (int i = 0; i < count; i++) {
                    eval_logo(app, block_body);
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
            } else {
                append_output(app, "WHILE: expected [ block ]\n");
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
            current_turtle(app)->angle += val;
        }
        else if (strcasecmp(token, "LEFT") == 0 || strcasecmp(token, "LT") == 0) {
            double val = parse_expr(app, &ptr);
            current_turtle(app)->angle -= val;
        }
        else if (strcasecmp(token, "SETXY") == 0) {
            double x = parse_expr(app, &ptr);
            double y = parse_expr(app, &ptr);
            move_turtle_to(app, x, y);
        }
        else if (strcasecmp(token, "SETHEADING") == 0 || strcasecmp(token, "SETH") == 0) {
            current_turtle(app)->angle = parse_expr(app, &ptr);
        }
        // 3a'. ARC angle radius — draws a circle of `radius` centered ON
        // the turtle, starting at its current heading and sweeping
        // through `angle` degrees. The turtle itself doesn't move.
        else if (strcasecmp(token, "ARC") == 0) {
            double angle_deg = parse_expr(app, &ptr);
            double radius = parse_expr(app, &ptr);

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
            int index = (int)parse_expr(app, &ptr);
            if (index < 0 || index >= MAX_TURTLES) {
                char msg[64];
                snprintf(msg, sizeof(msg), "TELL: turtle index must be 0-%d\n", MAX_TURTLES - 1);
                append_output(app, msg);
            } else {
                if (index >= app->turtle_count) {
                    for (int i = app->turtle_count; i <= index; i++) {
                        init_turtle(&app->turtles[i]);
                    }
                    app->turtle_count = index + 1;
                }
                app->current_turtle = index;
            }
        }
        // 3b. VARIABLES: MAKE "name expr   or   MAKE "name "word
        else if (strcasecmp(token, "MAKE") == 0) {
            char varname[64] = {0};
            if (sscanf(ptr, "%63s%n", varname, &read_bytes) == 1 && varname[0] == '"') {
                ptr += read_bytes;
                ptr = skip_whitespace(ptr);

                if (*ptr == '"') {
                    ptr++;
                    char word[128] = {0};
                    size_t i = 0;
                    while (*ptr && !isspace((unsigned char)*ptr) && i < sizeof(word) - 1) {
                        word[i++] = *ptr++;
                    }
                    word[i] = '\0';
                    set_var_word(app, varname + 1, word);
                } else {
                    double val = parse_expr(app, &ptr);
                    set_var(app, varname + 1, val);
                }
            } else {
                append_output(app, "MAKE: expected a \"name\n");
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
            for (int i = 0; i < app->turtle_count; i++) {
                app->turtles[i].x = HOME_X;
                app->turtles[i].y = HOME_Y;
                app->turtles[i].angle = 0;
            }
        }
        else if (strcasecmp(token, "HOME") == 0) {
            move_turtle_to(app, HOME_X, HOME_Y);
            current_turtle(app)->angle = 0;
        }
        else if (strcasecmp(token, "PENUP") == 0 || strcasecmp(token, "PU") == 0) {
            current_turtle(app)->pen_down = 0;
        }
        else if (strcasecmp(token, "PENDOWN") == 0 || strcasecmp(token, "PD") == 0) {
            current_turtle(app)->pen_down = 1;
        }
        // 3c'. SETPENCOLOR r g b — each channel 0-255, applies to lines drawn from now on
        else if (strcasecmp(token, "SETPENCOLOR") == 0 || strcasecmp(token, "SETPC") == 0) {
            double r = parse_expr(app, &ptr);
            double g = parse_expr(app, &ptr);
            double b = parse_expr(app, &ptr);
            Turtle *t = current_turtle(app);
            t->pen_r = clamp01(r / 255.0);
            t->pen_g = clamp01(g / 255.0);
            t->pen_b = clamp01(b / 255.0);
        }
        // 3c''a. SETPENWIDTH width — clamped to [0.5, 20], applies to lines drawn from now on
        else if (strcasecmp(token, "SETPENWIDTH") == 0 || strcasecmp(token, "SETPW") == 0) {
            double width = parse_expr(app, &ptr);
            current_turtle(app)->pen_width = clamp_range(width, MIN_PEN_WIDTH, MAX_PEN_WIDTH);
        }
        // 3c''. SETBACKGROUND r g b — each channel 0-255, the canvas's background color
        else if (strcasecmp(token, "SETBACKGROUND") == 0 || strcasecmp(token, "SETBG") == 0) {
            double r = parse_expr(app, &ptr);
            double g = parse_expr(app, &ptr);
            double b = parse_expr(app, &ptr);
            app->bg_r = clamp01(r / 255.0);
            app->bg_g = clamp01(g / 255.0);
            app->bg_b = clamp01(b / 255.0);
        }
        // 3d. OUTPUT: PRINT "word   PRINT [list of words]   or   PRINT <expr>
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
            } else if (*ptr == '[') {
                // A bracketed list literal prints as its words, single-
                // spaced — it's not evaluated as code the way REPEAT/IF
                // blocks are.
                char list_text[256] = {0};
                const char *after = extract_block(ptr, list_text, sizeof(list_text));
                if (after != NULL) ptr = after;

                line[0] = '\0';
                const char *lp = list_text;
                gboolean first_word = TRUE;
                while (*lp) {
                    lp = skip_whitespace(lp);
                    if (*lp == '\0') break;
                    char word[64] = {0};
                    int n = 0;
                    if (sscanf(lp, "%63s%n", word, &n) != 1 || n == 0) break;
                    if (!first_word) strncat(line, " ", sizeof(line) - strlen(line) - 1);
                    strncat(line, word, sizeof(line) - strlen(line) - 1);
                    first_word = FALSE;
                    lp += n;
                }
            } else if (*ptr == ':') {
                // A bare word-typed variable prints its text; anything
                // else (numeric variable, or :name as part of a larger
                // expression) falls through to normal numeric evaluation.
                const char *lookahead = ptr + 1;
                char name[32] = {0};
                size_t i = 0;
                while (lookahead[i] && (isalnum((unsigned char)lookahead[i]) || lookahead[i] == '_') && i < sizeof(name) - 1) {
                    name[i] = lookahead[i];
                    i++;
                }
                name[i] = '\0';
                const char *after_name = lookahead + i;
                gboolean sole_reference = (*after_name == '\0' || isspace((unsigned char)*after_name));

                Variable *v = sole_reference ? find_var(app, name) : NULL;
                if (v != NULL && v->type == VALUE_WORD) {
                    snprintf(line, sizeof(line), "%s", v->word);
                    ptr = after_name;
                } else {
                    double val = parse_expr(app, &ptr);
                    snprintf(line, sizeof(line), "%g", val);
                }
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
                        scope->vars[p].type = VALUE_NUMBER;
                        scope->vars[p].number = arg_vals[p];
                    }
                    app->scope_depth++;

                    eval_logo(app, proc->body);

                    app->scope_depth--;
                }
            } else {
                append_output(app, "I don't know how to ");
                append_output(app, token);
                append_output(app, "\n");
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
