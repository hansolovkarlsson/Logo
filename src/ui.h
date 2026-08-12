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
// (`bin/logomotive script.logo`) -- called on GApplication::open, which fires
// instead of "activate" once main.c registers G_APPLICATION_HANDLES_OPEN.
// Builds the window, then loads and runs files[0] as Logo source.
void logo_open(GApplication *app, GFile **files, gint n_files, const gchar *hint, gpointer user_data);

// Sets the turtle-motion delay (SETSPEED's own units, seconds) every
// subsequently-built window's LogoApp starts with -- main.c calls this
// once, from its own --speed/--speed=N argv parsing, before
// g_application_run ever fires "activate"/"open". A script's own
// SETSPEED call still overrides it same as always; this only affects
// the value already in place before the script's first instruction.
void set_startup_turtle_speed(double seconds);

#endif // UI_H
