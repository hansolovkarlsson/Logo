#!/bin/bash
# Installs LogoMotive's build dependencies: GTK4, SDL2, pkg-config, and
# on Linux a C toolchain (compiler + make).
#
# SDL2 is not optional despite this script's name -- the Makefile has
# required it since Phase 4's joystick input (see its own SDL_LIBS
# comment), so a gtk4-only install still fails at link on ui.c's SDL
# calls. It was missing here on macOS too, not just on Linux.
#
# The toolchain is here because leaving it out had a bad failure mode:
# the libraries would install cleanly, and 'make' would then die with
# "gcc: command not found" -- which reads as a broken project rather
# than a missing package, and at a point where the user reasonably
# believes setup already succeeded. It's a no-op on any machine that
# has built anything before.
#
# Package-manager detection rather than one script per platform: the
# Makefile and build.sh are already platform-neutral (plain
# pkg-config/gcc, no hardcoded paths), so this is the only file that
# needs to know what OS it's on. brew is probed first so that a Mac with
# some other package manager installed still takes the Homebrew path.
set -e

if command -v brew >/dev/null 2>&1; then
    # No compiler in this list, deliberately. On macOS it comes from
    # Xcode's Command Line Tools, not Homebrew -- and Homebrew's gcc
    # installs as gcc-N, leaving plain 'gcc' as Apple clang either way,
    # so adding it would install a second compiler that nothing uses
    # rather than fix a missing one. Homebrew itself requires the CLT
    # to install, so a machine with brew already has cc and make. The
    # check below covers it regardless.
    brew install gtk4 sdl2 pkg-config
elif command -v dnf >/dev/null 2>&1; then
    # Verified on Fedora 42 (aarch64), 2026-08-13.
    sudo dnf install -y gtk4-devel SDL2-devel pkgconf-pkg-config gcc make
elif command -v apt-get >/dev/null 2>&1; then
    # The library names are confirmed to exist on Ubuntu 24.04 LTS
    # (aarch64), 2026-08-13, and LogoMotive builds and tests clean
    # against what they provide (gtk4 4.14.5, sdl2 2.30.0, pkg-config
    # 1.8.1). The apt-get call itself is still unexercised -- they were
    # already installed on the box that verified them. Debian names
    # assumed from Ubuntu's, as before. build-essential pulls in gcc,
    # make and libc6-dev together.
    sudo apt-get install -y libgtk-4-dev libsdl2-dev pkg-config build-essential
elif command -v pacman >/dev/null 2>&1; then
    # Package names per Arch; not yet built there. base-devel is a
    # package group, which is why --needed matters here -- without it
    # pacman reinstalls every member.
    sudo pacman -S --needed gtk4 sdl2 pkgconf base-devel
else
    echo "install_gtk.sh: no supported package manager found." >&2
    echo "Tried: brew, dnf, apt-get, pacman." >&2
    echo "Install GTK4, SDL2, pkg-config and a C toolchain (compiler +" >&2
    echo "make) by hand, then run 'make'." >&2
    exit 1
fi

# Verify rather than assume: this is exactly the gap that made the
# toolchain worth adding, so it gets checked instead of trusted. Fails
# loudly here, where the cause is obvious, rather than later inside a
# compile.
missing=""
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
    missing="a C compiler"
fi
if ! command -v make >/dev/null 2>&1; then
    missing="${missing:+$missing and }make"
fi

if [ -n "$missing" ]; then
    echo "install_gtk.sh: libraries are installed, but still missing: $missing." >&2
    if [ "$(uname -s)" = "Darwin" ]; then
        echo "On macOS the compiler ships with Xcode's Command Line Tools:" >&2
        echo "    xcode-select --install" >&2
    else
        echo "Install your distribution's build-tools package -- build-essential" >&2
        echo "on Debian/Ubuntu, 'gcc make' on Fedora, base-devel on Arch." >&2
    fi
    exit 1
fi

echo "install_gtk.sh: dependencies ready. Run 'make' to build."
