#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_LINES 1000

typedef struct {
    double x1, y1, x2, y2;
} LineSegment;

typedef struct {
    double x, y;
    double angle; // in degrees
    int pen_down;
} Turtle;

// Application State
typedef struct {
    Turtle turtle;
    LineSegment lines[MAX_LINES];
    int line_count;
    GtkWidget *drawing_area;
    GtkWidget *text_view;
    GtkWidget *entry;
} LogoApp;

// Drawing callback for GtkDrawingArea (Cairo)
static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;

    // Clear Background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // Render Executed Lines
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_set_line_width(cr, 2.0);

    for (int i = 0; i < app->line_count; i++) {
        cairo_move_to(cr, app->lines[i].x1, app->lines[i].y1);
        cairo_line_to(cr, app->lines[i].x2, app->lines[i].y2);
    }
    cairo_stroke(cr);

    // Draw Turtle (Simple Triangle)
    cairo_save(cr);
    cairo_translate(cr, app->turtle.x, app->turtle.y);
    cairo_rotate(cr, app->turtle.angle * M_PI / 180.0);
    
    cairo_set_source_rgb(cr, 0.0, 0.6, 0.2); // Green turtle
    cairo_move_to(cr, 0, -10);
    cairo_line_to(cr, 7, 10);
    cairo_line_to(cr, -7, 10);
    cairo_close_path(cr);
    cairo_fill(cr);
    
    cairo_restore(cr);
}

// Simple Logo REPL Evaluator
void execute_command(LogoApp *app, const char *cmd) {
    char token[32];
    double val = 0;

    if (sscanf(cmd, "%s %lf", token, &val) >= 1) {
        if (strcasecmp(token, "FORWARD") == 0 || strcasecmp(token, "FD") == 0) {
            double rad = (app->turtle.angle - 90) * M_PI / 180.0;
            double new_x = app->turtle.x + val * cos(rad);
            double new_y = app->turtle.y + val * sin(rad);

            if (app->turtle.pen_down && app->line_count < MAX_LINES) {
                app->lines[app->line_count++] = (LineSegment){app->turtle.x, app->turtle.y, new_x, new_y};
            }
            app->turtle.x = new_x;
            app->turtle.y = new_y;
        } else if (strcasecmp(token, "RIGHT") == 0 || strcasecmp(token, "RT") == 0) {
            app->turtle.angle += val;
        } else if (strcasecmp(token, "LEFT") == 0 || strcasecmp(token, "LT") == 0) {
            app->turtle.angle -= val;
        } else if (strcasecmp(token, "CLEAR") == 0) {
            app->line_count = 0;
            app->turtle.x = 200;
            app->turtle.y = 200;
            app->turtle.angle = 0;
        }

        // Trigger Canvas Refresh
        gtk_widget_queue_draw(app->drawing_area);
    }
}

// Handler for when user presses Enter in the REPL Entry box
static void on_entry_activate(GtkEntry *entry, gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (strlen(text) == 0) return;

    // Append command to REPL log
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, "> ", -1);
    gtk_text_buffer_insert(buffer, &end, text, -1);
    gtk_text_buffer_insert(buffer, &end, "\n", -1);

    // Evaluate in Logo Interpreter
    execute_command(app, text);

    // Clear input box
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
}

static void activate(GtkApplication *app, gpointer user_data) {
    LogoApp *logo = g_new0(LogoApp, 1);
    logo->turtle = (Turtle){.x = 200, .y = 200, .angle = 0, .pen_down = 1};

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Logo Turtle REPL");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 450);

    // Main Paned Layout (Left: Canvas, Right: REPL)
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_window_set_child(GTK_WINDOW(window), paned);

    // 1. Canvas Setup
    logo->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo->drawing_area, 400, 400);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(logo->drawing_area), draw_cb, logo, NULL);
    gtk_paned_set_start_child(GTK_PANED(paned), logo->drawing_area);

    // 2. REPL Container (Vertical box: Output Log + Input Field)
    GtkWidget *repl_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    
    logo->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logo->text_view), FALSE);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), logo->text_view);
    gtk_widget_set_vexpand(scroll, TRUE);

    logo->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(logo->entry), "Type Logo command (e.g. FD 100, RT 90)...");
    g_signal_connect(logo->entry, "activate", G_CALLBACK(on_entry_activate), logo);

    gtk_box_append(GTK_BOX(repl_box), scroll);
    gtk_box_append(GTK_BOX(repl_box), logo->entry);
    
    gtk_paned_set_end_child(GTK_PANED(paned), repl_box);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.logo.repl", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

