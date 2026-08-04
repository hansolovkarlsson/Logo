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

#endif // UI_H
