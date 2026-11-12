#!/bin/sh
# Publishes a local branch to the public GitHub repository Y2Kill/3D_am.
# The public copy drops data/inventory/*.zip (the owner's collection listing)
# from every commit and replaces author/committer e-mail with the GitHub
# noreply address. The filter is deterministic, so earlier public hashes stay
# stable and each publish is a fast-forward. Local history is not changed.
# Usage: tools/publish_public.sh [branch] [--push]   (run by the manager only)
set -eu
ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
BRANCH="${1:-main}"
NOREPLY="27768668+Y2Kill@users.noreply.github.com"
rm -rf "$ROOT"/.cache/publish/work* 2>/dev/null || true
WORK="$ROOT/.cache/publish/work-$(date +%s)"
mkdir -p "$ROOT/.cache/publish"
git clone -q --no-local --single-branch --branch "$BRANCH" "$ROOT" "$WORK"
cd "$WORK"
export FILTER_BRANCH_SQUELCH_WARNING=1
git filter-branch -f \
  --env-filter "GIT_AUTHOR_EMAIL='$NOREPLY'; GIT_COMMITTER_EMAIL='$NOREPLY'; export GIT_AUTHOR_EMAIL GIT_COMMITTER_EMAIL" \
  --index-filter 'git rm -q --cached --ignore-unmatch "data/inventory/*.zip"' \
  -- "$BRANCH" >/dev/null
rm -rf .git/refs/original
git for-each-ref --format="%(refname)" refs/remotes | xargs -r -n1 git update-ref -d
git reflog expire --expire=now --all && git gc -q --prune=now
if git rev-list --objects --all | grep -q '3d_Models_inventory'; then
  echo "ERROR: inventory object still present" >&2; exit 1
fi
git remote add public https://github.com/Y2Kill/3D_am.git
echo "public $BRANCH = $(git rev-parse "$BRANCH")  ($WORK)"
if [ "${2:-}" = "--push" ]; then git push public "$BRANCH:$BRANCH"; fi
