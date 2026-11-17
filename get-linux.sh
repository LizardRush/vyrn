#!/usr/bin/env bash
# Vyrn for Linux, in one line. Downloads the source into temporary memory (/dev/shm, when there is plenty of RAM) or a temp folder, builds it, installs
# the app into ~/.local (menu entry + icon + file types) and deletes everything it downloaded.
#
#   curl -fsSL https://raw.githubusercontent.com/LizardRush/vyrn/main/get-linux.sh | bash
#
# Options (environment): VYRN_BRANCH (default main), PREFIX (default ~/.local), JOBS (default 3),
# VYRN_KEYBOARD=1 (also set up key presses for real-game projects: needs sudo, changes udev and labwc settings).
set -euo pipefail

REPO="LizardRush/vyrn"
BRANCH="${VYRN_BRANCH:-main}"
NEED_KB=4000000    # the build needs about 4 GB of room
SPARE_KB=10000000  # ...and compiling needs memory too: use RAM only when 10 GB are free, else the disk's temp folder

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1merror:\033[0m %s\n' "$*" >&2; exit 1; }

main() {
  command -v curl >/dev/null || die "curl is missing (sudo apt install curl)"
  command -v tar >/dev/null || die "tar is missing"

  local base="${TMPDIR:-/tmp}"
  local spare
  spare="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
  if [ -d /dev/shm ] && [ -w /dev/shm ] && [ "${spare:-0}" -ge "$SPARE_KB" ] \
     && [ "$(df -Pk /dev/shm | awk 'NR==2 {print $4}')" -ge "$NEED_KB" ]; then
    base=/dev/shm
  fi
  WORK="$(mktemp -d "$base/vyrn.XXXXXX")"
  trap 'cd /; rm -rf "$WORK"' EXIT
  say "Working in $WORK (deleted when this finishes)"

  say "Downloading Vyrn ($REPO, $BRANCH)"
  curl -fsSL "https://codeload.github.com/$REPO/tar.gz/refs/heads/$BRANCH" | tar -xz -C "$WORK" --strip-components=1
  [ -f "$WORK/install.sh" ] || die "that download has no install.sh. Is $REPO/$BRANCH the right place?"

  # Best effort: remember which commit this is, so the app can tell when a newer one exists.
  local sha
  sha="$(curl -fsSL -m 10 -H "Accept: application/vnd.github.sha" "https://api.github.com/repos/$REPO/commits/$BRANCH" 2>/dev/null | tr -d '[:space:]' || true)"
  case "$sha" in
    *[!0-9a-fA-F]*|"") ;;
    *) [ "${#sha}" -eq 40 ] && printf '%s\n' "$sha" > "$WORK/.vyrn-commit" ;;
  esac

  local steps=(--deps --app)
  [ "${VYRN_KEYBOARD:-}" = "1" ] && steps+=(--keyboard)
  cd "$WORK"
  bash ./install.sh "${steps[@]}"
  say "Done. Open Vyrn from the menu (Programming), or run: ${PREFIX:-$HOME/.local}/bin/vyrn"
}

main "$@"
