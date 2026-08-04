// main.c
//
// Entry point: creates the GtkApplication and hands control to
// logo_activate (ui.c) once GTK signals "activate".

#include "ui.h"

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.logo.procedures", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(logo_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
