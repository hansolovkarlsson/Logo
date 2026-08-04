#ifndef LOGO_TYPES_H
#define LOGO_TYPES_H

// logo_types.h
//
// Shared data structures and limits for the Logo interpreter: the turtle,
// drawn line segments, user-defined procedures, variables, and the overall
// LogoApp struct (interpreter state plus the GTK widgets that display it),
// passed around by both interpreter.c and ui.c.

#include <gtk/gtk.h>

#define MAX_LINES 10000
#define MAX_PROCEDURES 50
#define MAX_VARIABLES 100
#define MAX_WHILE_ITERATIONS 1000000
#define MAX_PARAMS 8

// One drawn segment of the turtle's trail.
typedef struct {
    double x1, y1, x2, y2;
} LineSegment;

// The turtle's position, heading, and pen state.
typedef struct {
    double x, y;
    double angle;
    int pen_down;
} Turtle;

// A user-defined procedure (TO ... END).
typedef struct {
    char name[32];
    char param_names[MAX_PARAMS][32]; // e.g. :SIZE :ANGLE ...
    int param_count;
    char body[2048];
} Procedure;

// A global variable (MAKE "name value / :name).
typedef struct {
    char name[32];
    double value;
} Variable;

// All interpreter state plus the GTK widgets that display it.
typedef struct {
    Turtle turtle;
    LineSegment lines[MAX_LINES];
    int line_count;

    Procedure procedures[MAX_PROCEDURES];
    int proc_count;

    Variable variables[MAX_VARIABLES];
    int var_count;

    GtkWidget *drawing_area;
    GtkWidget *text_view;
    GtkWidget *entry;

    GtkCssProvider *css_provider;
    int font_size;
} LogoApp;

#endif // LOGO_TYPES_H
