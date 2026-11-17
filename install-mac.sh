#!/usr/bin/env bash
# Vyrn installer for macOS 13 or newer on Apple Silicon. Safe to run again: every step checks first and only changes
# what is missing.
#
#   ./install-mac.sh          everything below
#   ./install-mac.sh --deps   developer tools check, Homebrew packages, libtorch (into ~/Library/Vyrn/libtorch)
#   ./install-mac.sh --app    build Vyrn and install it as ~/Applications/Vyrn.app (+ vyrn-cli in ~/.local/bin)
#
# Environment: PREFIX (default ~/Applications), JOBS (default: one per core, at most one per 2 GB of memory),
# LIBTORCH_VERSION (default 2.6.0), LIBTORCH_DIR (default ~/Library/Vyrn/libtorch), VYRN_CODESIGN_IDENTITY (default
# "-": an ad-hoc signature; an "Apple Development: ..." identity keeps macOS permissions across rebuilds).
# Needs no sudo. Linux: ./install.sh. Windows: install-windows.ps1.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/Applications}"
# Compiling against libtorch takes up to 2 GB of memory per job: one job per core, but at most one per 2 GB.
default_jobs() {
  local cores mem_gb
  cores="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  mem_gb=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 8589934592) / 1073741824 ))
  if [ $((mem_gb / 2)) -lt "$cores" ]; then cores=$((mem_gb / 2)); fi
  if [ "$cores" -lt 1 ]; then cores=1; fi
  echo "$cores"
}
JOBS="${JOBS:-$(default_jobs)}"
BUILD="$HERE/build"
# The libtorch C++ API Vyrn is written against: the same version as Debian's libtorch-dev on the Raspberry Pi.
# Other 2.x versions from https://pytorch.org/get-started/locally/ (LibTorch, macOS, C++) work the same way.
LIBTORCH_VERSION="${LIBTORCH_VERSION:-2.6.0}"
LIBTORCH_URL="https://download.pytorch.org/libtorch/cpu/libtorch-macos-arm64-${LIBTORCH_VERSION}.zip"
LIBTORCH_DIR="${LIBTORCH_DIR:-$HOME/Library/Vyrn/libtorch}"

# nlohmann-json and googletest are found by CMake (it would download them otherwise); numpy and pillow are for the
# Python side. python@3.13: see pick_python.
FORMULAE=(cmake ninja qt qscintilla2 python@3.13 numpy pillow nlohmann-json googletest)

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ok\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m !!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

check_mac() {
  [ "$(uname -s)" = "Darwin" ] || die "this script is for macOS. On Linux use ./install.sh, on Windows install-windows.ps1."
  if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = "1" ]; then
    die "this Terminal runs under Rosetta (as an Intel program). Quit it, untick 'Open using Rosetta' in its Get Info window, and try again."
  fi
  if [ "$(uname -m)" != "arm64" ] && [ ! -f "$LIBTORCH_DIR/share/cmake/Torch/TorchConfig.cmake" ]; then
    die "this Mac has an Intel processor: pytorch.org only makes libtorch for Apple Silicon now. Unpack a libtorch for x86_64 yourself and run again with LIBTORCH_DIR=<that folder>."
  fi
}

# Puts Homebrew on PATH for this script, if it is installed.
find_brew() {
  local b
  if ! command -v brew >/dev/null 2>&1; then
    for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do
      if [ -x "$b" ]; then
        eval "$("$b" shellenv)"
        break
      fi
    done
  fi
  command -v brew >/dev/null 2>&1 || return 1
  BREW_PREFIX="$(brew --prefix)"
}

# The Python that Vyrn runs is Homebrew's python3 (core/src/platform/system_mac.cpp: /opt/homebrew/bin/python3, then
# /usr/local/bin/python3). Apple's /usr/bin/python3 is too old.
pick_python() {
  local p
  PY=""
  for p in /opt/homebrew/bin/python3 /usr/local/bin/python3; do
    if [ -x "$p" ]; then
      "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null || return 1
      PY="$p"
      return 0
    fi
  done
  return 1
}

do_tools() {
  say "Checking Apple's command line developer tools (compiler)"
  if xcrun --find clang >/dev/null 2>&1; then
    ok "developer tools found in $(xcode-select -p)"
  else
    say "Asking macOS to install them: a window opens, click Install (about 10 minutes)"
    xcode-select --install >/dev/null 2>&1 || true
    die "run ./install-mac.sh again once the developer tools are installed"
  fi
}

do_brew() {
  say "Checking Homebrew"
  if ! find_brew; then
    die "Homebrew is missing. Install it from https://brew.sh (paste the command shown there into Terminal; it asks
       for your password), then run ./install-mac.sh again."
  fi
  ok "Homebrew in $BREW_PREFIX"

  say "Checking Homebrew packages: ${FORMULAE[*]}"
  local f missing=()
  for f in "${FORMULAE[@]}"; do
    brew list --formula --versions "$f" >/dev/null 2>&1 || missing+=("$f")
  done
  if [ ${#missing[@]} -eq 0 ]; then
    ok "all packages already installed"
  else
    say "Installing: ${missing[*]} (Qt alone is about 1 GB, so this can take a while)"
    brew install "${missing[@]}"
    ok "packages installed"
  fi

  say "Checking the Python that Vyrn runs"
  if ! pick_python; then
    # python@3.13 only provides python3.13 when another Python is Homebrew's main one: install the main one too.
    say "No python3 in $BREW_PREFIX/bin yet: installing Homebrew's main Python"
    brew install python3
    pick_python || die "Homebrew's python3 is missing or older than 3.10. Try: brew reinstall python3"
  fi
  ok "$PY ($("$PY" -c 'import platform; print(platform.python_version())'))"
  if "$PY" -c 'import numpy, PIL' 2>/dev/null; then
    ok "numpy and Pillow work in it"
  else
    say "Homebrew's numpy/Pillow are for another Python version: installing them for $PY with pip (your user only)"
    "$PY" -m pip install --user --break-system-packages numpy pillow
    "$PY" -c 'import numpy, PIL' || die "numpy or Pillow still doesn't load in $PY"
    ok "numpy and Pillow installed"
  fi
}

do_libtorch() {
  say "Checking libtorch $LIBTORCH_VERSION (the neural network library) in $LIBTORCH_DIR"
  local marker="$LIBTORCH_DIR/.vyrn-version"
  if [ -f "$LIBTORCH_DIR/share/cmake/Torch/TorchConfig.cmake" ] && [ "$(cat "$marker" 2>/dev/null)" = "$LIBTORCH_VERSION" ]; then
    ok "libtorch $LIBTORCH_VERSION is already there"
    return
  fi
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/vyrn-libtorch.XXXXXX")"
  say "Downloading $LIBTORCH_URL (about 65 MB)"
  curl -fL --retry 3 --progress-bar -o "$tmp/libtorch.zip" "$LIBTORCH_URL" \
    || die "download failed. Check the internet connection, or pick another version on https://pytorch.org and run again with LIBTORCH_VERSION=<version>."
  say "Unpacking it"
  unzip -q "$tmp/libtorch.zip" -d "$tmp" || die "the download is damaged; run ./install-mac.sh --deps again"
  [ -f "$tmp/libtorch/share/cmake/Torch/TorchConfig.cmake" ] || die "the download doesn't contain libtorch/share/cmake/Torch"
  mkdir -p "$(dirname "$LIBTORCH_DIR")"
  rm -rf "$LIBTORCH_DIR"
  mv "$tmp/libtorch" "$LIBTORCH_DIR"
  printf '%s\n' "$LIBTORCH_VERSION" > "$marker"
  rm -rf "$tmp"
  ok "libtorch $LIBTORCH_VERSION in $LIBTORCH_DIR"
}

do_deps() {
  do_tools
  do_brew
  do_libtorch
}

# A build/ folder copied from another computer (or made in another folder) stops CMake with a cryptic message.
check_build_dir() {
  local cache="$BUILD/CMakeCache.txt" made_in
  [ -f "$cache" ] || return 0
  made_in="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")"
  if [ "$made_in" != "$HERE" ] && [ "$made_in" != "$(cd "$HERE" && pwd -P)" ]; then
    die "$BUILD was made for $made_in (another computer or folder). Delete it, then run this again: rm -rf \"$BUILD\""
  fi
}

find_macdeployqt() {
  local d
  if command -v macdeployqt >/dev/null 2>&1; then
    command -v macdeployqt
    return 0
  fi
  for d in "$(brew --prefix qt 2>/dev/null || true)" "$(brew --prefix qttools 2>/dev/null || true)" \
           "$(brew --prefix qtbase 2>/dev/null || true)"; do
    if [ -n "$d" ] && [ -x "$d/bin/macdeployqt" ]; then
      printf '%s\n' "$d/bin/macdeployqt"
      return 0
    fi
  done
  return 1
}

do_app() {
  find_brew || die "Homebrew is missing: run ./install-mac.sh --deps first"
  [ -f "$LIBTORCH_DIR/share/cmake/Torch/TorchConfig.cmake" ] || die "libtorch is missing: run ./install-mac.sh --deps first"
  pick_python || die "Homebrew's python3 is missing: run ./install-mac.sh --deps first"
  [ -f "$HERE/CMakeLists.txt" ] || die "no CMakeLists.txt in $HERE"
  check_build_dir

  say "Configuring the build in $BUILD"
  cmake -S "$HERE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt);$BREW_PREFIX" \
    -DTorch_DIR="$LIBTORCH_DIR/share/cmake/Torch" \
    -DPython3_EXECUTABLE="$PY"
  say "Building Vyrn with $JOBS jobs (10-20 minutes the first time)"
  cmake --build "$BUILD" -j"$JOBS"

  local app
  say "Installing to $PREFIX"
  mkdir -p "$PREFIX"
  for app in "$PREFIX"/[Vv]yrn.app; do   # start from a clean bundle: macdeployqt and codesign work on it below
    if [ -d "$app" ]; then rm -rf "$app"; fi
  done
  cmake --install "$BUILD"
  app=""
  for app in "$PREFIX"/[Vv]yrn.app; do :; done
  [ -d "$app" ] || die "the install did not create $PREFIX/Vyrn.app"
  ok "$app"

  say "Copying Qt, QScintilla and libtorch into the app (macdeployqt)"
  local mdq exe extra=()
  mdq="$(find_macdeployqt)" || die "macdeployqt is missing (it comes with Homebrew's qt): brew reinstall qt"
  for exe in vyrn-cli vyrn-mcp; do
    if [ -f "$app/Contents/MacOS/$exe" ]; then
      extra+=("-executable=$app/Contents/MacOS/$exe")
    fi
  done
  "$mdq" "$app" -always-overwrite -libpath="$LIBTORCH_DIR/lib" ${extra[@]+"${extra[@]}"}

  say "Signing the app for this Mac"
  codesign --force --deep --sign "${VYRN_CODESIGN_IDENTITY:--}" "$app"
  codesign --verify --deep "$app" 2>/dev/null && ok "signed" || warn "the signature doesn't verify; Vyrn may still start"

  if "$app/Contents/MacOS/vyrn-cli" --version >/dev/null 2>&1; then
    ok "vyrn-cli starts ($("$app/Contents/MacOS/vyrn-cli" --version 2>&1 | head -n 1))"
  else
    warn "vyrn-cli inside the app doesn't start; run it to see why: \"$app/Contents/MacOS/vyrn-cli\" --version"
  fi

  say "Making vyrn-cli available in Terminal"
  mkdir -p "$HOME/.local/bin"
  ln -sfn "$app/Contents/MacOS/vyrn-cli" "$HOME/.local/bin/vyrn-cli"
  ok "$HOME/.local/bin/vyrn-cli -> $app/Contents/MacOS/vyrn-cli"
  case ":$PATH:" in
    *":$HOME/.local/bin:"*) ok "~/.local/bin is on your PATH" ;;
    *) warn "~/.local/bin is not on your PATH yet. Run this once, then open a new Terminal window:"
       printf "      echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.zprofile\n" ;;
  esac

  say "Two permissions for real games (simulations need neither)"
  cat <<EOF
      1. Keys: System Settings > Privacy & Security > Accessibility: click +, choose Vyrn in
         $PREFIX, and switch it on.
      2. Screen: System Settings > Privacy & Security > Screen Recording (on macOS 15: "Screen & System Audio
         Recording"): add Vyrn the same way.
      Vyrn's Setup window checks both and opens the right page. After a rebuild macOS may not recognise the new
      Vyrn: remove it from both lists (the - button) and add it again. If you train a real game with vyrn-cli in
      Terminal, Terminal needs the same two permissions.
      Panic keys: Control+Option+Esc stops an AI that is pressing keys, right away.
EOF
  ok "Vyrn is installed. Open it from Launchpad or Spotlight (Cmd+Space, type Vyrn), or: open \"$app\""
}

main() {
  local want_deps=0 want_app=0 a
  if [ $# -eq 0 ]; then want_deps=1; want_app=1; fi
  for a in "$@"; do
    case "$a" in
      --deps) want_deps=1 ;;
      --app) want_app=1 ;;
      --keyboard) warn "macOS has no --keyboard step: give Vyrn the Accessibility permission instead (--app shows how)" ;;
      -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
      *) die "unknown option: $a (try --help)" ;;
    esac
  done
  check_mac
  if [ $want_deps -eq 1 ]; then do_deps; fi
  if [ $want_app -eq 1 ]; then do_app; fi
  ok "done"
}

main "$@"
