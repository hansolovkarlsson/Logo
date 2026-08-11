// main.c
//
// Entry point: creates the GtkApplication and hands control to
// logo_activate (ui.c) once GTK signals "activate".

#include "ui.h"
#include "interpreter.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>

// Ctrl+C in the terminal this app was launched from would otherwise
// just kill the whole process outright (the default SIGINT
// disposition) -- request_interrupt instead asks whatever script is
// currently running to stop at its next opportunity, however deeply
// nested in loops/procedure calls, and leaves the app itself running
// (GTK's own main loop, and this handler, are unaffected).
static void handle_sigint(int sig) {
    (void)sig;
    request_interrupt();
}

// Pulls `--speed N` / `--speed=N` (SETSPEED's own units, seconds) out
// of argv before g_application_run ever sees it -- GApplication's
// default option handling (with no GOptionEntry table registered)
// rejects any argument it doesn't recognize as a file to open, so an
// unhandled --speed would abort the launch rather than being silently
// ignored. Rewrites argv/argc in place (the filtered form is always
// shorter or equal, so this never overruns the original array) and
// forwards the parsed value to ui.c via set_startup_turtle_speed --
// simple argv surgery rather than a full GOptionContext, matching this
// file's existing "a plain global flag, not a framework" SIGINT
// handling above.
static void consume_speed_flag(int *argc, char **argv) {
    int out = 1;
    for (int i = 1; i < *argc; i++) {
        if (strcmp(argv[i], "--speed") == 0 && i + 1 < *argc) {
            set_startup_turtle_speed(atof(argv[i + 1]));
            i++; // also consume the value
            continue;
        }
        if (strncmp(argv[i], "--speed=", 8) == 0) {
            set_startup_turtle_speed(atof(argv[i] + 8));
            continue;
        }
        argv[out++] = argv[i];
    }
    argv[out] = NULL;
    *argc = out;
}

int main(int argc, char **argv) {
    signal(SIGINT, handle_sigint);
    consume_speed_flag(&argc, argv);
    GtkApplication *app = gtk_application_new("org.logo.procedures", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(logo_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(logo_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
