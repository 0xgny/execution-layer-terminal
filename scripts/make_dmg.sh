#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# make_dmg.sh -- package the terminal GUI as a distributable macOS .dmg.
#
# Steps:
#   1. make gui                       (cpp/build/terminal)
#   2. PyInstaller the feedhandler    (Contents/Resources/feedhandler/)
#   3. wrap both in dist/<App>.app    (Info.plist + icon + launcher)
#   4. copy non-system dylibs (Homebrew glfw) into Contents/Frameworks and
#      rewrite the install names, so the app runs on a machine without brew
#   5. ad-hoc codesign
#   6. hdiutil -> dist/<App>-<version>.dmg with an /Applications drop target
#
# The result needs nothing installed on the target machine: no Homebrew, no
# Python, no runtime of any kind.
# Set EL_SKIP_FEED=1 to build a terminal-only bundle (much faster; the app then
# expects you to run scripts/run_stack.sh yourself).
#
# Usage:
#   scripts/make_dmg.sh                 # version 1.0.0, ad-hoc signature
#   scripts/make_dmg.sh 1.2.0
#   CODESIGN_ID="Developer ID Application: You (TEAMID)" scripts/make_dmg.sh 1.2.0
# ---------------------------------------------------------------------------
set -euo pipefail

VERSION="${1:-1.0.0}"
APP_NAME="Execution Layer Terminal"
BUNDLE_ID="dev.executionlayer.terminal"
MIN_MACOS="12.0"
CODESIGN_ID="${CODESIGN_ID:--}"   # "-" = ad-hoc

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DIST="$ROOT/dist"
APP="$DIST/$APP_NAME.app"
MACOS_DIR="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"
RESOURCES="$APP/Contents/Resources"
DMG="$DIST/${APP_NAME// /-}-$VERSION.dmg"

echo "[make_dmg] building GUI"
make -C cpp gui

echo "[make_dmg] staging $APP"
rm -rf "$APP" "$DMG"
mkdir -p "$MACOS_DIR" "$FRAMEWORKS" "$RESOURCES"
cp cpp/build/terminal "$MACOS_DIR/terminal"
chmod +x "$MACOS_DIR/terminal"

# --- icon ------------------------------------------------------------------
if [[ ! -f assets/icon.icns ]]; then
  echo "[make_dmg] generating assets/icon.icns"
  python3 scripts/make_icon.py assets/icon.icns
fi
cp assets/icon.icns "$RESOURCES/icon.icns"

# --- feedhandler ------------------------------------------------------------
# Frozen with PyInstaller in onedir mode: a real executable plus its own Python
# runtime and deps (websockets, certifi). Nothing is installed on the target
# machine. onedir over onefile because onefile unpacks to a temp dir on every
# launch, which is slower and shows up as a mystery process.
if [[ "${EL_SKIP_FEED:-0}" == "1" ]]; then
  echo "[make_dmg] EL_SKIP_FEED=1, terminal-only bundle"
else
  PY="${PYTHON:-$ROOT/.venv/bin/python}"
  if ! "$PY" -c "import PyInstaller" 2>/dev/null; then
    echo "[make_dmg] ERROR: PyInstaller not available in $PY" >&2
    echo "[make_dmg]   $PY -m pip install pyinstaller" >&2
    exit 1
  fi
  echo "[make_dmg] freezing feedhandler"
  rm -rf build/pyinstaller
  "$PY" -m PyInstaller \
    --name feedhandler \
    --distpath "$RESOURCES" \
    --workpath build/pyinstaller \
    --specpath build/pyinstaller \
    --noconfirm --clean --log-level WARN \
    --paths "$ROOT" \
    --hidden-import feedhandler.coinbase \
    --hidden-import feedhandler.binance \
    --hidden-import feedhandler.mock \
    scripts/feedhandler_entry.py
  [[ -x "$RESOURCES/feedhandler/feedhandler" ]] \
    || { echo "[make_dmg] ERROR: feedhandler not produced" >&2; exit 1; }
fi

# --- bundle non-system dylibs ----------------------------------------------
# Worklist over the dependency graph: anything not under /usr/lib or /System
# gets copied in and re-pointed at @executable_path/../Frameworks. Today that
# is just libglfw; the loop keeps working if the link line grows.
echo "[make_dmg] bundling dylibs"
install_name_tool -add_rpath @executable_path/../Frameworks "$MACOS_DIR/terminal" 2>/dev/null || true

worklist=("$MACOS_DIR/terminal")
declare -a copied=()
while ((${#worklist[@]})); do
  target="${worklist[0]}"
  worklist=("${worklist[@]:1}")
  while read -r dep; do
    case "$dep" in
      /usr/lib/*|/System/*|@*|"") continue ;;
    esac
    base="$(basename "$dep")"
    if [[ ! -f "$FRAMEWORKS/$base" ]]; then
      cp -L "$dep" "$FRAMEWORKS/$base"
      chmod u+w "$FRAMEWORKS/$base"
      install_name_tool -id "@rpath/$base" "$FRAMEWORKS/$base"
      copied+=("$base")
      worklist+=("$FRAMEWORKS/$base")
    fi
    install_name_tool -change "$dep" "@rpath/$base" "$target"
  done < <(otool -L "$target" | tail -n +2 | awk '{print $1}')
done
echo "[make_dmg] bundled: ${copied[*]:-none}"

# --- launcher ---------------------------------------------------------------
# Finder launches with cwd=/ and no shell environment. This wrapper moves to a
# writable dir (so ImGui can persist imgui.ini) and sources ~/.execution-layer.env
# if present, which is how ALPACA_API_KEY_ID / ALPACA_API_SECRET_KEY reach the
# app when it is not started from a terminal.
#
# EL_FEEDHANDLER tells the terminal where the bundled feed lives; without it
# (a dev build) the terminal launches nothing and waits for you to run one.
cat >"$MACOS_DIR/$APP_NAME" <<LAUNCHER
#!/bin/bash
HERE="\$(cd "\$(dirname "\$0")" && pwd)"
SUPPORT="\$HOME/Library/Application Support/$APP_NAME"
mkdir -p "\$SUPPORT"
cd "\$SUPPORT"
[[ -f "\$HOME/.execution-layer.env" ]] && set -a && . "\$HOME/.execution-layer.env" && set +a
FEED="\$HERE/../Resources/feedhandler/feedhandler"
[[ -x "\$FEED" ]] && export EL_FEEDHANDLER="\$FEED"
exec "\$HERE/terminal" "\$@"
LAUNCHER
chmod +x "$MACOS_DIR/$APP_NAME"

# --- Info.plist -------------------------------------------------------------
cat >"$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleExecutable</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>LSMinimumSystemVersion</key><string>$MIN_MACOS</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST

# --- sign (inside-out: nested code first, bundle last) ----------------------
echo "[make_dmg] signing with identity: $CODESIGN_ID"
for lib in "$FRAMEWORKS"/*.dylib; do
  [[ -e "$lib" ]] || continue
  codesign --force --timestamp=none -s "$CODESIGN_ID" "$lib"
done

# PyInstaller ships its own Python runtime plus a tree of .so/.dylib extension
# modules. Each is Mach-O code and has to be signed individually, innermost
# first, or --deep --strict rejects the bundle.
if [[ -d "$RESOURCES/feedhandler" ]]; then
  echo "[make_dmg] signing feedhandler payload"
  find "$RESOURCES/feedhandler" \( -name '*.so' -o -name '*.dylib' \) -type f -print0 \
    | xargs -0 -I{} codesign --force --timestamp=none -s "$CODESIGN_ID" {}
  codesign --force --timestamp=none -s "$CODESIGN_ID" "$RESOURCES/feedhandler/feedhandler"
fi

codesign --force --timestamp=none -s "$CODESIGN_ID" "$MACOS_DIR/terminal"
codesign --force --timestamp=none -s "$CODESIGN_ID" "$APP"
codesign --verify --deep --strict "$APP"

# --- dmg --------------------------------------------------------------------
echo "[make_dmg] building $DMG"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

hdiutil create \
  -volname "$APP_NAME" \
  -srcfolder "$STAGE" \
  -ov -format UDZO \
  "$DMG" >/dev/null

echo "[make_dmg] done -> $DMG"
echo "[make_dmg] $(du -h "$DMG" | cut -f1)"
