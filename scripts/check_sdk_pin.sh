#!/usr/bin/env bash
# The firmware's SDK pin may trail the fork's long-lived branch, never leave it (see .gitmodules).
# A pin that lives only on a feature branch goes when that branch does, and old firmware commits
# stop building.
#
# Usage: scripts/check_sdk_pin.sh [sha]    (default: the pin in HEAD)
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
pin="${1:-$(git rev-parse HEAD:freeink-sdk)}"
url="$(git config -f .gitmodules submodule.freeink-sdk.url)"
branch="$(git config -f .gitmodules submodule.freeink-sdk.branch)"

clone="$(mktemp -d)"
trap 'rm -rf "$clone"' EXIT
# Commits only: reachability needs no trees or blobs.
git clone --quiet --bare --filter=tree:0 --single-branch --branch "$branch" "$url" "$clone"

if git -C "$clone" merge-base --is-ancestor "$pin" "$branch" 2>/dev/null; then
  echo "SDK pin ${pin:0:9} is on $branch."
  exit 0
fi

echo "SDK pin ${pin:0:9} is not on $branch of $url." >&2
echo "Merge the SDK commits into $branch on the fork, then run this again." >&2
exit 1
