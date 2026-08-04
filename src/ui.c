// ui.c
//
// The GTK front end: the turtle canvas draw callback, the REPL entry's
// key handling (deciding when Enter runs a command vs. just adds a
// line), the View menu's text-size actions, and building/wiring the
// whole window in logo_activate.

#include "ui.h"
#include "interpreter.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// Cairo draw callback for the turtle canvas: paints the background, every
// recorded line segment, then the turtle itself as a triangle.
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

// Handles Enter/Shift+Enter in the command entry. Runs the accumulated
// text once it's syntactically complete (is_input_complete); otherwise
// lets GTK insert a newline so the box grows and composition continues.
static gboolean on_entry_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                      GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;

    if ((keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter) || (state & GDK_SHIFT_MASK)) {
        return FALSE;
    }

    LogoApp *app = (LogoApp *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->entry));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    if (!is_input_complete(text)) {
        g_free(text);
        return FALSE; // let GTK insert the newline; keep composing
    }

    if (strlen(text) > 0) {
        append_output(app, "> ");
        append_output(app, text);
        append_output(app, "\n");

        eval_logo(app, text);
        gtk_widget_queue_draw(app->drawing_area);
    }

    gtk_text_buffer_set_text(buffer, "", -1);
    g_free(text);
    return TRUE; // consume the keypress, don't insert a newline
}

// --- FILE MENU ---

// Finishes the async GtkFileDialog started by action_open_file: reads the
// chosen file and runs it as Logo source, same as typing it into the REPL.
static void on_file_open_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    LogoApp *app = (LogoApp *)user_data;

    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, &error);
    if (file == NULL) {
        if (error != NULL) g_error_free(error); // includes user cancellation
        return;
    }

    char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (g_file_load_contents(file, NULL, &contents, &length, NULL, &read_error)) {
        char *path = g_file_get_path(file);
        append_output(app, "Loaded ");
        append_output(app, path != NULL ? path : "file");
        append_output(app, "\n");
        g_free(path);

        eval_logo(app, contents);
        gtk_widget_queue_draw(app->drawing_area);
        g_free(contents);
    } else {
        append_output(app, "Could not read file\n");
        if (read_error != NULL) g_error_free(read_error);
    }

    g_object_unref(file);
}

// File > Open… — shows a native file picker, then hands off to
// on_file_open_response once the user picks a file.
static void action_open_file(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open Logo Script");

    GtkFileFilter *logo_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(logo_filter, "Logo scripts");
    gtk_file_filter_add_pattern(logo_filter, "*.logo");
    gtk_file_filter_add_pattern(logo_filter, "*.txt");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, logo_filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(logo_filter);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(app->window), NULL, on_file_open_response, app);
    g_object_unref(dialog);
}

// --- TEXT SIZE MENU ---

#define MIN_FONT_SIZE 8
#define MAX_FONT_SIZE 40
#define DEFAULT_FONT_SIZE 14

// Push app->font_size out to the CSS provider shared by the history pane
// and command entry.
static void apply_font_size(LogoApp *app) {
    char css[64];
    snprintf(css, sizeof(css), ".logo-text { font-size: %dpx; }", app->font_size);
    gtk_css_provider_load_from_string(app->css_provider, css);
}

// View > Increase Text Size
static void action_increase_text_size(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;
    if (app->font_size < MAX_FONT_SIZE) app->font_size += 2;
    apply_font_size(app);
}

// View > Decrease Text Size
static void action_decrease_text_size(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;
    if (app->font_size > MIN_FONT_SIZE) app->font_size -= 2;
    apply_font_size(app);
}

// View > Reset Text Size
static void action_reset_text_size(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;
    app->font_size = DEFAULT_FONT_SIZE;
    apply_font_size(app);
}

// Build the main window: the turtle canvas / REPL pane split, the View
// menu and its text-size actions/accelerators, then present it.
void logo_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    LogoApp *logo = g_new0(LogoApp, 1);
    logo->turtle = (Turtle){.x = 250, .y = 250, .angle = 0, .pen_down = 1};

    GtkWidget *window = gtk_application_window_new(app);
    logo->window = window;
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

    GtkWidget *hint_label = gtk_label_new(
        "Enter to run \xc2\xb7 Shift+Enter for a new line \xc2\xb7 unfinished TO/[ blocks continue automatically");
    gtk_label_set_xalign(GTK_LABEL(hint_label), 0.0);
    gtk_widget_add_css_class(hint_label, "dim-label");

    logo->entry = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(logo->entry), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(logo->entry, "logo-text");

    GtkEventController *entry_key_controller = gtk_event_controller_key_new();
    g_signal_connect(entry_key_controller, "key-pressed", G_CALLBACK(on_entry_key_pressed), logo);
    gtk_widget_add_controller(logo->entry, entry_key_controller);

    GtkWidget *entry_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(entry_scroll), logo->entry);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(entry_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(entry_scroll), TRUE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(entry_scroll), 34);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(entry_scroll), 150);

    gtk_box_append(GTK_BOX(repl_box), scroll);
    gtk_box_append(GTK_BOX(repl_box), hint_label);
    gtk_box_append(GTK_BOX(repl_box), entry_scroll);

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

    static GActionEntry file_actions[] = {
        {.name = "open-file", .activate = action_open_file},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), file_actions,
                                     G_N_ELEMENTS(file_actions), logo);

    static GActionEntry text_size_actions[] = {
        {.name = "increase-text-size", .activate = action_increase_text_size},
        {.name = "decrease-text-size", .activate = action_decrease_text_size},
        {.name = "reset-text-size", .activate = action_reset_text_size},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), text_size_actions,
                                     G_N_ELEMENTS(text_size_actions), logo);

    const char *open_accels[] = {"<Primary>o", NULL};
    const char *increase_accels[] = {"<Primary>plus", "<Primary>equal", NULL};
    const char *decrease_accels[] = {"<Primary>minus", NULL};
    const char *reset_accels[] = {"<Primary>0", NULL};
    gtk_application_set_accels_for_action(app, "app.open-file", open_accels);
    gtk_application_set_accels_for_action(app, "app.increase-text-size", increase_accels);
    gtk_application_set_accels_for_action(app, "app.decrease-text-size", decrease_accels);
    gtk_application_set_accels_for_action(app, "app.reset-text-size", reset_accels);

    GMenu *menu_bar = g_menu_new();

    GMenu *file_menu = g_menu_new();
    g_menu_append(file_menu, "Open\xe2\x80\xa6", "app.open-file");
    g_menu_append_submenu(menu_bar, "File", G_MENU_MODEL(file_menu));
    g_object_unref(file_menu);

    GMenu *view_menu = g_menu_new();
    g_menu_append(view_menu, "Increase Text Size", "app.increase-text-size");
    g_menu_append(view_menu, "Decrease Text Size", "app.decrease-text-size");
    g_menu_append(view_menu, "Reset Text Size", "app.reset-text-size");
    g_menu_append_submenu(menu_bar, "View", G_MENU_MODEL(view_menu));
    g_object_unref(view_menu);

    gtk_application_set_menubar(app, G_MENU_MODEL(menu_bar));
    g_object_unref(menu_bar);

    gtk_window_present(GTK_WINDOW(window));
}
