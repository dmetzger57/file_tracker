#!/bin/bash
# Turn "File Tracker Unified.app" (from `make apps`) into a self-contained app
# and put it on a disk image: dist/FileTracker-<version>.dmg
#
# The app as built links GTK, OpenSSL, SQLite and friends from Homebrew. This copies
# every Homebrew library it needs into Contents/Frameworks, rewrites the load paths
# to @rpath, adds the GTK data files (symbolic icons, settings schemas) under
# Contents/Resources/share, and ad-hoc signs the result, so it runs on a Mac
# without Homebrew. Run via `make dmg`.
set -euo pipefail

VERSION="${VERSION:?set VERSION (make dmg does this)}"
APP_NAME="File Tracker Unified"
EXE="file_tracker_unified"
BREW="$(brew --prefix)"
DIST="dist"
STAGE="$DIST/stage"
APP="$STAGE/$APP_NAME.app"
FRAMEWORKS="$APP/Contents/Frameworks"
SHARE="$APP/Contents/Resources/share"
DMG="$DIST/FileTracker-$VERSION.dmg"

[[ -d "$APP_NAME.app" ]] || { echo "Run 'make apps' first" >&2; exit 1; }

rm -rf "$DIST"
mkdir -p "$STAGE"
cp -R "$APP_NAME.app" "$STAGE/"
mkdir -p "$FRAMEWORKS" "$SHARE"

# Homebrew libraries a Mach-O file loads (paths as recorded in the file)
brew_deps() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -E "^($BREW|@rpath|@loader_path)" || true
}

# Resolve a recorded load path to the real file on disk
resolve() {
    local dep="$1" from="$2"
    case "$dep" in
        @loader_path/*) dep="$(dirname "$from")/${dep#@loader_path/}" ;;
        @rpath/*)       dep="$BREW/lib/${dep#@rpath/}" ;;
    esac
    realpath "$dep"
}

# Copy every library the executable needs, following dependencies of dependencies
queue=("$APP/Contents/MacOS/$EXE")
declare -a seen=()
while ((${#queue[@]})); do
    file="${queue[0]}"; queue=("${queue[@]:1}")
    origin="${file}"
    [[ "$file" == "$FRAMEWORKS"/* ]] && origin="$(cat "$file.origin" 2>/dev/null || echo "$file")"
    for dep in $(brew_deps "$file"); do
        src="$(resolve "$dep" "$origin")"
        name="$(basename "$dep")"
        if [[ ! -f "$FRAMEWORKS/$name" ]]; then
            cp "$src" "$FRAMEWORKS/$name"
            chmod u+w "$FRAMEWORKS/$name"
            echo "$src" > "$FRAMEWORKS/$name.origin"
            queue+=("$FRAMEWORKS/$name")
        fi
        install_name_tool -change "$dep" "@rpath/$name" "$file" 2>/dev/null
    done
    if [[ "$file" == "$FRAMEWORKS"/* ]]; then
        install_name_tool -id "@rpath/$(basename "$file")" "$file" 2>/dev/null
    fi
done
rm -f "$FRAMEWORKS"/*.origin
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/$EXE"

# Nothing may still point into Homebrew
if otool -L "$APP/Contents/MacOS/$EXE" "$FRAMEWORKS"/* | grep -q "$BREW"; then
    echo "Error: Homebrew paths remain:" >&2
    otool -L "$APP/Contents/MacOS/$EXE" "$FRAMEWORKS"/* | grep "$BREW" >&2
    exit 1
fi

# GTK data: Adwaita symbolic icons (sidebar and widget icons), hicolor fallback,
# compiled GSettings schemas (file dialogs read them)
mkdir -p "$SHARE/icons/Adwaita" "$SHARE/icons/hicolor" "$SHARE/glib-2.0/schemas"
cp -RL "$BREW/share/icons/Adwaita/index.theme" "$BREW/share/icons/Adwaita/symbolic" "$SHARE/icons/Adwaita/"
cp -L "$BREW/share/icons/hicolor/index.theme" "$SHARE/icons/hicolor/"
cp "$BREW/share/glib-2.0/schemas/"org.gtk.gtk4.*.gschema.xml "$SHARE/glib-2.0/schemas/"
glib-compile-schemas "$SHARE/glib-2.0/schemas"

# Homebrew builds its libraries for the macOS it runs on; the app needs at least that
minos="$(for f in "$APP/Contents/MacOS/$EXE" "$FRAMEWORKS"/*.dylib; do
    otool -l "$f" | awk '/LC_BUILD_VERSION/ {b = 1} b && /minos/ {print $2; exit}'
done | sort -V | tail -1)"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $minos" "$APP/Contents/Info.plist"

# Libraries were modified, so every signature must be redone (Apple silicon refuses
# to load unsigned code). Ad-hoc: not notarized.
codesign --force --sign - "$FRAMEWORKS"/*.dylib 2>/dev/null
codesign --force --sign - "$APP" 2>/dev/null
codesign --verify --deep --strict "$APP"

# Disk image: the app plus an Applications shortcut to drag it onto
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "File Tracker $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG"
rm -rf "$STAGE"
(cd "$DIST" >/dev/null && shasum -a 256 "$(basename "$DMG")" > SHA256SUMS)

echo "File Tracker $VERSION -> $DMG ($(du -h "$DMG" | cut -f1); arm64, macOS $minos or later)"
