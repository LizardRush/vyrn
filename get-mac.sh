#!/usr/bin/env bash
# Vyrn for macOS (13+, Apple Silicon), in one line. Downloads the source into a temporary folder, builds it, installs
# ~/Applications/Vyrn.app and deletes the download and the build.
#
#   curl -fsSL https://raw.githubusercontent.com/LizardRush/vyrn/main/get-mac.sh | bash
#
# Options (environment): VYRN_BRANCH (default main), PREFIX (default ~/Applications), JOBS.
# Needs no sudo. Homebrew's packages and libtorch are kept in ~/Library/Vyrn so a second install is quick.
set -euo pipefail

REPO="LizardRush/vyrn"
BRANCH="${VYRN_BRANCH:-main}"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1merror:\033[0m %s\n' "$*" >&2; exit 1; }

main() {
  [ "$(uname -s)" = "Darwin" ] || die "this one is for a Mac. On Linux use get-linux.sh."
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/vyrn.XXXXXX")"
  trap 'cd /; rm -rf "$WORK"' EXIT
  say "Working in $WORK (deleted when this finishes)"

  say "Downloading Vyrn ($REPO, $BRANCH)"
  curl -fsSL "https://codeload.github.com/$REPO/tar.gz/refs/heads/$BRANCH" | tar -xz -C "$WORK" --strip-components=1
  [ -f "$WORK/install-mac.sh" ] || die "that download has no install-mac.sh. Is $REPO/$BRANCH the right place?"

  # Best effort: remember which commit this is, so the app can tell when a newer one exists.
  local sha
  sha="$(curl -fsSL -m 10 -H "Accept: application/vnd.github.sha" "https://api.github.com/repos/$REPO/commits/$BRANCH" 2>/dev/null | tr -d '[:space:]' || true)"
  case "$sha" in
    *[!0-9a-fA-F]*|"") ;;
    *) [ "${#sha}" -eq 40 ] && printf '%s\n' "$sha" > "$WORK/.vyrn-commit" ;;
  esac

  cd "$WORK"
  bash ./install-mac.sh --deps
  bash ./install-mac.sh --app
  say "Done. Vyrn is in ${PREFIX:-$HOME/Applications}/Vyrn.app"
  open "${PREFIX:-$HOME/Applications}/Vyrn.app" || true
}

main "$@"
