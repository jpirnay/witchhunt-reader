#!/usr/bin/env bash
# The firmware's SDK pin may trail the fork's long-lived branch, never leave it (see .gitmodules).
# A pin that lives only on a feature branch goes when that branch does, and old firmware commits
# stop building.
#
# A pull request that changes the pin is reviewed before its SDK commits land: they sit on a
# pushed fork branch until the long-lived branch takes them (fast-forward or merge). With
# --allow-pending such a pin passes with a warning, as long as some branch of the fork holds it.
# A pin no fork branch holds (never pushed, or a deleted branch) still fails. Pushes to master
# run without the flag, so a merged pin must be on the long-lived branch.
#
# Usage: scripts/check_sdk_pin.sh [--allow-pending] [sha]    (default: the pin in HEAD)
# SDK_PIN_URL overrides the fork's URL (tests).
set -euo pipefail

allow_pending=0
if [[ "${1:-}" == "--allow-pending" ]]; then
  allow_pending=1
  shift
fi

cd "$(git rev-parse --show-toplevel)"
pin="${1:-$(git rev-parse HEAD:freeink-sdk)}"
url="${SDK_PIN_URL:-$(git config -f .gitmodules submodule.freeink-sdk.url)}"
branch="$(git config -f .gitmodules submodule.freeink-sdk.branch)"

clone="$(mktemp -d)"
trap 'rm -rf "$clone"' EXIT
# Commits only: reachability needs no trees or blobs. Every branch, for a pending pin.
git clone --quiet --bare --filter=tree:0 "$url" "$clone"

if git -C "$clone" merge-base --is-ancestor "$pin" "$branch" 2>/dev/null; then
  echo "SDK pin ${pin:0:9} is on $branch."
  exit 0
fi

if ((allow_pending)); then
  holders="$(git -C "$clone" branch --contains "$pin" --format='%(refname:short)' 2>/dev/null | paste -sd, - || true)"
  if [[ -n "$holders" ]]; then
    msg="SDK pin ${pin:0:9} is on $holders, not yet on $branch: fast-forward or merge $branch to it before this pull request merges."
    echo "$msg"
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
      echo "::warning title=SDK pin pending::$msg"
      [[ -n "${GITHUB_STEP_SUMMARY:-}" ]] && echo "$msg" >>"$GITHUB_STEP_SUMMARY"
    fi
    exit 0
  fi
fi

echo "SDK pin ${pin:0:9} is not on $branch of $url." >&2
if ((allow_pending)); then
  echo "No branch of the fork holds it: push the SDK commits to a fork branch." >&2
else
  echo "Merge the SDK commits into $branch on the fork, then run this again." >&2
fi
exit 1
