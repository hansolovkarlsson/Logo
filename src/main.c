// main.c
//
// Entry point: creates the GtkApplication and hands control to
// logo_activate (ui.c) once GTK signals "activate".

#include "ui.h"
#include "interpreter.h"
#include <signal.h>

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

int main(int argc, char **argv) {
    signal(SIGINT, handle_sigint);
    GtkApplication *app = gtk_application_new("org.logo.procedures", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(logo_activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(logo_open), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
