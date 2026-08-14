#!/bin/bash
# Prints the README.txt that ships inside a release archive, for the
# platform named by $1, at the version named by $2. Written to stdout;
# the release workflow redirects it into the staging folder.
#
# A script rather than a heredoc inside .github/workflows/release.yml so
# the wording can be read, diffed and test-run without pushing a tag.
#
# Deliberately plain ASCII: this file is opened in whatever the user's
# default text viewer is, including Notepad on Windows, and the v0.1.0
# release shipped a README.txt whose em-dashes rendered as "a??" there.
set -e

PLATFORM="${1:?usage: release_readme.sh <platform-id> <version>}"
VERSION="${2:?usage: release_readme.sh <platform-id> <version>}"

case "$PLATFORM" in
    macos-arm64)    TITLE="macOS (Apple Silicon / arm64)" ;;
    windows-x86_64) TITLE="Windows (Intel/AMD 64-bit)" ;;
    windows-arm64)  TITLE="Windows (ARM64)" ;;
    *) echo "release_readme.sh: unknown platform '$PLATFORM'" >&2; exit 1 ;;
esac

echo "LogoMotive $VERSION - $TITLE"
echo "=================================================="
echo

case "$PLATFORM" in
    macos-arm64)
        # The v0.1.0 README named only GTK4 here. That was wrong and
        # would have stopped the app starting: the binary links SDL2
        # too (joystick and audio -- see the Makefile's SDL_LIBS
        # comment), so a machine with just gtk4 installed gets a dyld
        # error about libSDL2 rather than a window.
        cat <<'BODY'
REQUIREMENTS

This build is dynamically linked against GTK4 and SDL2, neither of
which is bundled in this download. Install both with Homebrew before
running it:

    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    brew install gtk4 sdl2

(Skip the first line if you already have Homebrew.)

RUNNING

Open a terminal in this folder and run:

    ./logomotive
    ./logomotive examples/concurrent_agents.logo

GATEKEEPER

This build is not code-signed or notarized, so macOS will warn that it
comes from an unidentified developer the first time you open it.
Right-click the binary in Finder and choose "Open", or allow it under
System Settings > Privacy & Security.
BODY
        ;;
    windows-*)
        # Windows ships self-contained, unlike macOS: bundle_windows.sh
        # puts the whole GTK/SDL DLL closure in the folder, so there is
        # nothing for the user to install first.
        cat <<'BODY'
REQUIREMENTS

None. Everything this needs is in this folder -- the GTK4 and SDL2
libraries are included alongside the executable, so there is nothing
to install first. Keep the folder together; logomotive.exe will not
run on its own if moved out of it.

RUNNING

Double-click logomotive.exe, or from a command prompt in this folder:

    logomotive.exe
    logomotive.exe examples\concurrent_agents.logo

SMARTSCREEN

This build is not code-signed, so Windows SmartScreen may show a
"Windows protected your PC" warning the first time you run it. Choose
"More info" and then "Run anyway".
BODY
        ;;
esac

cat <<'BODY'

A window opens with the turtle's canvas on one side and a history pane
on the other. Type Logo commands into the entry box, or run any of the
.logo files in examples/.

GETTING STARTED

- Tutorial and full command reference:
  https://hansolovkarlsson.github.io/LogoMotive/
- Source and documentation:
  https://github.com/hansolovkarlsson/LogoMotive
BODY
