#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINES 5000

typedef struct {
    double x1, y1, x2, y2;
} LineSegment;

typedef struct {
    double x, y;
    double angle; // Degrees (0 = facing up)
    int pen_down;
} Turtle;

typedef struct {
    Turtle turtle;
    LineSegment lines[MAX_LINES];
    int line_count;
    GtkWidget *drawing_area;
    GtkWidget *text_view;
    GtkWidget *entry;
} LogoApp;

// Forward declarations
void eval_logo(LogoApp *app, const char *code);

// --- HELPER FUNCTIONS ---

static const char* skip_whitespace(const char *str) {
    while (*str && isspace((unsigned char)*str)) str++;
    return str;
}

static const char* extract_block(const char *str, char *buffer, size_t buf_size) {
    str = skip_whitespace(str);
    if (*str != '[') return NULL;

    str++; // Skip opening '['
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
    if (*str == ']') str++; // Skip closing ']'
    return str;
}

static void move_turtle_forward(LogoApp *app, double distance) {
    // 0 degrees points UP: convert to radians relative to -Y axis
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

// --- RECURSIVE INTERPRETER ---

void eval_logo(LogoApp *app, const char *code) {
    const char *ptr = code;

    while (*ptr != '\0') {
        ptr = skip_whitespace(ptr);
        if (*ptr == '\0') break;

        char token[32] = {0};
        int read_bytes = 0;

        if (sscanf(ptr, "%31s%n", token, &read_bytes) != 1) break;
        ptr += read_bytes;

        // 1. Handle REPEAT Loops
        if (strcasecmp(token, "REPEAT") == 0) {
            int count = 0;
            if (sscanf(ptr, "%d%n", &count, &read_bytes) == 1) {
                ptr += read_bytes;
                char block_body[1024];
                ptr = extract_block(ptr, block_body, sizeof(block_body));
                
                if (ptr != NULL) {
                    for (int i = 0; i < count; i++) {
                        eval_logo(app, block_body);
                    }
                }
            }
        } 
        // 2. Movement Commands
        else if (strcasecmp(token, "FORWARD") == 0 || strcasecmp(token, "FD") == 0) {
            double val = 0;
            if (sscanf(ptr, "%lf%n", &val, &read_bytes) == 1) {
                ptr += read_bytes;
                move_turtle_forward(app, val);
            }
        } 
        else if (strcasecmp(token, "BACK") == 0 || strcasecmp(token, "BK") == 0) {
            double val = 0;
            if (sscanf(ptr, "%lf%n", &val, &read_bytes) == 1) {
                ptr += read_bytes;
                move_turtle_forward(app, -val);
            }
        }
        else if (strcasecmp(token, "RIGHT") == 0 || strcasecmp(token, "RT") == 0) {
            double val = 0;
            if (sscanf(ptr, "%lf%n", &val, &read_bytes) == 1) {
                ptr += read_bytes;
                app->turtle.angle += val;
            }
        }
        else if (strcasecmp(token, "LEFT") == 0 || strcasecmp(token, "LT") == 0) {
            double val = 0;
            if (sscanf(ptr, "%lf%n", &val, &read_bytes) == 1) {
                ptr += read_bytes;
                app->turtle.angle -= val;
            }
        }
        // 3. Environment Commands
        else if (strcasecmp(token, "CLEAR") == 0 || strcasecmp(token, "CS") == 0) {
            app->line_count = 0;
            app->turtle.x = 200;
            app->turtle.y = 200;
            app->turtle.angle = 0;
        }
        else if (strcasecmp(token, "PENUP") == 0 || strcasecmp(token, "PU") == 0) {
            app->turtle.pen_down = 0;
        }
        else if (strcasecmp(token, "PENDOWN") == 0 || strcasecmp(token, "PD") == 0) {
            app->turtle.pen_down = 1;
        }
    }
}

// --- GTK DRAWING & CALLBACKS ---

static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;

    // Canvas Background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Render Lines
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_set_line_width(cr, 2.0);

    for (int i = 0; i < app->line_count; i++) {
        cairo_move_to(cr, app->lines[i].x1, app->lines[i].y1);
        cairo_line_to(cr, app->lines[i].x2, app->lines[i].y2);
    }
    cairo_stroke(cr);

    // Draw Turtle Pointer
    cairo_save(cr);
    cairo_translate(cr, app->turtle.x, app->turtle.y);
    cairo_rotate(cr, app->turtle.angle * M_PI / 180.0);
    
    cairo_set_source_rgb(cr, 0.1, 0.7, 0.3); // Turtle Green
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

    // Log to REPL Output
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, "> ", -1);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);

    // Execute through recursive interpreter
    eval_logo(app, text);

    // Redraw Canvas & Clear Input Field
    gtk_widget_queue_draw(app->drawing_area);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void activate(GtkApplication *app, gpointer user_data) {
    LogoApp *logo = g_new0(LogoApp, 1);
    logo->turtle = (Turtle){.x = 250, .y = 250, .angle = 0, .pen_down = 1};

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Logo Turtle Engine");
    gtk_window_set_default_size(GTK_WINDOW(window), 850, 500);

    // Paned Window (Canvas Left | REPL Right)
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_window_set_child(GTK_WINDOW(window), paned);

    // Canvas
    logo->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo->drawing_area, 500, 500);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(logo->drawing_area), draw_cb, logo, NULL);
    gtk_paned_set_start_child(GTK_PANED(paned), logo->drawing_area);

    // REPL UI
    GtkWidget *repl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    
    logo->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logo->text_view), FALSE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), logo->text_view);
    gtk_widget_set_vexpand(scroll, TRUE);

    logo->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(logo->entry), "Enter Logo commands...");
    g_signal_connect(logo->entry, "activate", G_CALLBACK(on_entry_activate), logo);

    gtk_box_append(GTK_BOX(repl_box), scroll);
    gtk_box_append(GTK_BOX(repl_box), logo->entry);
    
    gtk_paned_set_end_child(GTK_PANED(paned), repl_box);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.logo.interpreter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
