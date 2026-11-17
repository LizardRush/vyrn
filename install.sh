#!/usr/bin/env bash
# Vyrn installer. Safe to run again: every step checks first and only changes what is missing.
#
#   ./install.sh              everything below
#   ./install.sh --deps       system packages (needs sudo)
#   ./install.sh --keyboard   let Vyrn press keys for real-game projects + panic key (needs sudo)
#   ./install.sh --app        build Vyrn and install it to ~/.local (menu entry + icon + file types)
#   ./install.sh --desktop    only the file types: double-clicking a .vrlp or .vrl file opens Vyrn (part of --app)
#
# Environment: PREFIX (default ~/.local), JOBS (default 3).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
JOBS="${JOBS:-3}"

DEPS=(
  cmake ninja-build pkg-config
  qt6-base-dev qt6-qpa-plugins qt6-wayland libqscintilla2-qt6-dev
  libtorch-dev libopenblas0-openmp
  nlohmann-json3-dev libgtest-dev
  python3-numpy python3-pil python3-evdev python3-pytest
  libwayland-dev libwayland-bin
  grim slurp
)

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ok\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m !!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

need_sudo() {
  if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
    say "Vyrn needs your password once for system changes (sudo)."
    sudo -v || die "sudo was not granted"
  fi
}

do_deps() {
  say "Checking system packages"
  local missing=()
  for p in "${DEPS[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed"; then
      missing+=("$p")
    fi
  done
  if [ ${#missing[@]} -eq 0 ]; then
    ok "all packages already installed"
  else
    say "Installing: ${missing[*]}"
    say "(about 700 MB; libtorch also pulls in MPI/ROCm libraries)"
    need_sudo
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}"
    ok "packages installed"
  fi
  local blas
  blas="$(update-alternatives --query libblas.so.3-aarch64-linux-gnu 2>/dev/null | awk '/^Value:/ {print $2}')"
  if [[ "$blas" == *openblas-openmp* ]]; then
    ok "fast BLAS active: $blas"
  else
    warn "BLAS is '$blas' (expected openblas-openmp). Fix: sudo update-alternatives --config libblas.so.3-aarch64-linux-gnu"
  fi
}

do_keyboard() {
  say "Setting up keyboard access for real-game projects"
  local rule='KERNEL=="uinput", SUBSYSTEM=="misc", OPTIONS+="static_node=uinput", GROUP="input", MODE="0660"'
  local rule_file=/etc/udev/rules.d/60-vyrn-uinput.rules
  local mod_file=/etc/modules-load.d/vyrn-uinput.conf
  if [ "$(cat "$rule_file" 2>/dev/null)" != "$rule" ] || ! grep -qx uinput "$mod_file" 2>/dev/null \
     || [ "$(stat -c %G /dev/uinput 2>/dev/null)" != "input" ]; then
    need_sudo
    echo "$rule" | sudo tee "$rule_file" >/dev/null
    echo uinput | sudo tee "$mod_file" >/dev/null
    sudo modprobe uinput || true
    sudo udevadm control --reload-rules || true
    sudo udevadm trigger --subsystem-match=misc --sysname-match=uinput --action=add || true
    # Apply right away too, in case udev has not re-run yet.
    sudo chgrp input /dev/uinput && sudo chmod 0660 /dev/uinput
  fi
  if id -nG "$USER" | tr ' ' '\n' | grep -qx input; then
    ok "you are in the 'input' group"
  else
    need_sudo
    sudo usermod -aG input "$USER"
    warn "added you to the 'input' group: log out and back in once"
  fi
  ok "/dev/uinput: $(stat -c '%A %U:%G' /dev/uinput)"
  warn "note: any program you run can now type keys (members of 'input' can use /dev/uinput)"

  say "Installing the panic stop script"
  install -Dm755 "$HERE/scripts/vyrn-panic" "$PREFIX/bin/vyrn-panic"
  ok "$PREFIX/bin/vyrn-panic"

  say "Adding the panic keys (Ctrl+Alt+Esc) to labwc"
  python3 - "$PREFIX/bin/vyrn-panic" <<'PY'
import os, shutil, sys, time
import xml.etree.ElementTree as ET

panic = sys.argv[1]
path = os.path.expanduser("~/.config/labwc/rc.xml")
NS = "http://openbox.org/3.4/rc"
BINDS = ["C-A-Escape", "S-C-A-Escape"]  # Shift variant: the AI may be holding Shift

if not os.path.exists(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    binds = "\n".join(
        f'    <keybind key="{b}"><action name="Execute" command="{panic}"/></keybind>' for b in BINDS)
    with open(path, "w") as f:
        f.write(f'<?xml version="1.0"?>\n<openbox_config xmlns="{NS}">\n  <keyboard>\n{binds}\n'
                f'  </keyboard>\n</openbox_config>\n')
    print(" ok created", path)
    sys.exit(0)

ET.register_namespace("", NS)
parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
tree = ET.parse(path, parser)
root = tree.getroot()
ns = root.tag[1:].split("}")[0] if root.tag.startswith("{") else ""
q = (lambda t: "{%s}%s" % (ns, t)) if ns else (lambda t: t)
kb = root.find(q("keyboard"))
if kb is None:
    kb = ET.SubElement(root, q("keyboard"))
existing = {k.get("key"): k for k in kb.findall(q("keybind"))}
added = []
for b in BINDS:
    if b in existing:
        cmds = [a.get("command", "") for a in existing[b].iter(q("action"))]
        if not any("vyrn-panic" in c for c in cmds):
            print(f" !! {b} is already bound to something else in {path}; left it alone")
        continue
    kbind = ET.SubElement(kb, q("keybind"), {"key": b})
    ET.SubElement(kbind, q("action"), {"name": "Execute", "command": panic})
    added.append(b)
if added:
    backup = path + ".vyrn-backup-" + time.strftime("%Y%m%d-%H%M%S")
    shutil.copy2(path, backup)
    tree.write(path, xml_declaration=True, encoding="UTF-8")
    print(" ok added", ", ".join(added), "to", path, "(backup:", backup + ")")
else:
    print(" ok panic keys already present in", path)
PY
  if [ -n "${LABWC_PID:-}" ]; then
    labwc -r && ok "labwc reloaded"
  elif pgrep -x labwc >/dev/null; then
    pkill -HUP -x labwc && ok "labwc reloaded"
  else
    warn "labwc is not running; the panic keys apply next time you log in"
  fi
}

# Copies a file (mode 0644) when it is missing or different. Returns 1 when it was already there as it is.
put_file() {
  if cmp -s "$1" "$2"; then
    return 1
  fi
  install -Dm644 "$1" "$2"
}

do_desktop() {
  say "Registering Vyrn's files: .vrlp (a Vyrn project) and .vrl (a trained AI)"
  local share="$PREFIX/share" changed=0 kind
  put_file "$HERE/packaging/vyrn-mime.xml" "$share/mime/packages/vyrn.xml" && changed=1
  for kind in project brain; do
    put_file "$HERE/packaging/vyrn.png" "$share/icons/hicolor/256x256/mimetypes/application-x-vyrn-$kind.png" \
      && changed=1
  done
  if ! command -v update-mime-database >/dev/null; then
    warn "update-mime-database is missing (package shared-mime-info): the desktop won't know .vrlp/.vrl files yet"
  elif [ $changed -eq 1 ] || [ ! -f "$share/mime/mime.cache" ]; then
    update-mime-database "$share/mime" && ok "file types registered in $share/mime"
  else
    ok "file types already registered in $share/mime"
  fi
  if [ -d "$share/applications" ] && command -v update-desktop-database >/dev/null; then
    update-desktop-database -q "$share/applications" || true
  fi
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$share/icons/hicolor" || true
}

do_app() {
  say "Building Vyrn"
  [ -f "$HERE/CMakeLists.txt" ] || die "no CMakeLists.txt in $HERE"
  cmake -S "$HERE" -B "$HERE/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$PREFIX" -DVYRN_BUILD_TESTS=OFF
  cmake --build "$HERE/build" -j"$JOBS"
  say "Installing to $PREFIX"
  cmake --install "$HERE/build"
  do_desktop
  ok "Vyrn is in your menu (Programming). Or run: $PREFIX/bin/vyrn"
}

main() {
  local want_deps=0 want_keyboard=0 want_app=0 want_desktop=0
  if [ $# -eq 0 ]; then want_deps=1; want_keyboard=1; want_app=1; fi
  for a in "$@"; do
    case "$a" in
      --deps) want_deps=1 ;;
      --keyboard) want_keyboard=1 ;;
      --app) want_app=1 ;;
      --desktop) want_desktop=1 ;;
      -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
      *) die "unknown option: $a (try --help)" ;;
    esac
  done
  [ $want_deps -eq 1 ] && do_deps
  [ $want_keyboard -eq 1 ] && do_keyboard
  [ $want_app -eq 1 ] && do_app
  [ $want_desktop -eq 1 ] && [ $want_app -eq 0 ] && do_desktop
  ok "done"
}

main "$@"
