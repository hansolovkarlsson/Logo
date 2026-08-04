#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINES 10000
#define MAX_PROCEDURES 50
#define MAX_VARIABLES 100
#define MAX_WHILE_ITERATIONS 1000000

typedef struct {
    double x1, y1, x2, y2;
} LineSegment;

typedef struct {
    double x, y;
    double angle;
    int pen_down;
} Turtle;

// Structure to hold a user-defined procedure (TO ... END)
typedef struct {
    char name[32];
    char param_name[32]; // Single param support (e.g. :SIZE)
    char body[2048];
} Procedure;

// Global variable (MAKE "name value)
typedef struct {
    char name[32];
    double value;
} Variable;

typedef struct {
    Turtle turtle;
    LineSegment lines[MAX_LINES];
    int line_count;

    Procedure procedures[MAX_PROCEDURES];
    int proc_count;

    Variable variables[MAX_VARIABLES];
    int var_count;

    GtkWidget *drawing_area;
    GtkWidget *text_view;
    GtkWidget *entry;

    GtkCssProvider *css_provider;
    int font_size;
} LogoApp;

// Forward Declarations
void eval_logo(LogoApp *app, const char *code);

// --- HELPER FUNCTIONS ---

static const char* skip_whitespace(const char *str) {
    while (*str && isspace((unsigned char)*str)) str++;
    return str;
}

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

static void move_turtle_forward(LogoApp *app, double distance) {
    double rad = (app->turtle.angle - 90.0) * M_PI / 180.0;
    double new_x = app->turtle.x + distance * cos(rad);
    double new_y = app->turtle.y + distance * sin(rad);

    if (app->turtle.pen_down && app->line_count < MAX_LINES) {
        app->lines[app->line_count++] = (LineSegment){
            app->turtle.x, app->turtle.y, new_x, new_y
        };
    }

    app->turtle.x = new_x;
    app->turtle.y = new_y;
}

// Procedure Symbol Table Lookup
Procedure* find_procedure(LogoApp *app, const char *name) {
    for (int i = 0; i < app->proc_count; i++) {
        if (strcasecmp(app->procedures[i].name, name) == 0) {
            return &app->procedures[i];
        }
    }
    return NULL;
}

// Variable Symbol Table (MAKE / :name)
static double get_var(LogoApp *app, const char *name) {
    for (int i = 0; i < app->var_count; i++) {
        if (strcasecmp(app->variables[i].name, name) == 0) {
            return app->variables[i].value;
        }
    }
    return 0;
}

static void set_var(LogoApp *app, const char *name, double value) {
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

// Simple text replacement for parameter binding (e.g. replace :SIZE with 100)
void substitute_param(const char *body, const char *param, const char *val, char *output, size_t out_size) {
    output[0] = '\0';
    if (strlen(param) == 0) {
        snprintf(output, out_size, "%s", body);
        return;
    }

    const char *pos = body;
    const char *match;
    size_t param_len = strlen(param);

    while ((match = strstr(pos, param)) != NULL) {
        strncat(output, pos, match - pos);
        strcat(output, val);
        pos = match + param_len;
    }
    strcat(output, pos);
}

// --- EXPRESSION EVALUATION (numbers, :variables, + - * / (), comparisons) ---

static double parse_expr(LogoApp *app, const char **ptr);

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

// Relational expression used by IF/IFELSE, e.g. :X > 10, :X = :Y, :X <> 0
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
static void append_output(LogoApp *app, const char *text) {
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

void eval_logo(LogoApp *app, const char *code) {
    const char *ptr = code;

    while (*ptr != '\0') {
        ptr = skip_whitespace(ptr);
        if (*ptr == '\0') break;

        char token[64] = {0};
        int read_bytes = 0;

        if (sscanf(ptr, "%63s%n", token, &read_bytes) != 1) break;
        ptr += read_bytes;

        // 1. PROCEDURE DEFINITION: TO <NAME> [:PARAM] ... END
        if (strcasecmp(token, "TO") == 0) {
            if (app->proc_count >= MAX_PROCEDURES) break;

            Procedure *proc = &app->procedures[app->proc_count];
            memset(proc, 0, sizeof(Procedure));

            // Read Procedure Name
            if (sscanf(ptr, "%31s%n", proc->name, &read_bytes) == 1) {
                ptr += read_bytes;
                ptr = skip_whitespace(ptr);

                // Optional Parameter (starts with ':')
                if (*ptr == ':') {
                    sscanf(ptr, "%31s%n", proc->param_name, &read_bytes);
                    ptr += read_bytes;
                }

                // Extract body until END
                const char *end_ptr = strcasestr(ptr, "END");
                if (end_ptr != NULL) {
                    size_t body_len = end_ptr - ptr;
                    if (body_len < sizeof(proc->body)) {
                        strncpy(proc->body, ptr, body_len);
                        proc->body[body_len] = '\0';
                    }
                    ptr = end_ptr + 3; // Advance past "END"
                    app->proc_count++;
                }
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
                char val_str[32] = "";
                // Read argument if procedure expects a parameter
                if (strlen(proc->param_name) > 0) {
                    double arg_val = parse_expr(app, &ptr);
                    snprintf(val_str, sizeof(val_str), "%g", arg_val);
                }

                // Substitute parameter into body and evaluate recursively
                char bound_body[2048];
                substitute_param(proc->body, proc->param_name, val_str, bound_body, sizeof(bound_body));
                eval_logo(app, bound_body);
            }
        }
    }
}

// --- GTK DRAWING & CALLBACKS ---

static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_set_line_width(cr, 2.0);

    for (int i = 0; i < app->line_count; i++) {
        cairo_move_to(cr, app->lines[i].x1, app->lines[i].y1);
        cairo_line_to(cr, app->lines[i].x2, app->lines[i].y2);
    }
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_translate(cr, app->turtle.x, app->turtle.y);
    cairo_rotate(cr, app->turtle.angle * M_PI / 180.0);
    
    cairo_set_source_rgb(cr, 0.1, 0.7, 0.3);
    cairo_move_to(cr, 0, -10);
    cairo_line_to(cr, 7, 10);
    cairo_line_to(cr, -7, 10);
    cairo_close_path(cr);
    cairo_fill(cr);
    
    cairo_restore(cr);
}

static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (strlen(text) == 0) return;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, "> ", -1);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);

    eval_logo(app, text);

    gtk_widget_queue_draw(app->drawing_area);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

// --- TEXT SIZE MENU ---

#define MIN_FONT_SIZE 8
#define MAX_FONT_SIZE 40
#define DEFAULT_FONT_SIZE 14

static void apply_font_size(LogoApp *app) {
    char css[64];
    snprintf(css, sizeof(css), ".logo-text { font-size: %dpx; }", app->font_size);
    gtk_css_provider_load_from_string(app->css_provider, css);
}

static void action_increase_text_size(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;
    if (app->font_size < MAX_FONT_SIZE) app->font_size += 2;
    apply_font_size(app);
}

static void action_decrease_text_size(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;
    if (app->font_size > MIN_FONT_SIZE) app->font_size -= 2;
    apply_font_size(app);
}

static void action_reset_text_size(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;
    app->font_size = DEFAULT_FONT_SIZE;
    apply_font_size(app);
}

static void activate(GtkApplication *app, gpointer user_data) {
    LogoApp *logo = g_new0(LogoApp, 1);
    logo->turtle = (Turtle){.x = 250, .y = 250, .angle = 0, .pen_down = 1};

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Logo Turtle Engine with Procedures");
    gtk_window_set_default_size(GTK_WINDOW(window), 1010, 500);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_window_set_child(GTK_WINDOW(window), paned);

    logo->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo->drawing_area, 500, 500);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(logo->drawing_area), draw_cb, logo, NULL);
    gtk_paned_set_start_child(GTK_PANED(paned), logo->drawing_area);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    GtkWidget *repl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_hexpand(repl_box, TRUE);
    gtk_widget_set_size_request(repl_box, 500, -1);

    logo->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logo->text_view), FALSE);
    gtk_widget_add_css_class(logo->text_view, "logo-text");
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), logo->text_view);
    gtk_widget_set_vexpand(scroll, TRUE);

    logo->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(logo->entry), "Enter command or TO procedure...");
    gtk_widget_add_css_class(logo->entry, "logo-text");
    g_signal_connect(logo->entry, "activate", G_CALLBACK(on_entry_activate), logo);

    gtk_box_append(GTK_BOX(repl_box), scroll);
    gtk_box_append(GTK_BOX(repl_box), logo->entry);

    gtk_paned_set_end_child(GTK_PANED(paned), repl_box);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 500);

    // Text size: CSS provider driven by the View menu / accelerators below.
    logo->css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                                GTK_STYLE_PROVIDER(logo->css_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    logo->font_size = DEFAULT_FONT_SIZE;
    apply_font_size(logo);

    static GActionEntry text_size_actions[] = {
        {.name = "increase-text-size", .activate = action_increase_text_size},
        {.name = "decrease-text-size", .activate = action_decrease_text_size},
        {.name = "reset-text-size", .activate = action_reset_text_size},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), text_size_actions,
                                     G_N_ELEMENTS(text_size_actions), logo);

    const char *increase_accels[] = {"<Primary>plus", "<Primary>equal", NULL};
    const char *decrease_accels[] = {"<Primary>minus", NULL};
    const char *reset_accels[] = {"<Primary>0", NULL};
    gtk_application_set_accels_for_action(app, "app.increase-text-size", increase_accels);
    gtk_application_set_accels_for_action(app, "app.decrease-text-size", decrease_accels);
    gtk_application_set_accels_for_action(app, "app.reset-text-size", reset_accels);

    GMenu *menu_bar = g_menu_new();
    GMenu *view_menu = g_menu_new();
    g_menu_append(view_menu, "Increase Text Size", "app.increase-text-size");
    g_menu_append(view_menu, "Decrease Text Size", "app.decrease-text-size");
    g_menu_append(view_menu, "Reset Text Size", "app.reset-text-size");
    g_menu_append_submenu(menu_bar, "View", G_MENU_MODEL(view_menu));
    gtk_application_set_menubar(app, G_MENU_MODEL(menu_bar));
    g_object_unref(view_menu);
    g_object_unref(menu_bar);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.logo.procedures", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

