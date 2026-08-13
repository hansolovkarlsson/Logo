#!/bin/bash
# Installs LogoMotive's build dependencies: GTK4, SDL2, pkg-config.
#
# SDL2 is not optional despite this script's name -- the Makefile has
# required it since Phase 4's joystick input (see its own SDL_LIBS
# comment), so a gtk4-only install still fails at link on ui.c's SDL
# calls. It was missing here on macOS too, not just on Linux.
#
# Package-manager detection rather than one script per platform: the
# Makefile and build.sh are already platform-neutral (plain
# pkg-config/gcc, no hardcoded paths), so this is the only file that
# needs to know what OS it's on. brew is probed first so that a Mac with
# some other package manager installed still takes the Homebrew path.
set -e

if command -v brew >/dev/null 2>&1; then
    brew install gtk4 sdl2 pkg-config
elif command -v dnf >/dev/null 2>&1; then
    # Verified on Fedora 42 (aarch64), 2026-08-13.
    sudo dnf install -y gtk4-devel SDL2-devel pkgconf-pkg-config
elif command -v apt-get >/dev/null 2>&1; then
    # All three names confirmed to exist on Ubuntu 24.04 LTS (aarch64),
    # 2026-08-13, and LogoMotive builds and tests clean against what
    # they provide (gtk4 4.14.5, sdl2 2.30.0, pkg-config 1.8.1). The
    # apt-get call itself is still unexercised -- they were already
    # installed on the box that verified them. Debian names assumed
    # from Ubuntu's, as before.
    sudo apt-get install -y libgtk-4-dev libsdl2-dev pkg-config
elif command -v pacman >/dev/null 2>&1; then
    # Package names per Arch; not yet built there.
    sudo pacman -S --needed gtk4 sdl2 pkgconf
else
    echo "install_gtk.sh: no supported package manager found." >&2
    echo "Tried: brew, dnf, apt-get, pacman." >&2
    echo "Install GTK4, SDL2 and pkg-config by hand, then run 'make'." >&2
    exit 1
fi
