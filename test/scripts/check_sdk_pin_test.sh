#!/usr/bin/env bash
# scripts/check_sdk_pin.sh against a local stand-in for the SDK fork: a long-lived branch, a pushed
# feature branch, and a commit no branch holds any more.
set -euo pipefail

root="$(git rev-parse --show-toplevel)"
branch="$(git -C "$root" config -f .gitmodules submodule.freeink-sdk.branch)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fork="$work/fork"
git init -q -b "$branch" "$fork"
git -C "$fork" config uploadpack.allowFilter true
g() { git -C "$fork" -c user.name=test -c user.email=test@example.invalid "$@"; }
g commit -q --allow-empty -m base
base="$(g rev-parse HEAD)"
g commit -q --allow-empty -m tip
tip="$(g rev-parse HEAD)"
g checkout -q -b feature
g commit -q --allow-empty -m feature
feature="$(g rev-parse HEAD)"
g checkout -q -b deleted "$base"
g commit -q --allow-empty -m deleted
deleted="$(g rev-parse HEAD)"
g checkout -q "$branch"
g branch -q -D deleted

fails=0
expect() {
  local want="$1" got=0
  shift
  env -u GITHUB_ACTIONS SDK_PIN_URL="file://$fork" "$root/scripts/check_sdk_pin.sh" "$@" >/dev/null 2>&1 || got=1
  if [[ "$got" != "$want" ]]; then
    echo "FAIL: check_sdk_pin.sh $* exited $got, want $want"
    fails=1
  fi
}

expect 0 "$base"                    # trails the long-lived branch
expect 0 "$tip"                     # its tip
expect 1 "$feature"                 # master: only a feature branch holds it
expect 0 --allow-pending "$feature" # pull request: pending, a pushed branch holds it
expect 0 --allow-pending "$tip"     # pull request: already on the long-lived branch
expect 1 --allow-pending "$deleted" # pull request: no branch holds it
expect 1 --allow-pending "0000000000000000000000000000000000000000"

if ((fails)); then exit 1; fi
echo "check_sdk_pin.sh: all cases pass"
