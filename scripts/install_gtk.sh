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

# MSYS2 (Windows) is checked FIRST, and specifically before the pacman
# branch further down, because MSYS2 ships pacman too. Without this the
# Arch branch matched on Windows and asked for the bare gtk4/sdl2
# package names, which don't exist in MSYS2's repositories -- every
# native package there carries an environment prefix. It failed with
# "target not found" having installed nothing, which reads as "this
# project doesn't support Windows" rather than "wrong package names".
#
# $MSYSTEM is set by the MSYS2 shell itself and names the environment,
# which is what decides both the prefix and the compiler, so it's a
# more precise signal than uname here.
if [ -n "$MSYSTEM" ]; then
    # Prefix per environment, and the compiler that goes with it. The
    # CLANG* environments have no gcc of their own, so they take clang
    # plus gcc-compat -- the wrapper that provides the `gcc` the
    # Makefile's own CC asks for. CLANGARM64 is the only toolchain that
    # targets Windows-on-ARM at all, which is why ARM64 is a clang
    # environment rather than a gcc one.
    case "$MSYSTEM" in
        CLANGARM64) prefix="mingw-w64-clang-aarch64-"; cc_pkgs="clang gcc-compat" ;;
        CLANG64)    prefix="mingw-w64-clang-x86_64-";  cc_pkgs="clang gcc-compat" ;;
        UCRT64)     prefix="mingw-w64-ucrt-x86_64-";   cc_pkgs="gcc" ;;
        MINGW64)    prefix="mingw-w64-x86_64-";        cc_pkgs="gcc" ;;
        MINGW32)    prefix="mingw-w64-i686-";          cc_pkgs="gcc" ;;
        MSYS)
            # The MSYS environment builds POSIX-emulation binaries against
            # msys-2.0.dll and has no native GTK4 at all, so there is
            # nothing useful to install here -- this is a wrong-shell
            # error, not a missing-package one.
            echo "install_gtk.sh: this is the MSYS shell, which can't build a native" >&2
            echo "Windows GUI binary (no native GTK4, and it links msys-2.0.dll)." >&2
            echo "Open the environment matching your CPU instead and re-run:" >&2
            echo "    ARM64   -> CLANGARM64" >&2
            echo "    x86-64  -> UCRT64" >&2
            exit 1
            ;;
        *)
            echo "install_gtk.sh: unrecognized MSYS2 environment '$MSYSTEM'." >&2
            echo "Expected one of: CLANGARM64, UCRT64, CLANG64, MINGW64, MINGW32." >&2
            exit 1
            ;;
    esac

    # No sudo: MSYS2's pacman runs as the invoking user, and sudo isn't
    # part of a default install. `make` is deliberately the unprefixed
    # MSYS package -- the prefixed one installs as mingw32-make, and the
    # Makefile and this project's docs both say plain `make`.
    #
    # Verified end to end on CLANGARM64 (Windows 11 ARM64), 2026-08-14.
    # The other four environments use the same package set behind their
    # own prefix; every name was checked to resolve against the live
    # repos, but only CLANGARM64 has actually been built with.
    pacman -S --needed --noconfirm \
        "${prefix}gtk4" \
        "${prefix}SDL2" \
        "${prefix}pkgconf" \
        $(for p in $cc_pkgs; do printf '%s%s ' "$prefix" "$p"; done) \
        make
elif command -v brew >/dev/null 2>&1; then
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
    echo "Tried: MSYS2 (\$MSYSTEM), brew, dnf, apt-get, pacman." >&2
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
        echo "on Debian/Ubuntu, 'gcc make' on Fedora, base-devel on Arch. On" >&2
        echo "MSYS2, check you opened the environment for your CPU (CLANGARM64" >&2
        echo "on ARM64, UCRT64 on x86-64) rather than the plain MSYS shell." >&2
    fi
    exit 1
fi

echo "install_gtk.sh: dependencies ready. Run 'make' to build."
