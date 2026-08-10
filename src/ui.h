#ifndef UI_H
#define UI_H

// ui.h
//
// Public interface to the GTK front end: building and wiring up the main
// window (logo_activate), which main.c calls in response to the
// GtkApplication's "activate" signal.

#include "logo_types.h"

// Build and present the main window; called on GApplication::activate.
void logo_activate(GtkApplication *app, gpointer user_data);

// Same, but for a launch with a script path on the command line
// (`bin/logo script.logo`) -- called on GApplication::open, which fires
// instead of "activate" once main.c registers G_APPLICATION_HANDLES_OPEN.
// Builds the window, then loads and runs files[0] as Logo source.
void logo_open(GApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer user_data);

#endif // UI_H
