#include <gtk/gtk.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

// Simulated Interpreter Thread
void* interpreter_thread(void* arg) {
    printf("[Interpreter] Starting execution loop...\n");
    
    // Example: Interpreter running in background
    for (int i = 1; i <= 5; i++) {
        sleep(1);
        printf("[Interpreter] Evaluated step %d\n", i);
    }
    
    printf("[Interpreter] Execution finished.\n");
    return NULL;
}

static void activate(GtkApplication* app, gpointer user_data) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "C Interpreter + GTK4");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);

    // Spawn the interpreter thread so it doesn't block GTK's main event loop
    pthread_t thread_id;
    pthread_create(&thread_id, NULL, interpreter_thread, NULL);

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.interpreter", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

