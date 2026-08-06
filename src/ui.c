// ui.c
//
// The GTK front end: the turtle canvas draw callback, the REPL entry's
// key handling (deciding when Enter runs a command vs. just adds a
// line), the View menu's text-size actions, and building/wiring the
// whole window in logo_activate.

#include "ui.h"
#include "interpreter.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The real append_output sink: writes to the history pane's text buffer
// and scrolls it into view. Assigned to LogoApp.output_sink in
// logo_activate; interpreter.c never touches GTK widgets directly.
static void history_pane_output_sink(LogoApp *app, const char *text) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, text, -1);

    gtk_text_buffer_get_end_iter(buffer, &end);
    GtkTextMark *mark = gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(app->text_view), mark, 0.0, FALSE, 0, 0);
    gtk_text_buffer_delete_mark(buffer, mark);
}

// The real request_redraw callback: queues a repaint of the turtle
// canvas. Assigned to LogoApp.request_redraw in logo_activate; WAIT is
// what calls this, so a script's drawing so far actually appears before
// a long-running WAIT's pause, rather than staying invisible until the
// whole script (which is what one LOADed file runs as) finishes.
static void queue_canvas_redraw(LogoApp *app) {
    gtk_widget_queue_draw(app->drawing_area);
}

// Flood-fills the region containing (x0, y0) in an ARGB32 pixel buffer
// with fill_pixel, replacing every pixel reachable from there that
// currently matches whatever color is already at (x0, y0) -- a plain
// 4-directional flood fill, iterative (a heap-allocated stack, not
// actual C recursion) so it can't blow the call stack on a large
// region. Each pixel is marked (and so excluded from being pushed
// again) the moment it's first reached, which bounds the stack at
// exactly one entry per pixel.
static void push_if_match(int *stack_x, int *stack_y, int *sp, unsigned char *data, int stride, int width, int height, int x, int y, uint32_t target_pixel, uint32_t fill_pixel) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    uint32_t *row = (uint32_t *)(data + y * stride);
    if (row[x] != target_pixel) return;
    row[x] = fill_pixel;
    stack_x[*sp] = x;
    stack_y[*sp] = y;
    (*sp)++;
}

static void flood_fill_pixels(unsigned char *data, int stride, int width, int height, int x0, int y0, uint32_t fill_pixel) {
    if (x0 < 0 || x0 >= width || y0 < 0 || y0 >= height) return;
    uint32_t *row0 = (uint32_t *)(data + y0 * stride);
    if (row0[x0] == fill_pixel) return; // already this color -- nothing to do
    uint32_t target_pixel = row0[x0];

    int capacity = width * height; // each pixel is pushed at most once
    int *stack_x = g_malloc_n(capacity, sizeof(int));
    int *stack_y = g_malloc_n(capacity, sizeof(int));
    int sp = 0;

    push_if_match(stack_x, stack_y, &sp, data, stride, width, height, x0, y0, target_pixel, fill_pixel);
    while (sp > 0) {
        sp--;
        int x = stack_x[sp];
        int y = stack_y[sp];
        push_if_match(stack_x, stack_y, &sp, data, stride, width, height, x + 1, y, target_pixel, fill_pixel);
        push_if_match(stack_x, stack_y, &sp, data, stride, width, height, x - 1, y, target_pixel, fill_pixel);
        push_if_match(stack_x, stack_y, &sp, data, stride, width, height, x, y + 1, target_pixel, fill_pixel);
        push_if_match(stack_x, stack_y, &sp, data, stride, width, height, x, y - 1, target_pixel, fill_pixel);
    }

    g_free(stack_x);
    g_free(stack_y);
}

// Ensures app->fill_raster reflects every FILL/ERASERECT request
// recorded so far. Each op is baked in using exactly the lines that
// existed at the moment it was called (RasterOp.line_count_at_call), so
// a line drawn *after* a FILL/ERASERECT can never retroactively change
// what it affected -- unlike recomputing every op from scratch against
// whatever lines currently exist, which is what FILL alone used to do
// (see docs/ROADMAP.md's old note about that tradeoff). A drop in
// raster_op_count (CLEAR/CS resetting it back to 0) is detected by
// comparing against raster_ops_baked and invalidates the whole raster
// so it gets rebuilt from nothing next time a FILL/ERASERECT happens.
static void bake_pending_fills(LogoApp *app) {
    if (app->raster_op_count < app->raster_ops_baked) {
        if (app->fill_raster != NULL) {
            cairo_surface_destroy(app->fill_raster);
            app->fill_raster = NULL;
        }
        app->raster_lines_baked = 0;
        app->raster_ops_baked = 0;
    }
    if (app->raster_ops_baked >= app->raster_op_count) return;

    if (app->fill_raster == NULL) {
        app->fill_raster = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)CANVAS_WIDTH, (int)CANVAS_HEIGHT);
        cairo_t *ocr = cairo_create(app->fill_raster);
        cairo_set_source_rgb(ocr, app->bg_r, app->bg_g, app->bg_b);
        cairo_paint(ocr);
        cairo_destroy(ocr);
    }

    int width = cairo_image_surface_get_width(app->fill_raster);
    int height = cairo_image_surface_get_height(app->fill_raster);

    while (app->raster_ops_baked < app->raster_op_count) {
        RasterOp *op = &app->raster_ops[app->raster_ops_baked];

        if (op->line_count_at_call > app->raster_lines_baked) {
            cairo_t *ocr = cairo_create(app->fill_raster);
            for (int i = app->raster_lines_baked; i < op->line_count_at_call; i++) {
                cairo_set_source_rgb(ocr, app->lines[i].r, app->lines[i].g, app->lines[i].b);
                cairo_set_line_width(ocr, app->lines[i].width);
                cairo_move_to(ocr, app->lines[i].x1, app->lines[i].y1);
                cairo_line_to(ocr, app->lines[i].x2, app->lines[i].y2);
                cairo_stroke(ocr);
            }
            cairo_destroy(ocr);
            app->raster_lines_baked = op->line_count_at_call;
        }

        if (op->kind == RASTER_OP_ERASE_RECT) {
            cairo_t *ocr = cairo_create(app->fill_raster);
            cairo_set_source_rgb(ocr, app->bg_r, app->bg_g, app->bg_b);
            cairo_rectangle(ocr, op->x - op->w / 2.0, op->y - op->h / 2.0, op->w, op->h);
            cairo_fill(ocr);
            cairo_destroy(ocr);
        } else { // RASTER_OP_FILL
            cairo_surface_flush(app->fill_raster);
            unsigned char *data = cairo_image_surface_get_data(app->fill_raster);
            int stride = cairo_image_surface_get_stride(app->fill_raster);

            // Pen colors are already clamped to [0, 1] when set
            // (SETPENCOLOR); fully opaque, so ARGB32's premultiplication
            // is a no-op here.
            unsigned char r = (unsigned char)(op->r * 255.0 + 0.5);
            unsigned char g = (unsigned char)(op->g * 255.0 + 0.5);
            unsigned char b = (unsigned char)(op->b * 255.0 + 0.5);
            uint32_t fill_pixel = ((uint32_t)0xFF << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            flood_fill_pixels(data, stride, width, height, (int)op->x, (int)op->y, fill_pixel);
            cairo_surface_mark_dirty(app->fill_raster);
        }

        app->raster_ops_baked++;
    }
}

// Paints the background, every recorded line segment, then every active
// turtle as a triangle (see TELL — turtles[0..turtle_count-1] all exist
// and are all drawn, not just the current one). Shared by the live
// on-screen canvas (draw_cb) and PNG export (export_canvas_to_png), so
// both render identically.
static void draw_scene(LogoApp *app, cairo_t *cr) {
    bake_pending_fills(app);

    if (app->fill_raster != NULL) {
        cairo_set_source_surface(cr, app->fill_raster, 0, 0);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgb(cr, app->bg_r, app->bg_g, app->bg_b);
        cairo_paint(cr);
    }

    // Lines not yet baked into fill_raster (nothing has FILLed since
    // they were drawn) -- always redrawn fresh as plain vector strokes,
    // same as the whole line list was before any FILL ever happened.
    for (int i = app->raster_lines_baked; i < app->line_count; i++) {
        cairo_set_source_rgb(cr, app->lines[i].r, app->lines[i].g, app->lines[i].b);
        cairo_set_line_width(cr, app->lines[i].width);
        cairo_move_to(cr, app->lines[i].x1, app->lines[i].y1);
        cairo_line_to(cr, app->lines[i].x2, app->lines[i].y2);
        cairo_stroke(cr);
    }

    cairo_set_font_size(cr, 14);
    for (int i = 0; i < app->label_count; i++) {
        cairo_set_source_rgb(cr, app->labels[i].r, app->labels[i].g, app->labels[i].b);
        cairo_move_to(cr, app->labels[i].x, app->labels[i].y);
        cairo_show_text(cr, app->labels[i].text);
    }

    for (int i = 0; i < app->turtle_count; i++) {
        if (!app->turtles[i].visible) continue;

        cairo_save(cr);
        cairo_translate(cr, app->turtles[i].x, app->turtles[i].y);
        cairo_rotate(cr, app->turtles[i].angle * M_PI / 180.0);

        cairo_set_source_rgb(cr, 0.1, 0.7, 0.3);
        cairo_move_to(cr, 0, -10);
        cairo_line_to(cr, 7, 10);
        cairo_line_to(cr, -7, 10);
        cairo_close_path(cr);
        cairo_fill(cr);

        cairo_restore(cr);
    }
}

// Cairo draw callback for the turtle canvas.
static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    (void)width;
    (void)height;
    draw_scene((LogoApp *)user_data, cr);
}

// Renders the current scene to an off-screen surface at the canvas's
// actual size and writes it out as a PNG. Returns FALSE on failure.
static gboolean export_canvas_to_png(LogoApp *app, const char *path) {
    int width = gtk_widget_get_width(app->drawing_area);
    int height = gtk_widget_get_height(app->drawing_area);
    if (width <= 0 || height <= 0) {
        width = 500;
        height = 500;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    draw_scene(app, cr);
    cairo_status_t status = cairo_surface_write_to_png(surface, path);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return status == CAIRO_STATUS_SUCCESS;
}

// --- BRACKET MATCHING (entry box) ---

// Given `bracket` positioned exactly at an opening '[', find its
// matching ']' (honoring nesting). Returns FALSE if unmatched.
static gboolean find_matching_bracket_forward(const GtkTextIter *bracket, GtkTextIter *match) {
    GtkTextIter iter = *bracket;
    gtk_text_iter_forward_char(&iter); // step past the opening '['
    int depth = 1;
    while (!gtk_text_iter_is_end(&iter)) {
        gunichar ch = gtk_text_iter_get_char(&iter);
        if (ch == '[') {
            depth++;
        } else if (ch == ']') {
            depth--;
            if (depth == 0) {
                *match = iter;
                return TRUE;
            }
        }
        gtk_text_iter_forward_char(&iter);
    }
    return FALSE;
}

// Given `bracket` positioned exactly at a closing ']', find its matching
// '[' (honoring nesting). Returns FALSE if unmatched.
static gboolean find_matching_bracket_backward(const GtkTextIter *bracket, GtkTextIter *match) {
    GtkTextIter iter = *bracket;
    int depth = 1;
    while (!gtk_text_iter_is_start(&iter)) {
        gtk_text_iter_backward_char(&iter);
        gunichar ch = gtk_text_iter_get_char(&iter);
        if (ch == ']') {
            depth++;
        } else if (ch == '[') {
            depth--;
            if (depth == 0) {
                *match = iter;
                return TRUE;
            }
        }
    }
    return FALSE;
}

// Re-highlights bracket matches around the cursor whenever it moves (by
// typing, arrow keys, or a click) — fires on GtkTextBuffer's
// "notify::cursor-position". Looks at the character immediately after
// the cursor first, then immediately before, so the cursor only needs to
// be touching a bracket on either side to trigger a match.
static void update_bracket_match(GtkTextBuffer *buffer, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;

    GtkTextTagTable *tags = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *match_tag = gtk_text_tag_table_lookup(tags, "bracket-match");
    GtkTextTag *nomatch_tag = gtk_text_tag_table_lookup(tags, "bracket-nomatch");

    GtkTextIter buf_start, buf_end;
    gtk_text_buffer_get_bounds(buffer, &buf_start, &buf_end);
    gtk_text_buffer_remove_tag(buffer, match_tag, &buf_start, &buf_end);
    gtk_text_buffer_remove_tag(buffer, nomatch_tag, &buf_start, &buf_end);

    GtkTextIter cursor;
    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));

    GtkTextIter before = cursor;
    gboolean has_before = gtk_text_iter_backward_char(&before);

    GtkTextIter bracket;
    gboolean is_open = FALSE;
    gboolean found = FALSE;

    if (!gtk_text_iter_is_end(&cursor)) {
        gunichar ch = gtk_text_iter_get_char(&cursor);
        if (ch == '[' || ch == ']') {
            bracket = cursor;
            is_open = (ch == '[');
            found = TRUE;
        }
    }
    if (!found && has_before) {
        gunichar ch = gtk_text_iter_get_char(&before);
        if (ch == '[' || ch == ']') {
            bracket = before;
            is_open = (ch == '[');
            found = TRUE;
        }
    }
    if (!found) return;

    GtkTextIter bracket_end = bracket;
    gtk_text_iter_forward_char(&bracket_end);

    GtkTextIter match;
    gboolean matched = is_open
        ? find_matching_bracket_forward(&bracket, &match)
        : find_matching_bracket_backward(&bracket, &match);

    if (matched) {
        GtkTextIter match_end = match;
        gtk_text_iter_forward_char(&match_end);
        gtk_text_buffer_apply_tag(buffer, match_tag, &bracket, &bracket_end);
        gtk_text_buffer_apply_tag(buffer, match_tag, &match, &match_end);
    } else {
        gtk_text_buffer_apply_tag(buffer, nomatch_tag, &bracket, &bracket_end);
    }
}

// --- SYNTAX HIGHLIGHTING (entry box) ---

// Every command eval_logo recognizes, kept in sync with interpreter.c by
// hand — there's no shared source of truth to generate this from without
// exposing eval_logo's internal token dispatch.
static gboolean is_known_keyword(const char *token) {
    static const char *keywords[] = {
        "TO", "END", "ERASE", "LOAD", "SAVE",
        "REPEAT", "WHILE", "IF", "IFELSE", "ELSE", "AND", "OR", "NOT",
        "FORWARD", "FD", "BACK", "BK", "RIGHT", "RT", "LEFT", "LT",
        "SETXY", "SETHEADING", "SETH", "HOME", "ARC", "TELL",
        "MAKE", "PENUP", "PU", "PENDOWN", "PD",
        "SETPENCOLOR", "SETPC", "SETPENWIDTH", "SETPW",
        "SETBACKGROUND", "SETBG", "CLEAR", "CS",
        "PRINT", "PR",
        "FIRST", "BUTFIRST", "LAST", "COUNT",
        NULL,
    };
    for (int i = 0; keywords[i] != NULL; i++) {
        if (strcasecmp(token, keywords[i]) == 0) return TRUE;
    }
    return FALSE;
}

// True if `token` names one of app's currently-defined procedures, so
// calls to your own TO/END definitions get keyword coloring too.
static gboolean is_user_procedure(LogoApp *app, const char *token) {
    for (int i = 0; i < app->proc_count; i++) {
        if (strcasecmp(app->procedures[i].name, token) == 0) return TRUE;
    }
    return FALSE;
}

// Applies `tag` to the buffer range [tok_start, tok_end), given as byte
// offsets from `text_start` into the whole-buffer text. Assumes ASCII
// content (byte offset == character offset), same as the rest of the
// interpreter's parsing.
static void apply_syntax_tag(GtkTextBuffer *buffer, GtkTextTag *tag,
                              const char *text_start, const char *tok_start, const char *tok_end) {
    GtkTextIter start_iter, end_iter;
    gtk_text_buffer_get_iter_at_offset(buffer, &start_iter, (int)(tok_start - text_start));
    gtk_text_buffer_get_iter_at_offset(buffer, &end_iter, (int)(tok_end - text_start));
    gtk_text_buffer_apply_tag(buffer, tag, &start_iter, &end_iter);
}

// Re-colors the whole entry buffer on every edit: keywords/procedure
// calls, numbers, :variables, and "word literals each get their own
// tag. A small hand-rolled scan, not eval_logo's real tokenizer — this
// only needs to classify spans for coloring, not evaluate anything.
static void update_syntax_highlighting(GtkTextBuffer *buffer, gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;

    GtkTextTagTable *tags = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *keyword_tag = gtk_text_tag_table_lookup(tags, "syntax-keyword");
    GtkTextTag *number_tag = gtk_text_tag_table_lookup(tags, "syntax-number");
    GtkTextTag *variable_tag = gtk_text_tag_table_lookup(tags, "syntax-variable");
    GtkTextTag *word_tag = gtk_text_tag_table_lookup(tags, "syntax-word");

    GtkTextIter buf_start, buf_end;
    gtk_text_buffer_get_bounds(buffer, &buf_start, &buf_end);
    gtk_text_buffer_remove_tag(buffer, keyword_tag, &buf_start, &buf_end);
    gtk_text_buffer_remove_tag(buffer, number_tag, &buf_start, &buf_end);
    gtk_text_buffer_remove_tag(buffer, variable_tag, &buf_start, &buf_end);
    gtk_text_buffer_remove_tag(buffer, word_tag, &buf_start, &buf_end);

    char *text = gtk_text_buffer_get_text(buffer, &buf_start, &buf_end, FALSE);
    const char *p = text;

    while (*p) {
        if (*p == '"') {
            const char *tok_start = p++;
            while (*p && !isspace((unsigned char)*p)) p++;
            apply_syntax_tag(buffer, word_tag, text, tok_start, p);
        } else if (*p == ':') {
            const char *tok_start = p++;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            apply_syntax_tag(buffer, variable_tag, text, tok_start, p);
        } else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)*(p + 1)))) {
            const char *tok_start = p;
            while (*p && (isdigit((unsigned char)*p) || *p == '.')) p++;
            apply_syntax_tag(buffer, number_tag, text, tok_start, p);
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            const char *tok_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            char token[64] = {0};
            size_t len = (size_t)(p - tok_start);
            if (len >= sizeof(token)) len = sizeof(token) - 1;
            memcpy(token, tok_start, len);
            if (is_known_keyword(token) || is_user_procedure(app, token)) {
                apply_syntax_tag(buffer, keyword_tag, text, tok_start, p);
            }
        } else {
            p++; // whitespace, brackets, operators — left uncolored
        }
    }

    g_free(text);
}

// Record a submitted command in the history, skipping an exact repeat of
// the last entry, and reset browsing back to the "live" position.
static void history_push(LogoApp *app, const char *text) {
    if (app->history_count > 0 && strcmp(app->history[app->history_count - 1], text) == 0) {
        app->history_pos = app->history_count;
        return;
    }
    if (app->history_count < MAX_HISTORY) {
        snprintf(app->history[app->history_count], sizeof(app->history[0]), "%s", text);
        app->history_count++;
    } else {
        memmove(app->history[0], app->history[1], (MAX_HISTORY - 1) * sizeof(app->history[0]));
        snprintf(app->history[MAX_HISTORY - 1], sizeof(app->history[0]), "%s", text);
    }
    app->history_pos = app->history_count;
}

// Move history browsing by `direction` (-1 = older, +1 = newer) and load
// the resulting entry into the entry buffer, cursor at the end. Returns
// FALSE (nothing to do, let the default cursor movement happen) if
// there's nowhere to go in that direction.
static gboolean history_recall(LogoApp *app, GtkTextBuffer *buffer, int direction) {
    if (direction < 0) {
        if (app->history_pos == 0) return FALSE;
        if (app->history_pos == app->history_count) {
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            char *draft = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            snprintf(app->history_draft, sizeof(app->history_draft), "%s", draft);
            g_free(draft);
        }
        app->history_pos--;
    } else {
        if (app->history_pos >= app->history_count) return FALSE;
        app->history_pos++;
    }

    const char *text = (app->history_pos == app->history_count)
        ? app->history_draft
        : app->history[app->history_pos];

    gtk_text_buffer_set_text(buffer, text, -1);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_place_cursor(buffer, &end);
    return TRUE;
}

// Handles Enter/Shift+Enter and Up/Down history recall in the command
// entry. Enter runs the accumulated text once it's syntactically complete
// (is_input_complete); otherwise lets GTK insert a newline so the box
// grows and composition continues. Up/Down recall history only when the
// cursor is on the entry's first/last line respectively, so they still
// move the cursor normally while editing a multi-line block.
static gboolean on_entry_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                      GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;

    LogoApp *app = (LogoApp *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->entry));

    GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
    if ((keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) && mods == 0) {
        GtkTextIter cursor;
        gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
        int line = gtk_text_iter_get_line(&cursor);
        int last_line = gtk_text_buffer_get_line_count(buffer) - 1;

        if (keyval == GDK_KEY_Up && line == 0) {
            return history_recall(app, buffer, -1);
        }
        if (keyval == GDK_KEY_Down && line == last_line) {
            return history_recall(app, buffer, 1);
        }
        return FALSE;
    }

    if ((keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter) || (state & GDK_SHIFT_MASK)) {
        return FALSE;
    }

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

        history_push(app, text);

        eval_logo(app, text);
        gtk_widget_queue_draw(app->drawing_area);
    }

    gtk_text_buffer_set_text(buffer, "", -1);
    g_free(text);
    return TRUE; // consume the keypress, don't insert a newline
}

// --- FILE MENU ---

// The *.logo/*.txt filter shared by the Open and Save dialogs.
static GListModel *logo_file_filters(void) {
    GtkFileFilter *logo_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(logo_filter, "Logo scripts");
    gtk_file_filter_add_pattern(logo_filter, "*.logo");
    gtk_file_filter_add_pattern(logo_filter, "*.txt");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, logo_filter);
    g_object_unref(logo_filter);
    return G_LIST_MODEL(filters); // caller owns the returned GListStore
}

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
    GListModel *filters = logo_file_filters();
    gtk_file_dialog_set_filters(dialog, filters);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(app->window), NULL, on_file_open_response, app);
    g_object_unref(dialog);
}

// Finishes the async GtkFileDialog started by action_save_file: writes
// every currently-defined procedure to the chosen file.
static void on_file_save_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    LogoApp *app = (LogoApp *)user_data;

    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, result, &error);
    if (file == NULL) {
        if (error != NULL) g_error_free(error); // includes user cancellation
        return;
    }

    char *content = serialize_procedures(app);
    GError *write_error = NULL;
    if (g_file_replace_contents(file, content, strlen(content), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &write_error)) {
        char *path = g_file_get_path(file);
        append_output(app, "Saved ");
        append_output(app, path != NULL ? path : "file");
        append_output(app, "\n");
        g_free(path);
    } else {
        append_output(app, "Could not save file\n");
        if (write_error != NULL) g_error_free(write_error);
    }

    g_free(content);
    g_object_unref(file);
}

// File > Save… — shows a native save dialog, then hands off to
// on_file_save_response once the user picks a destination.
static void action_save_file(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Logo Script");
    gtk_file_dialog_set_initial_name(dialog, "untitled.logo");
    GListModel *filters = logo_file_filters();
    gtk_file_dialog_set_filters(dialog, filters);
    g_object_unref(filters);

    gtk_file_dialog_save(dialog, GTK_WINDOW(app->window), NULL, on_file_save_response, app);
    g_object_unref(dialog);
}

// Finishes the async GtkFileDialog started by action_export_png: renders
// the current canvas to the chosen file.
static void on_export_png_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    LogoApp *app = (LogoApp *)user_data;

    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, result, &error);
    if (file == NULL) {
        if (error != NULL) g_error_free(error); // includes user cancellation
        return;
    }

    char *path = g_file_get_path(file);
    if (path != NULL && export_canvas_to_png(app, path)) {
        append_output(app, "Exported ");
        append_output(app, path);
        append_output(app, "\n");
    } else {
        append_output(app, "Could not export PNG\n");
    }
    g_free(path);
    g_object_unref(file);
}

// File > Export as PNG… — shows a native save dialog, then hands off to
// on_export_png_response once the user picks a destination.
static void action_export_png(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Canvas as PNG");
    gtk_file_dialog_set_initial_name(dialog, "turtle.png");

    GtkFileFilter *png_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(png_filter, "PNG image");
    gtk_file_filter_add_pattern(png_filter, "*.png");
    GListStore *png_filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(png_filters, png_filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(png_filters));
    g_object_unref(png_filter);
    g_object_unref(png_filters);

    gtk_file_dialog_save(dialog, GTK_WINDOW(app->window), NULL, on_export_png_response, app);
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

// File > Quit — without this, macOS's Cmd-Q is grayed out: GTK's app
// menu shows a Quit item automatically but it has nothing to activate
// until an "app.quit" action actually exists.
static void action_quit(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    g_application_quit(G_APPLICATION(user_data));
}

// Build the main window: the turtle canvas / REPL pane split, the View
// menu and its text-size actions/accelerators, then present it.
void logo_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    LogoApp *logo = g_new0(LogoApp, 1);
    init_turtle(&logo->turtles[0]);
    logo->turtle_count = 1;
    logo->current_turtle = 0;
    logo->bg_r = 1.0;
    logo->bg_g = 1.0;
    logo->bg_b = 1.0;
    logo->output_sink = history_pane_output_sink;
    logo->request_redraw = queue_canvas_redraw;

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

    // Bracket matching: highlight the [ ] pair the cursor is touching, or
    // flag a lone unmatched one. Semi-transparent so it reads fine in
    // both light and dark themes.
    GtkTextBuffer *entry_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logo->entry));
    GdkRGBA match_color;
    gdk_rgba_parse(&match_color, "rgba(90,140,255,0.35)");
    gtk_text_buffer_create_tag(entry_buffer, "bracket-match", "background-rgba", &match_color, NULL);
    GdkRGBA nomatch_color;
    gdk_rgba_parse(&nomatch_color, "rgba(255,80,80,0.35)");
    gtk_text_buffer_create_tag(entry_buffer, "bracket-nomatch", "background-rgba", &nomatch_color, NULL);
    g_signal_connect(entry_buffer, "notify::cursor-position", G_CALLBACK(update_bracket_match), logo);

    // Syntax highlighting: keywords/procedure calls, numbers, :variables,
    // and "word literals each get their own foreground color. Chosen at
    // medium lightness/high saturation so they hold reasonable contrast
    // against both light and dark theme backgrounds.
    GdkRGBA keyword_color, number_color, variable_color, word_color;
    gdk_rgba_parse(&keyword_color, "#7C3AED");  // violet
    gdk_rgba_parse(&number_color, "#059669");   // emerald
    gdk_rgba_parse(&variable_color, "#D97706"); // amber
    gdk_rgba_parse(&word_color, "#DC2626");     // red
    gtk_text_buffer_create_tag(entry_buffer, "syntax-keyword", "foreground-rgba", &keyword_color, NULL);
    gtk_text_buffer_create_tag(entry_buffer, "syntax-number", "foreground-rgba", &number_color, NULL);
    gtk_text_buffer_create_tag(entry_buffer, "syntax-variable", "foreground-rgba", &variable_color, NULL);
    gtk_text_buffer_create_tag(entry_buffer, "syntax-word", "foreground-rgba", &word_color, NULL);
    g_signal_connect(entry_buffer, "changed", G_CALLBACK(update_syntax_highlighting), logo);

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
        {.name = "save-file", .activate = action_save_file},
        {.name = "export-png", .activate = action_export_png},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), file_actions,
                                     G_N_ELEMENTS(file_actions), logo);

    // Registered separately from file_actions since it needs `app` (to
    // quit the application), not `logo`, as its user_data.
    static GActionEntry quit_actions[] = {
        {.name = "quit", .activate = action_quit},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), quit_actions,
                                     G_N_ELEMENTS(quit_actions), app);

    static GActionEntry text_size_actions[] = {
        {.name = "increase-text-size", .activate = action_increase_text_size},
        {.name = "decrease-text-size", .activate = action_decrease_text_size},
        {.name = "reset-text-size", .activate = action_reset_text_size},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), text_size_actions,
                                     G_N_ELEMENTS(text_size_actions), logo);

    // <Primary> doesn't resolve to Cmd on this GTK build (shows as Ctrl in
    // the menu), so use <Meta> directly — this app is macOS-only anyway.
    const char *open_accels[] = {"<Meta>o", NULL};
    const char *save_accels[] = {"<Meta>s", NULL};
    const char *export_png_accels[] = {"<Meta>e", NULL};
    const char *increase_accels[] = {"<Meta>plus", "<Meta>equal", NULL};
    const char *decrease_accels[] = {"<Meta>minus", NULL};
    const char *reset_accels[] = {"<Meta>0", NULL};
    const char *quit_accels[] = {"<Meta>q", NULL};
    gtk_application_set_accels_for_action(app, "app.open-file", open_accels);
    gtk_application_set_accels_for_action(app, "app.save-file", save_accels);
    gtk_application_set_accels_for_action(app, "app.export-png", export_png_accels);
    gtk_application_set_accels_for_action(app, "app.increase-text-size", increase_accels);
    gtk_application_set_accels_for_action(app, "app.decrease-text-size", decrease_accels);
    gtk_application_set_accels_for_action(app, "app.reset-text-size", reset_accels);
    gtk_application_set_accels_for_action(app, "app.quit", quit_accels);

    GMenu *menu_bar = g_menu_new();

    GMenu *file_menu = g_menu_new();
    g_menu_append(file_menu, "Open\xe2\x80\xa6", "app.open-file");
    g_menu_append(file_menu, "Save\xe2\x80\xa6", "app.save-file");
    g_menu_append(file_menu, "Export as PNG\xe2\x80\xa6", "app.export-png");
    g_menu_append(file_menu, "Quit", "app.quit");
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
