#!/bin/bash
# Assembles a self-contained Windows folder around bin/logomotive.exe --
# the .exe plus every DLL it transitively needs and the runtime data GTK
# looks up by path -- so it runs on a machine with no MSYS2 installed.
#
# Run from an MSYS2 shell for your CPU (CLANGARM64 or UCRT64), after
# `make`. Output goes to dist/logomotive-windows-<env>/.
#
# Why this exists at all: a GTK4 program on Windows is not one binary.
# It's ~40 DLLs, plus three things GTK finds through the filesystem
# rather than through the linker -- compiled GSettings schemas, the
# gdk-pixbuf loader modules, and an icon theme. Miss a DLL and the
# process dies at load with no message a user can act on; miss the
# runtime data and it starts and then misbehaves in ways that look like
# application bugs. There is no windeployqt equivalent for GTK, hence
# doing it by hand here.
set -e

if [ -z "$MSYSTEM" ] || [ "$MSYSTEM" = "MSYS" ]; then
    echo "bundle_windows.sh: run this from a native MSYS2 environment shell" >&2
    echo "(CLANGARM64 on ARM64, UCRT64 on x86-64), not the plain MSYS shell." >&2
    exit 1
fi

cd "$(dirname "$0")/.."
EXE=bin/logomotive.exe
if [ ! -f "$EXE" ]; then
    echo "bundle_windows.sh: $EXE not found -- run 'make' first." >&2
    exit 1
fi

PREFIX="${MINGW_PREFIX:-/$(echo "$MSYSTEM" | tr '[:upper:]' '[:lower:]')}"
OUT="dist/logomotive-windows-$(echo "$MSYSTEM" | tr '[:upper:]' '[:lower:]')"

# Clear the contents rather than the directory itself. On Windows a
# directory can't be removed while any process has it as a working
# directory -- and testing a bundle means running things from inside it,
# so a shell left sitting there makes a plain `rm -rf "$OUT"` fail with
# "Device or resource busy" and, under set -e, abort the whole script.
# Emptying it in place sidesteps that. A DLL still loaded by a running
# copy of the app is a genuine conflict, though, so that's reported
# properly instead of being worked around.
mkdir -p "$OUT"
if ! find "$OUT" -mindepth 1 -delete 2>/dev/null; then
    echo "bundle_windows.sh: couldn't clear $OUT -- is a copy of" >&2
    echo "logomotive.exe from a previous bundle still running?" >&2
    exit 1
fi
cp "$EXE" "$OUT/"

# Transitive DLL closure, walked with objdump rather than ldd: ldd
# resolves against the CURRENT PATH, so it happily reports the MSYS2
# copies as "found" and tells us nothing about what a clean machine
# would be missing. objdump reads the import table out of the file
# itself, which is the actual question being asked. Anything not present
# in $PREFIX/bin is a system DLL shipped by Windows (kernel32, msvcrt,
# ...) and is deliberately skipped.
echo "Resolving DLL closure..."
declare -A seen
queue=("$EXE")
count=0
while [ ${#queue[@]} -gt 0 ]; do
    cur="${queue[0]}"
    queue=("${queue[@]:1}")
    while read -r dll; do
        [ -n "$dll" ] || continue
        [ -n "${seen[$dll]:-}" ] && continue
        src="$PREFIX/bin/$dll"
        if [ -f "$src" ]; then
            seen[$dll]=1
            cp "$src" "$OUT/"
            queue+=("$src")
            count=$((count+1))
        fi
    done < <(objdump -p "$cur" 2>/dev/null | sed -n 's/^\s*DLL Name:\s*//p')
done
echo "  bundled $count DLL(s)"

# The three things GTK resolves through the filesystem, not the linker.
#
# 1. GSettings schemas. GTK's own settings live here; without the
#    compiled schema file GTK aborts at startup ("Settings schema
#    'org.gtk.Settings.FileChooser' is not installed") the first time
#    something touches it -- which for this app is the File > Open
#    dialog, not launch, so it looks like a crash in the file picker.
echo "Copying GSettings schemas..."
mkdir -p "$OUT/share/glib-2.0/schemas"
cp "$PREFIX/share/glib-2.0/schemas/gschemas.compiled" "$OUT/share/glib-2.0/schemas/"

# 2. gdk-pixbuf loaders -- the modules that decode PNG/JPEG. LOADPIC,
#    LOADSPRITE and LOADSPRITESHEET all go through gdk-pixbuf, so
#    without these every image load fails while the rest of the app
#    works normally.
echo "Copying gdk-pixbuf loaders..."
#    loaders.cache is copied as-is rather than regenerated: MSYS2 builds
#    it with paths relative to the bundle root ("lib\gdk-pixbuf-2.0\..."),
#    so it relocates correctly. Verified, not assumed -- an absolute
#    path baked in there pointing back at /clangarm64 is the usual way
#    this breaks, and would need gdk-pixbuf-query-loaders re-run at the
#    destination.
#
#    The .a files alongside the loaders are import libraries for linking
#    against, with no role at runtime, so they're skipped.
pixbuf_dir=$(cd "$PREFIX/lib" && echo gdk-pixbuf-2.0/*/ 2>/dev/null | head -1)
if [ -n "$pixbuf_dir" ] && [ -d "$PREFIX/lib/$pixbuf_dir" ]; then
    mkdir -p "$OUT/lib/$pixbuf_dir"
    cp -r "$PREFIX/lib/$pixbuf_dir." "$OUT/lib/$pixbuf_dir"
    find "$OUT/lib/gdk-pixbuf-2.0" -name '*.a' -delete
fi

# 3. Icon theme. GTK4 compiles its own Adwaita icons into libgtk as a
#    GResource, so the menus and dialogs render without this -- but
#    hicolor is the fallback theme every GTK app is expected to find,
#    and its index.theme is cheap to carry.
if [ -d "$PREFIX/share/icons/hicolor" ]; then
    echo "Copying hicolor icon theme index..."
    mkdir -p "$OUT/share/icons/hicolor"
    cp "$PREFIX/share/icons/hicolor/index.theme" "$OUT/share/icons/hicolor/" 2>/dev/null || true
fi

cp README.md "$OUT/" 2>/dev/null || true

# The whole examples/ directory, not just the .logo files: several
# scripts load image assets that live beside them (background_image.logo
# wants backyard.png, sprites.logo wants ant.png, spritesheet.logo wants
# walker.png), and they resolve those by relative path. Shipping the
# scripts without the images produced a bundle where those examples
# started up fine and then reported 'LOADPIC: could not load
# "backyard.png' -- which reads as a broken image decoder rather than a
# missing file, and is exactly the wrong thing to leave a first-time
# Windows user staring at.
mkdir -p "$OUT/examples"
cp -r examples/. "$OUT/examples/"

echo
echo "Bundle ready: $OUT"
du -sh "$OUT" 2>/dev/null || true
echo "Test it with MSYS2 off PATH -- see scripts/bundle_windows.sh's own comments."
