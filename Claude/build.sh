gcc $(pkg-config --cflags gtk4) -o myapp myapp.c $(pkg-config --libs gtk4)
