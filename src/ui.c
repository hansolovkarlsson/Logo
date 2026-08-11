// ui.c
//
// The GTK front end: the turtle canvas draw callback, the REPL entry's
// key handling (deciding when Enter runs a command vs. just adds a
// line), the View menu's text-size actions, and building/wiring the
// whole window in logo_activate.

#include "ui.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "compiler.h"
#include "vm.h"
#include "agent.h"
#include <SDL2/SDL.h> // JOYSTICK?/JOYSTICKAXIS/JOYSTICKBUTTON?, TONE/PLAYSOUND/STOPSOUND -- see logo_types.h

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

// The real clear_history callback: empties the history pane's text
// buffer. Assigned to LogoApp.clear_history in logo_activate; CLEARTEXT
// is what calls this.
static void clear_history_pane(LogoApp *app) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->text_view));
    gtk_text_buffer_set_text(buffer, "", -1);
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

// Paints one turtle's shape (a LOADSPRITE/LOADSPRITESHEET/SETSPRITE
// image, or the default triangle) at (x, y) rotated to `angle` — shared
// by draw_scene's live turtle loop and bake_pending_fills' STAMPSPRITE
// baking below, so a stamped copy always looks exactly like the turtle
// did at the moment it was stamped. `frame` selects which cell of the
// sprite's cols-by-rows grid to blit (0 for a plain, non-sheet sprite,
// which is just a 1x1 grid); the source rectangle is that cell's slice
// of the image's native resolution, scaled up/down to fill the fixed
// SPRITE_SIZE x SPRITE_SIZE box — the sprite-sheet blit itself.
static void draw_turtle_shape(LogoApp *app, cairo_t *cr, double x, double y, double angle, int sprite_index, int frame) {
    cairo_save(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, angle * M_PI / 180.0);

    if (sprite_index >= 0 && sprite_index < app->sprite_count && app->sprite_images[sprite_index] != NULL) {
        cairo_surface_t *sheet = app->sprite_images[sprite_index];
        int cols = app->sprite_frame_cols[sprite_index];
        int rows = app->sprite_frame_rows[sprite_index];
        int frame_count = cols * rows;
        int f = ((frame % frame_count) + frame_count) % frame_count;
        double frame_w = (double)cairo_image_surface_get_width(sheet) / cols;
        double frame_h = (double)cairo_image_surface_get_height(sheet) / rows;
        double frame_x = (f % cols) * frame_w;
        double frame_y = (f / cols) * frame_h;

        cairo_translate(cr, -SPRITE_SIZE / 2.0, -SPRITE_SIZE / 2.0);
        cairo_scale(cr, SPRITE_SIZE / frame_w, SPRITE_SIZE / frame_h);
        cairo_rectangle(cr, 0, 0, frame_w, frame_h);
        cairo_clip(cr);
        cairo_set_source_surface(cr, sheet, -frame_x, -frame_y);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgb(cr, 0.1, 0.7, 0.3);
        cairo_move_to(cr, 0, -10);
        cairo_line_to(cr, 7, 10);
        cairo_line_to(cr, -7, 10);
        cairo_close_path(cr);
        cairo_fill(cr);
    }

    cairo_restore(cr);
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
        app->fill_raster = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)app->canvas_width, (int)app->canvas_height);
        cairo_t *ocr = cairo_create(app->fill_raster);
        // A LOADPIC'd background image is the base layer if one's been
        // loaded (see load_canvas_background_image) -- otherwise the
        // flat SETBACKGROUND color, same as before this existed. Note
        // ERASERECT (below) always erases back to the flat color, not
        // this image, in whatever rectangle it covers -- a known,
        // accepted simplification (see docs/ROADMAP.md's known
        // limitations).
        if (app->bg_image != NULL) {
            cairo_set_source_surface(ocr, app->bg_image, 0, 0);
        } else {
            cairo_set_source_rgb(ocr, app->bg_r, app->bg_g, app->bg_b);
        }
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
        } else if (op->kind == RASTER_OP_STAMP) {
            cairo_t *ocr = cairo_create(app->fill_raster);
            draw_turtle_shape(app, ocr, op->x, op->y, op->angle, op->sprite_index, op->sprite_frame);
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

// Converts a decoded GdkPixbuf into a premultiplied ARGB32 Cairo surface
// at the pixbuf's own dimensions — draw_scene's native format, so
// painting it each redraw is a single cairo_paint call rather than a
// decode. Shared by decode_and_scale_image and decode_image_native
// below; does not take ownership of `pixbuf`.
static cairo_surface_t *convert_pixbuf_to_argb32(GdkPixbuf *pixbuf) {
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    unsigned char *dst = cairo_image_surface_get_data(surface);
    int dst_stride = cairo_image_surface_get_stride(surface);

    const unsigned char *src = gdk_pixbuf_get_pixels(pixbuf);
    int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);

    for (int y = 0; y < height; y++) {
        const unsigned char *srow = src + y * src_stride;
        uint32_t *drow = (uint32_t *)(dst + y * dst_stride);
        for (int x = 0; x < width; x++) {
            unsigned char r = srow[x * channels + 0];
            unsigned char g = srow[x * channels + 1];
            unsigned char b = srow[x * channels + 2];
            unsigned char a = has_alpha ? srow[x * channels + 3] : 0xFF;
            // Cairo's ARGB32 stores premultiplied color channels.
            drow[x] = ((uint32_t)a << 24)
                | ((uint32_t)(r * a / 255) << 16)
                | ((uint32_t)(g * a / 255) << 8)
                | (uint32_t)(b * a / 255);
        }
    }
    cairo_surface_mark_dirty(surface);
    return surface;
}

// Decodes any gdk-pixbuf-supported image file, scales it to exactly
// width x height, and converts it into a premultiplied ARGB32 Cairo
// surface. Used by load_canvas_background_image (LOADPIC, canvas-sized)
// — the whole canvas background is always one fixed size, so scaling at
// load time (rather than render time) avoids redoing it every redraw.
// Returns NULL if the file can't be read or decoded.
static cairo_surface_t *decode_and_scale_image(const char *path, int width, int height) {
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (pixbuf == NULL) {
        g_error_free(error);
        return NULL;
    }

    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, width, height, GDK_INTERP_BILINEAR);
    g_object_unref(pixbuf);
    if (scaled == NULL) return NULL;

    cairo_surface_t *surface = convert_pixbuf_to_argb32(scaled);
    g_object_unref(scaled);
    return surface;
}

// Decodes any gdk-pixbuf-supported image file at its own native
// resolution, with no scaling. Used by load_named_sprite_image
// (LOADSPRITE/LOADSPRITESHEET) — a sprite's frame grid divides this
// native surface at render time (draw_turtle_shape), so the load-time
// size must stay whatever the file's own dimensions are, not a fixed
// box. Returns NULL if the file can't be read or decoded.
static cairo_surface_t *decode_image_native(const char *path) {
    GError *error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &error);
    if (pixbuf == NULL) {
        g_error_free(error);
        return NULL;
    }

    cairo_surface_t *surface = convert_pixbuf_to_argb32(pixbuf);
    g_object_unref(pixbuf);
    return surface;
}

// The real load_background_image callback (LOADPIC). Stored
// persistently on the app (like fill_raster) so it survives across
// redraws; a fresh LOADPIC replaces (and frees) whatever was loaded
// before. Returns FALSE, leaving any existing background image
// untouched, if the file can't be read or decoded.
static gboolean load_canvas_background_image(LogoApp *app, const char *path) {
    cairo_surface_t *surface = decode_and_scale_image(path, (int)app->canvas_width, (int)app->canvas_height);
    if (surface == NULL) return FALSE;

    if (app->bg_image != NULL) cairo_surface_destroy(app->bg_image);
    app->bg_image = surface;
    return TRUE;
}

// The real load_sprite_image callback (LOADSPRITE/LOADSPRITESHEET):
// decodes and registers a named turtle shape at its native resolution
// (draw_turtle_shape divides it into a cols-by-rows grid at render
// time — LOADSPRITE always passes cols=rows=1). Overwrites an existing
// entry with the same name (case-insensitive, matching SETSPRITE's own
// lookup), or takes a new slot if there's room. Returns FALSE, leaving
// any existing entry under that name untouched, if the file can't be
// decoded or the sprite table is already full.
static gboolean load_named_sprite_image(LogoApp *app, const char *name, const char *path, int cols, int rows) {
    cairo_surface_t *surface = decode_image_native(path);
    if (surface == NULL) return FALSE;

    int idx = -1;
    for (int i = 0; i < app->sprite_count; i++) {
        if (strcasecmp(app->sprite_names[i], name) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (app->sprite_count >= MAX_TURTLE_SPRITES) {
            cairo_surface_destroy(surface);
            return FALSE;
        }
        idx = app->sprite_count++;
        snprintf(app->sprite_names[idx], sizeof(app->sprite_names[idx]), "%s", name);
    }

    if (app->sprite_images[idx] != NULL) cairo_surface_destroy(app->sprite_images[idx]);
    app->sprite_images[idx] = surface;
    app->sprite_frame_cols[idx] = cols;
    app->sprite_frame_rows[idx] = rows;
    return TRUE;
}

// Paints the background, every recorded line segment, then every active
// turtle's shape (see TELL — turtles[0..turtle_count-1] all exist and
// are all drawn, not just the current one). Shared by the live
// on-screen canvas (draw_cb) and PNG export (export_canvas_to_png), so
// both render identically.
static void draw_scene(LogoApp *app, cairo_t *cr) {
    bake_pending_fills(app);

    if (app->fill_raster != NULL) {
        cairo_set_source_surface(cr, app->fill_raster, 0, 0);
        cairo_paint(cr);
    } else if (app->bg_image != NULL) {
        cairo_set_source_surface(cr, app->bg_image, 0, 0);
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
        draw_turtle_shape(app, cr, app->turtles[i].x, app->turtles[i].y, app->turtles[i].angle, app->turtles[i].sprite_index, app->turtles[i].sprite_frame);
    }
}

// Cairo draw callback for the turtle canvas.
static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)area;
    (void)width;
    (void)height;
    draw_scene((LogoApp *)user_data, cr);
}

// Defined down in the "Bytecode VM execution" section, alongside
// run_logo_script -- forward-declared here since on_canvas_button_
// pressed/released (ONCLICK/ONRELEASE), on_canvas_motion
// (ONMOUSEMOVE), and on_entry_key_pressed/released (ONKEY/ONKEYUP) all
// need to fire a handler well before that section's own definitions
// appear.
static void fire_onkey(LogoApp *app, const char *key_name);
static void fire_onkeyup(LogoApp *app, const char *key_name);
static void fire_onclick(LogoApp *app, double x, double y, int button);
static void fire_onrelease(LogoApp *app, double x, double y, int button);
static void fire_onmousemove(LogoApp *app, double x, double y);

// MOUSEPOS/MOUSEX/MOUSEY: continuously updated from the canvas's own
// motion events, in the same widget-relative pixel coordinates
// SETXY/POS already use (origin top-left) -- no conversion needed.
// ONMOUSEMOVE's own handler (if one is registered) fires alongside
// this, with the same x/y.
static void on_canvas_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, gpointer user_data) {
    (void)controller;
    LogoApp *app = (LogoApp *)user_data;
    app->mouse_x = x;
    app->mouse_y = y;
    fire_onmousemove(app, x, y);
}

// BUTTON?: continuously updated from the canvas's own click events.
// Deliberately not position/button-number specific -- "is any button
// down right now", matching Terrapin's own BUTTON. ONCLICK's own
// handler (if one is registered) fires alongside this, with the
// gesture's actual button number (1/2/3 -- GDK's own left/middle/right
// convention), even though the gesture itself is configured to match
// any button (see gtk_gesture_single_set_button(..., 0) below).
static void on_canvas_button_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    LogoApp *app = (LogoApp *)user_data;
    app->mouse_button_down = TRUE;
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    fire_onclick(app, x, y, button);
}

// ONRELEASE's own handler (if one is registered) fires alongside this,
// same button-number convention as on_canvas_button_pressed's ONCLICK.
static void on_canvas_button_released(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
    (void)n_press;
    LogoApp *app = (LogoApp *)user_data;
    app->mouse_button_down = FALSE;
    int button = (int)gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    fire_onrelease(app, x, y, button);
}

// --- JOYSTICK (SDL2) ---
// Lazily opens the first available joystick and keeps it open across
// calls, closing and forgetting it if unplugged so a later call can
// pick up whatever's connected next. SDL_JoystickUpdate refreshes
// SDL's own internal state -- nothing else in this app pumps SDL's
// event queue on a timer, so every call does it fresh rather than
// relying on stale state.
static SDL_Joystick *ensure_joystick(LogoApp *app) {
    SDL_JoystickUpdate();
    SDL_Joystick *joy = (SDL_Joystick *)app->joystick_handle;
    if (joy != NULL && !SDL_JoystickGetAttached(joy)) {
        SDL_JoystickClose(joy);
        joy = NULL;
        app->joystick_handle = NULL;
    }
    if (joy == NULL && SDL_NumJoysticks() > 0) {
        joy = SDL_JoystickOpen(0);
        app->joystick_handle = joy;
    }
    return joy;
}

static gboolean logo_joystick_connected(LogoApp *app) {
    return ensure_joystick(app) != NULL;
}

// Normalized to -100..100 -- this project's own preference for
// friendlier units over raw hardware ranges (WAIT's seconds instead of
// 60ths, ANIMATESPRITE's plain frame counts), rather than SDL's raw
// Sint16 axis range.
static double logo_joystick_axis(LogoApp *app, int axis) {
    SDL_Joystick *joy = ensure_joystick(app);
    if (joy == NULL || axis < 0 || axis >= SDL_JoystickNumAxes(joy)) return 0;
    return (double)SDL_JoystickGetAxis(joy, axis) / 32768.0 * 100.0;
}

static gboolean logo_joystick_button(LogoApp *app, int button) {
    SDL_Joystick *joy = ensure_joystick(app);
    if (joy == NULL || button < 0 || button >= SDL_JoystickNumButtons(joy)) return FALSE;
    return SDL_JoystickGetButton(joy, button) != 0;
}

// --- SOUND (SDL2 audio device) ---
// A fixed output spec (44.1kHz, signed 16-bit, mono) for the one audio
// device this app ever opens -- TONE synthesizes straight into it, and
// PLAYSOUND converts whatever a WAV file's own format is into it, so
// there's only ever one format to reason about past this point.
#define LOGO_AUDIO_FREQ 44100
#define LOGO_AUDIO_FORMAT AUDIO_S16SYS
#define LOGO_AUDIO_CHANNELS 1

// Lazily opens the device the first time TONE/PLAYSOUND is called and
// keeps it open for the rest of the app's life -- nothing here ever
// closes it, same as ensure_joystick's handle never being closed until
// the controller itself disconnects. 0 is SDL's own reserved
// "invalid device" value, so it doubles as the "not yet opened" flag.
static SDL_AudioDeviceID ensure_audio_device(LogoApp *app) {
    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)app->audio_device;
    if (dev != 0) return dev;
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = LOGO_AUDIO_FREQ;
    want.format = LOGO_AUDIO_FORMAT;
    want.channels = LOGO_AUDIO_CHANNELS;
    want.samples = 2048;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(dev, 0); // unpaused -- queued audio plays right away
    }
    app->audio_device = (guint32)dev;
    return dev;
}

// TONE frequency seconds — synthesizes a sine wave directly in the
// device's own format, so no conversion step is needed the way
// PLAYSOUND's arbitrary WAV files need. SDL_ClearQueuedAudio first: a
// second TONE/PLAYSOUND while one is still playing replaces it rather
// than queueing up behind it, matching STOPSOUND's "only one sound at
// a time" model.
static gboolean logo_play_tone(LogoApp *app, double frequency, double seconds) {
    SDL_AudioDeviceID dev = ensure_audio_device(app);
    if (dev == 0 || frequency <= 0 || seconds <= 0) return FALSE;
    int sample_count = (int)(LOGO_AUDIO_FREQ * seconds);
    Sint16 *samples = malloc((size_t)sample_count * sizeof(Sint16));
    if (samples == NULL) return FALSE;
    for (int i = 0; i < sample_count; i++) {
        double t = (double)i / LOGO_AUDIO_FREQ;
        samples[i] = (Sint16)(sin(2.0 * G_PI * frequency * t) * 16000.0);
    }
    SDL_ClearQueuedAudio(dev);
    SDL_QueueAudio(dev, samples, (Uint32)(sample_count * (int)sizeof(Sint16)));
    free(samples);
    return TRUE;
}

// PLAYSOUND "path — loads a WAV file and queues it on the same device,
// converting sample rate/format/channel count to match if the file's
// own don't already (SDL_LoadWAV keeps whatever the file was encoded
// with; SDL_QueueAudio requires the device's own format).
static gboolean logo_play_sound_file(LogoApp *app, const char *path) {
    SDL_AudioDeviceID dev = ensure_audio_device(app);
    if (dev == 0) return FALSE;
    SDL_AudioSpec wav_spec;
    Uint8 *wav_buf = NULL;
    Uint32 wav_len = 0;
    if (SDL_LoadWAV(path, &wav_spec, &wav_buf, &wav_len) == NULL) {
        return FALSE;
    }
    SDL_ClearQueuedAudio(dev);
    if (wav_spec.freq == LOGO_AUDIO_FREQ && wav_spec.format == LOGO_AUDIO_FORMAT &&
        wav_spec.channels == LOGO_AUDIO_CHANNELS) {
        SDL_QueueAudio(dev, wav_buf, wav_len);
    } else {
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, wav_spec.format, wav_spec.channels, wav_spec.freq,
                               LOGO_AUDIO_FORMAT, LOGO_AUDIO_CHANNELS, LOGO_AUDIO_FREQ) < 0) {
            SDL_FreeWAV(wav_buf);
            return FALSE;
        }
        cvt.len = (int)wav_len;
        cvt.buf = malloc((size_t)(cvt.len * cvt.len_mult));
        if (cvt.buf == NULL) {
            SDL_FreeWAV(wav_buf);
            return FALSE;
        }
        memcpy(cvt.buf, wav_buf, wav_len);
        SDL_ConvertAudio(&cvt);
        SDL_QueueAudio(dev, cvt.buf, (Uint32)cvt.len_cvt);
        free(cvt.buf);
    }
    SDL_FreeWAV(wav_buf);
    return TRUE;
}

static void logo_stop_sound(LogoApp *app) {
    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)app->audio_device;
    if (dev != 0) {
        SDL_ClearQueuedAudio(dev);
    }
}

// Renders the current scene to an off-screen surface at the canvas's
// actual size and writes it out as a PNG. Returns FALSE on failure.
static gboolean export_canvas_to_png(LogoApp *app, const char *path) {
    int width = gtk_widget_get_width(app->drawing_area);
    int height = gtk_widget_get_height(app->drawing_area);
    if (width <= 0 || height <= 0) {
        width = (int)app->canvas_width;
        height = (int)app->canvas_height;
    }

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    draw_scene(app, cr);
    cairo_status_t status = cairo_surface_write_to_png(surface, path);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);

    return status == CAIRO_STATUS_SUCCESS;
}

// The real resize_canvas callback (SETCANVASSIZE): resizes the drawing
// area and moves the paned divider to match, and drops bg_image -- a
// LOADPIC'd background was scaled to the old size and no longer fits.
// fill_raster needs no attention here (see logo_types.h's comment on
// resize_canvas): bake_pending_fills already rebuilds it lazily once
// SETCANVASSIZE resets raster_op_count, the same way it already does
// for CLEAR.
static void resize_canvas_widget(LogoApp *app, double width, double height) {
    gtk_widget_set_size_request(app->drawing_area, (int)width, (int)height);
    if (app->paned != NULL) {
        gtk_paned_set_position(GTK_PANED(app->paned), (int)width);
    }
    if (app->bg_image != NULL) {
        cairo_surface_destroy(app->bg_image);
        app->bg_image = NULL;
    }
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

// --- Bytecode VM execution (see docs/BYTECODE_VM_DESIGN.md's
// suspend/resume design) ---
//
// bin/logo runs scripts through the Stage 2 compiler+VM now, not
// eval_logo directly -- eval_logo itself is untouched and still used
// by tests/test_interpreter.c, just no longer this app's own execution
// path. The one thing eval_logo's own busy-wait loops did that a
// single vm_run call can't by itself is WAIT/WAITKEY's real suspend:
// vm_run returns early, mid-script, and this file is what actually
// waits (a real GLib timer, or the next keypress) before resuming it,
// instead of blocking here the way eval_logo used to.

// The most tokens a single REPL submission or LOADed file may lex into
// -- matches eval.c's own MAX_LOAD_TOKENS (LOAD can pull in a whole
// file, not just one typed line).
#define UI_SCRIPT_MAX_TOKENS 8192

// A script paused on VM_RUN_SUSPENDED_WAIT/VM_RUN_SUSPENDED_WAITKEY/
// VM_RUN_SUSPENDED_INPUT: kept alive across however many GTK callbacks
// it takes to resume (a timer firing, a keypress arriving, a line
// submitted), instead of the single
// call-scoped locals every other vm_run caller (tests/test_vm.c) uses.
// This app only ever runs one script "thread" in one window, so a
// single file-scope pointer -- not anything owned by LogoApp itself --
// is enough: only ui.c ever drives resumption, the same reasoning
// request_redraw's own callback-pointer seam already established for
// interpreter.c reaching back into this file.
typedef struct {
    ParseResult *result;
    BytecodeChunk *chunk;
    Vm *vm;
    // FALSE only for a run spawned by one of the fire_on* functions
    // below, whose result/chunk are borrowed from the corresponding
    // *_owner global (still needed for as long as that handler stays
    // registered) --
    // free_suspended_run must not free them in that case. TRUE for
    // every run_logo_script-spawned run, which always owns its own
    // result/chunk outright.
    gboolean owns_chunk;
} SuspendedRun;

static SuspendedRun *g_suspended_run = NULL;

// ONKEY/ONCLICK/ONMOUSEMOVE/ONKEYUP/ONRELEASE's own retained
// chunk+result, kept alive independently of g_suspended_run's normal
// free-on-finish lifecycle for as long as a handler still needs them
// (see handle_vm_result's own VM_RUN_HALTED case below, and
// release_retained_chunk right after). Five separate slots, not one
// shared one: each handler can validly be registered by a different
// script/submission at a different time, each needing its own chunk
// kept alive -- sharing one slot would silently break whichever
// handler registered first the moment another one's script finishes.
// When two or more happen to point at the very same chunk (the common
// case: one script registers more than one handler in a single run),
// release_retained_chunk's own scan over every OTHER slot avoids a
// double free.
typedef struct {
    ParseResult *result;
    BytecodeChunk *chunk;
} RetainedChunk;
static RetainedChunk g_onkey_owner = {0};
static RetainedChunk g_onclick_owner = {0};
static RetainedChunk g_onmousemove_owner = {0};
static RetainedChunk g_onkeyup_owner = {0};
static RetainedChunk g_onrelease_owner = {0};

#define NUM_EVENT_HANDLER_OWNERS 5

static void release_retained_chunk(RetainedChunk *slot) {
    if (slot->chunk == NULL) return;
    RetainedChunk *all_owners[NUM_EVENT_HANDLER_OWNERS] = {
        &g_onkey_owner, &g_onclick_owner, &g_onmousemove_owner, &g_onkeyup_owner, &g_onrelease_owner
    };
    gboolean shared = FALSE;
    for (int i = 0; i < NUM_EVENT_HANDLER_OWNERS; i++) {
        if (all_owners[i] != slot && all_owners[i]->chunk == slot->chunk) {
            shared = TRUE;
            break;
        }
    }
    if (!shared) {
        free(slot->chunk);
        parse_result_destroy(slot->result);
    }
    slot->chunk = NULL;
    slot->result = NULL;
}

// PAUSE's own paused runs -- deliberately a SEPARATE stack from
// g_suspended_run, not folded into it. WAIT/WAITKEY/INPUT all suspend
// in a way that must block a second concurrent script (see
// run_logo_script's own g_suspended_run != NULL check); PAUSE needs the
// opposite: run_logo_script must keep accepting ordinary submissions
// while one or more PAUSEs are outstanding, since that's the entire
// feature (a command typed while paused can inspect/modify the paused
// call's own live variables -- variable *storage* is already shared
// global state, not per-Vm, so this falls out for free once the
// concurrency-blocking check doesn't also, wrongly, apply here).
// Nesting is supported: a command run while paused can itself hit
// PAUSE again, pushing a second entry with a strictly higher level
// (app->pause_depth only ever increments via PAUSE/decrements via
// CONTINUE), so this is a genuine LIFO stack, not a single slot.
// Fixed-size and generously bounded -- pause nesting is human-driven
// (typing PAUSE-triggering commands interactively) and realistically
// stays tiny; silently dropping (and freeing) an overflow entry rather
// than reporting it robustly is the same "not yet a real limit anyone
// hits" tradeoff already made for MAX_VM_STACK's own overflow handling.
#define MAX_PAUSED_RUNS 32
typedef struct {
    SuspendedRun *run;
    int level;
} PausedRun;
static PausedRun g_paused_runs[MAX_PAUSED_RUNS];
static int g_paused_run_count = 0;

static void free_suspended_run(SuspendedRun *run) {
    free(run->vm);
    if (run->owns_chunk) {
        free(run->chunk);
        parse_result_destroy(run->result);
    }
    free(run);
}

static gboolean on_wait_timeout(gpointer user_data);
static gboolean on_animatesprite_timeout(gpointer user_data);
static void maybe_resume_paused_runs(LogoApp *app);

// Common tail for run_logo_script/on_wait_timeout/on_entry_key_pressed's
// own WAITKEY-resume branch: acts on whatever vm_run/vm_resume* just
// returned, either finishing up (freeing `run`) or arranging the next
// resume. Redrawing unconditionally here (not just on completion)
// matters: a script can move the turtle before hitting a suspend
// point, and the old busy-wait design's own mid-wait redraws (via
// request_redraw) mean users already expect to see that motion right
// away, not only once the whole script finally finishes.
// {handler-name-field, its own retained-chunk owner} pairs -- one per
// registered-event-trigger builtin. Data-driven rather than one
// hand-duplicated block per handler in handle_vm_result below: five
// near-identical copies was already a real repetition smell, and a
// sixth (a future ONMOUSEUP-style addition, say) shouldn't mean a sixth
// copy-paste.
typedef struct {
    char *handler_name; // points into a LogoApp field, e.g. app->onkey_handler
    RetainedChunk *owner;
} EventHandlerSlot;

static void handle_vm_result(LogoApp *app, SuspendedRun *run, VmRunResult result) {
    EventHandlerSlot slots[NUM_EVENT_HANDLER_OWNERS] = {
        { app->onkey_handler, &g_onkey_owner },
        { app->onclick_handler, &g_onclick_owner },
        { app->onmousemove_handler, &g_onmousemove_owner },
        { app->onkeyup_handler, &g_onkeyup_owner },
        { app->onrelease_handler, &g_onrelease_owner },
    };

    // OFFKEY/OFFCLICK/OFFMOUSEMOVE/OFFKEYUP/OFFRELEASE cleared a
    // handler at some point during whatever run just reached this call
    // (or a fresh script never re-registered one at all) -- release the
    // correspondingly retained chunk right away rather than leaving it
    // held onto uselessly until some LATER unrelated event happens to
    // trigger a check.
    for (int i = 0; i < NUM_EVENT_HANDLER_OWNERS; i++) {
        if (slots[i].handler_name[0] == '\0') release_retained_chunk(slots[i].owner);
    }

    gtk_widget_queue_draw(app->drawing_area);
    switch (result) {
        case VM_RUN_HALTED: {
            // If this run's own execution registered any of these
            // handlers (its own chunk's procs[] resolves the handler
            // name now set), its chunk/result must outlive this call
            // instead of being freed below -- see the *_owner globals
            // above. A handler-invocation run's own chunk (owns_chunk
            // FALSE) is always already retained by someone else, never
            // newly adopted here.
            gboolean kept_any = FALSE;
            if (run->owns_chunk) {
                for (int i = 0; i < NUM_EVENT_HANDLER_OWNERS; i++) {
                    if (slots[i].handler_name[0] != '\0' &&
                        bytecode_find_proc_entry(run->chunk, slots[i].handler_name) != NULL) {
                        release_retained_chunk(slots[i].owner);
                        slots[i].owner->result = run->result;
                        slots[i].owner->chunk = run->chunk;
                        kept_any = TRUE;
                    }
                }
            }
            if (kept_any) {
                free(run->vm);
                free(run);
            } else {
                free_suspended_run(run);
            }
            break;
        }
        case VM_RUN_SUSPENDED_WAIT:
            g_suspended_run = run;
            g_timeout_add((guint)(run->vm->suspend_seconds * 1000), on_wait_timeout, app);
            break;
        case VM_RUN_SUSPENDED_WAITKEY:
            g_suspended_run = run;
            app->waiting_for_key = TRUE;
            if (app->request_redraw != NULL) app->request_redraw(app);
            break;
        case VM_RUN_SUSPENDED_INPUT:
            g_suspended_run = run;
            app->waiting_for_input = TRUE;
            if (app->request_redraw != NULL) app->request_redraw(app);
            break;
        case VM_RUN_SUSPENDED_PAUSE:
            if (g_paused_run_count < MAX_PAUSED_RUNS) {
                g_paused_runs[g_paused_run_count].run = run;
                g_paused_runs[g_paused_run_count].level = run->vm->pause_level;
                g_paused_run_count++;
            } else {
                append_output(app, "PAUSE: too many nested pauses, this one was dropped\n");
                free_suspended_run(run);
            }
            break;
        case VM_RUN_SUSPENDED_ANIMATESPRITE:
            // Same "block concurrent submission" slot as WAIT, not the
            // PAUSE-style reentrant stack -- ANIMATESPRITE doesn't want
            // other commands interleaving mid-animation any more than
            // WAIT does.
            g_suspended_run = run;
            g_timeout_add((guint)(run->vm->suspend_seconds * 1000), on_animatesprite_timeout, app);
            break;
        case VM_RUN_SUSPENDED_MOTION_DELAY:
            // SETSPEED's own throttle -- identical to VM_RUN_SUSPENDED_
            // WAIT's own handling (same real timer, same resume
            // callback): from ui.c's own perspective there's no
            // meaningful difference between "the script explicitly
            // asked to wait" and "a motion command's own automatic
            // throttle fired," both just need the window to stay
            // responsive for suspend_seconds before continuing.
            g_suspended_run = run;
            g_timeout_add((guint)(run->vm->suspend_seconds * 1000), on_wait_timeout, app);
            break;
        case VM_RUN_SUSPENDED_LAUNCH: {
            // Phase 6's own first slice (see docs/CONCURRENT_AGENTS_
            // DESIGN.md): a LAUNCH hands off to agent.c's own
            // synchronous scheduler instead of any of the ordinary
            // suspend/resume cases above -- reached identically whether
            // this is a script's own very first vm_run call (via
            // run_logo_script's own ordinary SuspendedRun-wrap-and-
            // dispatch, ever since it stopped special-casing this
            // itself) or a LATER one, after an earlier WAIT/WAITKEY/
            // INPUT/PAUSE/ANIMATESPRITE already suspended and resumed
            // once (or several times) -- handle_vm_result is the one
            // dispatch point every resume path already funnels through,
            // so a single case here covers both, no special-casing
            // needed at either call site. Wraps `run`'s own vm (its
            // already-live scope/throw/run_depth/turtle state captured
            // exactly as it stood the moment LAUNCH suspended it) as the
            // scheduler's "initial" Agent, plus every other agent it or
            // its own descendants LAUNCH along the way, and runs them
            // all to completion before returning -- no further GTK
            // re-entry needed, since this slice never needs a real
            // timer/keypress (see agent.h's own file comment).
            Agent *initial_agent = calloc(1, sizeof(Agent));
            initial_agent->vm = *run->vm; // includes vm's own scopes/scope_depth already -- nothing left to copy separately, see agent.h's own comment
            free(run->vm);
            initial_agent->turtle_index = app->current_turtle;
            initial_agent->throw_requested = app->throw_requested;
            snprintf(initial_agent->throw_tag, sizeof(initial_agent->throw_tag), "%s", app->throw_tag);
            initial_agent->run_depth = app->run_depth;
            initial_agent->state = AGENT_READY;
            initial_agent->started = TRUE;
            scheduler_run(app, &run->result->pool, run->chunk, initial_agent);
            // Same owns_chunk guard as VM_RUN_HALTED above -- a LAUNCH
            // reached from inside any event-trigger handler invocation
            // runs against a *borrowed* chunk (still needed by that
            // handler's own *_owner global), which must not be freed
            // here just because this particular run is done with it.
            if (run->owns_chunk) {
                parse_result_destroy(run->result);
                free(run->chunk);
            }
            free(run); // not free_suspended_run -- run->vm already freed above, not double-freed
            // A second redraw, after the whole concurrent run finishes
            // -- the one at this function's own top already fired
            // before scheduler_run even started, reflecting only
            // whatever the script drew before LAUNCH suspended it.
            gtk_widget_queue_draw(app->drawing_area);
            break;
        }
        default:
            // VM_RUN_SUSPENDED_AWAIT/YIELD reached with no LAUNCH ever
            // having run first -- AWAIT/YIELD only have well-defined
            // meaning inside agent.c's own scheduler (see the LAUNCH
            // case above); used bare at an ordinary top level, there's
            // no scheduler and no sibling agents for either to mean
            // anything against. An explicit, reported error, not a
            // crash or a silent no-op.
            append_output(app, "AWAIT/YIELD outside a concurrent-agent run (started by LAUNCH) are not supported\n");
            free_suspended_run(run);
            break;
    }
    // Every point control returns here is a point CONTINUE/CO might
    // just have run (as an ordinary submitted command, or as part of a
    // just-resumed paused run's own further execution) -- so always
    // check whether the innermost paused run is now eligible, cascading
    // through handle_vm_result's own recursive call if resuming it
    // completes or itself re-pauses/re-suspends.
    maybe_resume_paused_runs(app);
}

// Fires once, exactly as long after WAIT suspended as it asked for --
// a real GLib timer source, not eval_logo's own g_usleep spin.
static gboolean on_wait_timeout(gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;
    SuspendedRun *run = g_suspended_run;
    g_suspended_run = NULL;
    VmRunResult result = vm_resume(run->vm, app, &run->result->pool, run->chunk);
    handle_vm_result(app, run, result);
    return G_SOURCE_REMOVE; // one-shot; a further WAIT gets its own new timer via handle_vm_result
}

// Fires once per frame, exactly as long after the previous frame as
// ANIMATESPRITE's own delay asked for. vm_resume_animatesprite may
// return VM_RUN_SUSPENDED_ANIMATESPRITE again (more frames left), which
// handle_vm_result's own case above re-arms into a fresh timer here --
// or it may finally fall through to a real vm_run continuation, whose
// result (VM_RUN_HALTED, another suspend, ...) handle_vm_result also
// already knows how to handle.
static gboolean on_animatesprite_timeout(gpointer user_data) {
    LogoApp *app = (LogoApp *)user_data;
    SuspendedRun *run = g_suspended_run;
    g_suspended_run = NULL;
    VmRunResult result = vm_resume_animatesprite(run->vm, app, &run->result->pool, run->chunk);
    handle_vm_result(app, run, result);
    return G_SOURCE_REMOVE;
}

// Resumes the innermost (highest-level, top-of-stack) paused run once
// app->pause_depth has dropped low enough for it -- mirrors
// interpreter.c's own do_pause loop condition exactly (it keeps waiting
// while pause_depth >= my_level, i.e. it's eligible again once
// pause_depth < my_level). CONTINUE/CO only ever decrement
// app->pause_depth by one, so at most this single innermost entry ever
// becomes eligible per call; resuming it can itself complete, re-pause
// (pushing a new entry), or hit another suspend point, all handled by
// handle_vm_result's own recursive call back into this same function,
// which is what lets a chain of nested pauses unwind one CONTINUE at a
// time without any separate loop here.
static void maybe_resume_paused_runs(LogoApp *app) {
    if (g_paused_run_count > 0 && g_paused_runs[g_paused_run_count - 1].level > app->pause_depth) {
        PausedRun top = g_paused_runs[--g_paused_run_count];
        VmRunResult result = vm_resume(top.run->vm, app, &top.run->result->pool, top.run->chunk);
        handle_vm_result(app, top.run, result);
    }
}

// Lexes/parses/compiles `source` and runs it through the VM -- the
// REPL/LOAD entry point eval_logo(app, text) used to be. A script that
// never hits a suspend point just runs to completion here, same as
// before; one that does returns immediately (control genuinely goes
// back to GTK's own main loop, not a busy-wait) with the paused run
// stashed in g_suspended_run (WAIT/WAITKEY/INPUT) or g_paused_runs
// (PAUSE).
static void run_logo_script(LogoApp *app, const char *source) {
    // Only one non-PAUSE script "thread" at a time -- g_suspended_run
    // is a single slot, not a queue/stack. WAITKEY/INPUT already can't
    // reach here while suspended (on_entry_key_pressed's own
    // waiting_for_key/waiting_for_input branches intercept Enter first
    // and never fall through to this call site), but WAIT suspends with
    // neither flag set, so without this check a second concurrent
    // run_logo_script call here would silently overwrite g_suspended_run
    // out from under the first one -- its eventual timer callback would
    // then resume the wrong script (or dereference a freed one), a real
    // bug, not a hypothetical one. Deliberately does NOT check
    // g_paused_run_count: PAUSE's entire point is that ordinary
    // submissions keep working while one or more are outstanding (see
    // the g_paused_runs comment above), so a non-empty pause stack must
    // never block a new submission the way a non-empty g_suspended_run
    // does.
    if (g_suspended_run != NULL) {
        append_output(app, "A script is still running -- please wait for it to finish\n");
        return;
    }
    LogoToken tokens[UI_SCRIPT_MAX_TOKENS];
    int n = logo_lex(source, tokens, UI_SCRIPT_MAX_TOKENS);
    if (n < 0) {
        append_output(app, "Error: script is too large to parse\n");
        return;
    }
    ParseResult *result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, result);
    if (result->error_count > 0) {
        for (int i = 0; i < result->error_count; i++) {
            char line[320];
            snprintf(line, sizeof(line), "%d:%d: %s\n", result->errors[i].line, result->errors[i].col, result->errors[i].message);
            append_output(app, line);
        }
        parse_result_destroy(result);
        return;
    }

    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    int start_pc = compile_program(&result->pool, result->program, chunk);
    Vm *vm = calloc(1, sizeof(Vm));
    VmRunResult status = vm_run(vm, app, &result->pool, chunk, start_pc);

    // A LAUNCH (Phase 6, see docs/CONCURRENT_AGENTS_DESIGN.md) is just
    // another SUSPENDED result here, same as WAIT/PAUSE/etc -- handled
    // uniformly by handle_vm_result's own VM_RUN_SUSPENDED_LAUNCH case,
    // whether this is the script's own very first vm_run call (this
    // one) or a later one reached after an earlier WAIT/WAITKEY/INPUT/
    // PAUSE/ANIMATESPRITE already suspended and resumed.
    SuspendedRun *run = calloc(1, sizeof(SuspendedRun));
    run->result = result;
    run->chunk = chunk;
    run->vm = vm;
    run->owns_chunk = TRUE;
    handle_vm_result(app, run, status);
}

// Fires `proc_name` (already validated at ONKEY/ONCLICK registration
// time to exist in `owner`'s own chunk, with exactly `argc` params) as
// a brand-new top-level run, sharing `owner`'s retained result/chunk
// (never owned by the new SuspendedRun -- see its own owns_chunk field)
// rather than compiling anything fresh. Silently drops the fire if the
// interpreter isn't idle (g_suspended_run != NULL): matches the
// existing "only one script thread at a time" rule every ordinary
// submission already follows (run_logo_script's own g_suspended_run
// check above), rather than queuing events or letting invocations pile
// up -- an event that fires while a previous handler invocation (or
// the main script) is still running/suspended is simply missed, not
// queued.
static void fire_handler(LogoApp *app, RetainedChunk *owner, const char *proc_name, EvalValue *args, int argc) {
    if (g_suspended_run != NULL) return;
    const ProcAddr *def = bytecode_find_proc_entry(owner->chunk, proc_name);
    if (def == NULL) return; // shouldn't happen -- validated at registration time; erased since then, silently skipped

    Scope handler_scope;
    int scratch_depth = 0;
    ScopeStack ss = {&handler_scope, &scratch_depth, 1};
    eval_push_scope_for_call(app, ss, def->name, def->param_count, def->param_names, args, argc); // always succeeds -- argc already matches param_count, checked at registration

    Vm *vm = calloc(1, sizeof(Vm));
    vm->scopes[0] = handler_scope;
    vm->scope_depth = 1;
    VmRunResult status = vm_run(vm, app, &owner->result->pool, owner->chunk, def->start_pc);

    SuspendedRun *run = calloc(1, sizeof(SuspendedRun));
    run->result = owner->result;
    run->chunk = owner->chunk;
    run->vm = vm;
    run->owns_chunk = FALSE;
    handle_vm_result(app, run, status);
}

static void fire_onkey(LogoApp *app, const char *key_name) {
    if (app->onkey_handler[0] == '\0') return;
    EvalValue args[1] = { word_val(key_name) };
    fire_handler(app, &g_onkey_owner, app->onkey_handler, args, 1);
}

static void fire_onclick(LogoApp *app, double x, double y, int button) {
    if (app->onclick_handler[0] == '\0') return;
    EvalValue args[3] = { num_val(x), num_val(y), num_val(button) };
    fire_handler(app, &g_onclick_owner, app->onclick_handler, args, 3);
}

static void fire_onmousemove(LogoApp *app, double x, double y) {
    if (app->onmousemove_handler[0] == '\0') return;
    EvalValue args[2] = { num_val(x), num_val(y) };
    fire_handler(app, &g_onmousemove_owner, app->onmousemove_handler, args, 2);
}

static void fire_onkeyup(LogoApp *app, const char *key_name) {
    if (app->onkeyup_handler[0] == '\0') return;
    EvalValue args[1] = { word_val(key_name) };
    fire_handler(app, &g_onkeyup_owner, app->onkeyup_handler, args, 1);
}

static void fire_onrelease(LogoApp *app, double x, double y, int button) {
    if (app->onrelease_handler[0] == '\0') return;
    EvalValue args[3] = { num_val(x), num_val(y), num_val(button) };
    fire_handler(app, &g_onrelease_owner, app->onrelease_handler, args, 3);
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

    // WAITKEY suspended the VM waiting for exactly this: the *next*
    // keypress, whatever it is -- captured here instead of its normal
    // handling (Return submitting, Up/Down navigating history, an
    // ordinary key being typed), consumed so none of that also
    // happens. (eval_logo's own busy-wait still uses key_ready/
    // pending_key -- see interpreter.c -- but bin/logo no longer calls
    // eval_logo, so this branch only ever has a VM run to resume.)
    if (app->waiting_for_key) {
        app->waiting_for_key = FALSE;
        const char *name = gdk_keyval_name(keyval);
        SuspendedRun *run = g_suspended_run;
        g_suspended_run = NULL;
        VmRunResult result = vm_resume_with_key(run->vm, app, &run->result->pool, run->chunk, name != NULL ? name : "");
        handle_vm_result(app, run, result);
        return TRUE;
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->entry));

    // INPUT suspended the VM waiting for a submitted line -- unlike
    // WAITKEY, ordinary keys must still type normally here (the user is
    // composing the line to submit), so only Return/KP_Enter is
    // special-cased: it captures the whole entry as raw text instead of
    // running it as a command, and (unlike ordinary command submission)
    // does so unconditionally, ignoring Shift and is_input_complete --
    // INPUT reads one line of data, not Logo source that might span
    // several. (eval_logo's own busy-wait still uses input_ready/
    // pending_input -- see interpreter.c -- but bin/logo no longer
    // calls eval_logo, so this branch only ever has a VM run to
    // resume, same as the waiting_for_key branch above.)
    if (app->waiting_for_input) {
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            GtkTextIter start, end;
            gtk_text_buffer_get_bounds(buffer, &start, &end);
            char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
            gtk_text_buffer_set_text(buffer, "", -1);
            app->waiting_for_input = FALSE;
            SuspendedRun *run = g_suspended_run;
            g_suspended_run = NULL;
            VmRunResult result = vm_resume_with_input(run->vm, app, &run->result->pool, run->chunk, text);
            g_free(text);
            handle_vm_result(app, run, result);
            return TRUE;
        }
        return FALSE; // every other key behaves normally: typing, backspace, cursor movement
    }

    // ONKEY's own handler (if one is registered) fires here, on every
    // ordinary keypress that reaches this point -- same key-name
    // convention as WAITKEY's own output above, but passive: unlike
    // WAITKEY/waiting_for_input, this never consumes the keystroke or
    // returns early, so typing/history/submission below all still
    // happen normally alongside it.
    const char *pressed_key_name = gdk_keyval_name(keyval);
    if (pressed_key_name != NULL) fire_onkey(app, pressed_key_name);

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

        run_logo_script(app, text);
    }

    gtk_text_buffer_set_text(buffer, "", -1);
    g_free(text);
    return TRUE; // consume the keypress, don't insert a newline
}

// ONKEYUP's own handler (if one is registered) -- unlike on_entry_key_
// pressed above, this has no WAITKEY/waiting_for_input/history/submit
// logic to interact with at all: nothing in this app currently cares
// about key *release*, so this is purely ONKEYUP's own passive fire,
// unconditional on every release.
static void on_entry_key_released(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                   GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;
    (void)state;
    LogoApp *app = (LogoApp *)user_data;
    const char *key_name = gdk_keyval_name(keyval);
    if (key_name != NULL) fire_onkeyup(app, key_name);
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

        run_logo_script(app, contents);
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

// The *.lgb filter shared by the Save Bytecode/Load Bytecode dialogs --
// same shape as logo_file_filters above, just bytecode.h/bytecode.c's
// own text format instead of Logo source (see docs/BYTECODE_REFERENCE.md
// and docs/ROADMAP.md's "Bytecode save/load/assembler" section).
static GListModel *bytecode_file_filters(void) {
    GtkFileFilter *bytecode_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(bytecode_filter, "Logo bytecode");
    gtk_file_filter_add_pattern(bytecode_filter, "*.lgb");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, bytecode_filter);
    g_object_unref(bytecode_filter);
    return G_LIST_MODEL(filters);
}

// Runs previously-assembled bytecode text as a genuinely top-level
// script -- the GUI counterpart of the Logo-level LOADBYTECODE builtin
// (vm.c's own exec_loadbytecode), but wired through this file's own
// suspend/resume machinery (handle_vm_result) instead of a nested
// recursive vm_run, since a file picked from the File menu is the
// WHOLE thing being run here, not one instruction inside an
// already-running script -- same shape as run_logo_script below, just
// bytecode_assemble instead of logo_lex+logo_parse+compile_program.
// `result` is a genuinely empty ParseResult (node_count=0, calloc'd) --
// SuspendedRun's own shape needs one, but nothing in this run ever
// reads it: an assembled chunk is already fully self-contained (Stage
// A, docs/BYTECODE_VM_DESIGN.md). Starts at the chunk's own recovered
// start_pc (see BytecodeChunk.start_pc's own comment -- never 0
// assumed).
static void run_bytecode_script(LogoApp *app, const char *text) {
    if (g_suspended_run != NULL) {
        append_output(app, "A script is still running -- please wait for it to finish\n");
        return;
    }

    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    char err[256];
    if (!bytecode_assemble(text, chunk, err, sizeof(err))) {
        append_output(app, "LOADBYTECODE: ");
        append_output(app, err);
        append_output(app, "\n");
        free(chunk);
        return;
    }

    ParseResult *result = calloc(1, sizeof(ParseResult));
    Vm *vm = calloc(1, sizeof(Vm));
    VmRunResult status = vm_run(vm, app, &result->pool, chunk, chunk->start_pc);

    SuspendedRun *run = calloc(1, sizeof(SuspendedRun));
    run->result = result;
    run->chunk = chunk;
    run->vm = vm;
    run->owns_chunk = TRUE;
    handle_vm_result(app, run, status);
}

// Finishes the async GtkFileDialog started by action_load_bytecode: reads
// the chosen file and runs it as previously-assembled bytecode, same as
// on_file_open_response does for ordinary Logo source.
static void on_bytecode_open_response(GObject *source, GAsyncResult *result, gpointer user_data) {
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

        run_bytecode_script(app, contents);
        g_free(contents);
    } else {
        append_output(app, "Could not read file\n");
        if (read_error != NULL) g_error_free(read_error);
    }

    g_object_unref(file);
}

// File > Load Bytecode… — shows a native file picker, then hands off to
// on_bytecode_open_response once the user picks a file.
static void action_load_bytecode(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Load Bytecode");
    GListModel *filters = bytecode_file_filters();
    gtk_file_dialog_set_filters(dialog, filters);
    g_object_unref(filters);

    gtk_file_dialog_open(dialog, GTK_WINDOW(app->window), NULL, on_bytecode_open_response, app);
    g_object_unref(dialog);
}

// Bundles the already-disassembled text (see action_save_bytecode below)
// with `app` for on_save_bytecode_response's own single user_data slot.
typedef struct {
    LogoApp *app;
    char *bytecode_text; // owned; malloc'd by open_memstream, freed with plain free(), not g_free()
} SaveBytecodeContext;

// Finishes the async GtkFileDialog started by action_save_bytecode:
// writes the text action_save_bytecode already compiled+disassembled
// (captured before the dialog even opened, since the entry box's own
// text can change while the async dialog is up) to the chosen file.
static void on_save_bytecode_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    SaveBytecodeContext *ctx = (SaveBytecodeContext *)user_data;
    LogoApp *app = ctx->app;

    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, result, &error);
    if (file == NULL) {
        if (error != NULL) g_error_free(error); // includes user cancellation
        free(ctx->bytecode_text);
        free(ctx);
        return;
    }

    GError *write_error = NULL;
    if (g_file_replace_contents(file, ctx->bytecode_text, strlen(ctx->bytecode_text), NULL, FALSE,
                                 G_FILE_CREATE_NONE, NULL, NULL, &write_error)) {
        char *path = g_file_get_path(file);
        append_output(app, "Saved ");
        append_output(app, path != NULL ? path : "file");
        append_output(app, "\n");
        g_free(path);
    } else {
        append_output(app, "Could not save bytecode file\n");
        if (write_error != NULL) g_error_free(write_error);
    }

    free(ctx->bytecode_text);
    free(ctx);
    g_object_unref(file);
}

// File > Save Bytecode… — compiles the entry box's own current text
// (the same text Enter would run) via logo_lex/logo_parse/
// compile_program, WITHOUT running it (no turtle motion, no PRINT
// output, no side effects at all -- unlike the Logo-level SAVEBYTECODE
// builtin, which only ever runs mid-script and saves whatever chunk is
// already executing; there's no such "currently executing chunk" to
// reach for from a menu click), disassembles the result, and shows a
// native save dialog for where to write it. Parse errors are reported
// the same way run_logo_script already reports them, and no dialog is
// shown at all in that case.
static void action_save_bytecode(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    LogoApp *app = (LogoApp *)user_data;

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->entry));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *source = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    LogoToken tokens[UI_SCRIPT_MAX_TOKENS];
    int n = logo_lex(source, tokens, UI_SCRIPT_MAX_TOKENS);
    if (n < 0) {
        append_output(app, "Error: script is too large to parse\n");
        g_free(source);
        return;
    }
    ParseResult *parse_result = calloc(1, sizeof(ParseResult));
    logo_parse(tokens, n, parse_result);
    g_free(source);
    if (parse_result->error_count > 0) {
        for (int i = 0; i < parse_result->error_count; i++) {
            char line[320];
            snprintf(line, sizeof(line), "%d:%d: %s\n", parse_result->errors[i].line, parse_result->errors[i].col, parse_result->errors[i].message);
            append_output(app, line);
        }
        parse_result_destroy(parse_result);
        return;
    }

    BytecodeChunk *chunk = calloc(1, sizeof(BytecodeChunk));
    compile_program(&parse_result->pool, parse_result->program, chunk);

    char *text = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&text, &size);
    bytecode_disassemble(chunk, f);
    fclose(f);

    free(chunk);
    parse_result_destroy(parse_result);

    SaveBytecodeContext *ctx = malloc(sizeof(SaveBytecodeContext));
    ctx->app = app;
    ctx->bytecode_text = text;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Save Bytecode");
    gtk_file_dialog_set_initial_name(dialog, "untitled.lgb");
    GListModel *filters = bytecode_file_filters();
    gtk_file_dialog_set_filters(dialog, filters);
    g_object_unref(filters);

    gtk_file_dialog_save(dialog, GTK_WINDOW(app->window), NULL, on_save_bytecode_response, ctx);
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

// main.c's own --speed/--speed=N parsing stashes its result here (via
// set_startup_turtle_speed) before g_application_run ever fires
// "activate"/"open" -- a single file-scope value is enough for the same
// reason g_suspended_run is: this app only ever builds one window/one
// LogoApp per process.
static double g_startup_turtle_speed = 0.0;

void set_startup_turtle_speed(double seconds) {
    g_startup_turtle_speed = seconds;
}

// Build the main window: the turtle canvas / REPL pane split, the View
// menu and its text-size actions/accelerators, then present it.
// Builds the window/widgets/actions shared by a normal launch
// (logo_activate) and a launch with a script path on the command line
// (logo_open) -- the two differ only in what happens after the window
// is up: logo_open goes on to load and run the given file, same as
// action_open_file's own on_file_open_response.
static LogoApp *build_main_window(GtkApplication *app) {
    LogoApp *logo = g_new0(LogoApp, 1);
    logo->canvas_width = DEFAULT_CANVAS_WIDTH;
    logo->canvas_height = DEFAULT_CANVAS_HEIGHT;
    init_turtle(logo, &logo->turtles[0]);
    logo->turtle_count = 1;
    logo->current_turtle = 0;
    logo->turtle_speed_delay = g_startup_turtle_speed;
    logo->bg_r = 1.0;
    logo->bg_g = 1.0;
    logo->bg_b = 1.0;
    logo->output_sink = history_pane_output_sink;
    logo->request_redraw = queue_canvas_redraw;
    logo->clear_history = clear_history_pane;
    logo->load_background_image = load_canvas_background_image;
    logo->save_canvas_image = export_canvas_to_png;
    logo->load_sprite_image = load_named_sprite_image;
    logo->resize_canvas = resize_canvas_widget;
    logo->joystick_connected = logo_joystick_connected;
    logo->joystick_axis = logo_joystick_axis;
    logo->joystick_button = logo_joystick_button;
    logo->play_tone = logo_play_tone;
    logo->play_sound_file = logo_play_sound_file;
    logo->stop_sound = logo_stop_sound;

    // Joystick and audio -- this app doesn't use SDL for anything else
    // (no window, no event loop of its own; GTK's own main loop keeps
    // running exactly as before). A failure here just means JOYSTICK?/
    // TONE/etc. degrade to reporting nothing connected / silently doing
    // nothing rather than crashing (ensure_joystick/ensure_audio_device
    // both check for this), same as "no controller plugged in"/"no
    // sound played" -- not worth a separate report.
    SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_AUDIO);

    GtkWidget *window = gtk_application_window_new(app);
    logo->window = window;
    gtk_window_set_title(GTK_WINDOW(window), "Logo Turtle Engine with Procedures");
    gtk_window_set_default_size(GTK_WINDOW(window), 1010, (int)logo->canvas_height);

    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    logo->paned = paned;
    gtk_window_set_child(GTK_WINDOW(window), paned);

    logo->drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(logo->drawing_area, (int)logo->canvas_width, (int)logo->canvas_height);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(logo->drawing_area), draw_cb, logo, NULL);
    gtk_paned_set_start_child(GTK_PANED(paned), logo->drawing_area);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

    // MOUSEPOS/MOUSEX/MOUSEY/BUTTON?: a passive mouse state query, kept
    // fresh by these two controllers rather than any pause/resume
    // machinery.
    GtkEventController *motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(motion_controller, "motion", G_CALLBACK(on_canvas_motion), logo);
    gtk_widget_add_controller(logo->drawing_area, motion_controller);

    GtkGesture *click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), 0); // any button
    g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_canvas_button_pressed), logo);
    g_signal_connect(click_gesture, "released", G_CALLBACK(on_canvas_button_released), logo);
    gtk_widget_add_controller(logo->drawing_area, GTK_EVENT_CONTROLLER(click_gesture));

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
    g_signal_connect(entry_key_controller, "key-released", G_CALLBACK(on_entry_key_released), logo);
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
    gtk_paned_set_position(GTK_PANED(paned), (int)logo->canvas_width);

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
        {.name = "load-bytecode", .activate = action_load_bytecode},
        {.name = "save-bytecode", .activate = action_save_bytecode},
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
    const char *load_bytecode_accels[] = {"<Meta><Shift>o", NULL};
    const char *save_bytecode_accels[] = {"<Meta><Shift>s", NULL};
    const char *export_png_accels[] = {"<Meta>e", NULL};
    const char *increase_accels[] = {"<Meta>plus", "<Meta>equal", NULL};
    const char *decrease_accels[] = {"<Meta>minus", NULL};
    const char *reset_accels[] = {"<Meta>0", NULL};
    const char *quit_accels[] = {"<Meta>q", NULL};
    gtk_application_set_accels_for_action(app, "app.open-file", open_accels);
    gtk_application_set_accels_for_action(app, "app.save-file", save_accels);
    gtk_application_set_accels_for_action(app, "app.load-bytecode", load_bytecode_accels);
    gtk_application_set_accels_for_action(app, "app.save-bytecode", save_bytecode_accels);
    gtk_application_set_accels_for_action(app, "app.export-png", export_png_accels);
    gtk_application_set_accels_for_action(app, "app.increase-text-size", increase_accels);
    gtk_application_set_accels_for_action(app, "app.decrease-text-size", decrease_accels);
    gtk_application_set_accels_for_action(app, "app.reset-text-size", reset_accels);
    gtk_application_set_accels_for_action(app, "app.quit", quit_accels);

    GMenu *menu_bar = g_menu_new();

    GMenu *file_menu = g_menu_new();
    g_menu_append(file_menu, "Open\xe2\x80\xa6", "app.open-file");
    g_menu_append(file_menu, "Save\xe2\x80\xa6", "app.save-file");
    g_menu_append(file_menu, "Load Bytecode\xe2\x80\xa6", "app.load-bytecode");
    g_menu_append(file_menu, "Save Bytecode\xe2\x80\xa6", "app.save-bytecode");
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
    // Focus the entry box immediately -- otherwise a fresh window opens
    // with no widget focused at all, and nothing typed (or WAITKEY
    // waiting on a keypress there) does anything until the user
    // happens to click into it themselves first.
    gtk_widget_grab_focus(logo->entry);

    return logo;
}

void logo_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    build_main_window(app);
}

// GApplication's own "open" signal -- fires instead of "activate" when
// the app is launched with a file argument (`bin/logo script.logo`),
// since main.c registers G_APPLICATION_HANDLES_OPEN. Builds the same
// window logo_activate would, then loads and runs the first file given
// (extras, if any, are ignored -- this app only ever has one script
// entry point), identical in spirit to action_open_file's own
// on_file_open_response.
void logo_open(GApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer user_data) {
    (void)hint;
    (void)user_data;
    LogoApp *logo = build_main_window(GTK_APPLICATION(app));
    if (n_files < 1) return;

    char *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (g_file_load_contents(files[0], NULL, &contents, &length, NULL, &error)) {
        char *path = g_file_get_path(files[0]);
        append_output(logo, "Loaded ");
        append_output(logo, path != NULL ? path : "file");
        append_output(logo, "\n");
        g_free(path);

        run_logo_script(logo, contents);
        g_free(contents);
    } else {
        append_output(logo, "Could not read file\n");
        if (error != NULL) g_error_free(error);
    }
}
