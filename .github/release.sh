#!/usr/bin/env bash
# release.sh: version, notes and prerelease cleanup for .github/workflows/release.yml (also `just release-notes`).
#   .github/release.sh plan [ci|stable|prerelease] [notes.md]   key=value lines (also to $GITHUB_OUTPUT), notes to the file
#   .github/release.sh id <branch>                              the branch's prerelease id
#   .github/release.sh prune <branch>                           delete that branch's prereleases (and tags)
#   .github/release.sh prune-merged                             delete prereleases of branches merged into HEAD or gone
# Version: the latest vX.Y.Z of $RELEASES_REPO with the patch + 1, or EXPLORER_LITE_VERSION of src/update.h when higher.
# Prerelease: vX.Y.Z-<id>.N; id = the branch with every run outside [0-9A-Za-z-] turned into "-"; N = 1 + the highest
# N of that branch (= 1 + how many it has, unless one was deleted).
# Notes: commit subjects since the previous release: its source tag vX.Y.Z in this repo (pushed by the workflow; the
# app shows the release body as is, so it is not written there), else its "Version X.Y.Z" commit, else the last 20.
# No merges, no "Version x". A prerelease lists its branch's commits (origin/main..HEAD).
# Needs the full history (fetch-depth: 0); prune needs GH_TOKEN with write access to $RELEASES_REPO. DRY_RUN=1: prune
# only prints.
set -euo pipefail
cd "$(dirname "$0")/.."
RELEASES_REPO=${RELEASES_REPO:-justdiegocom/explorer-lite-releases}
RELEASES_GIT=${RELEASES_GIT:-https://github.com/$RELEASES_REPO.git} # another git URL or path: local dry runs
die() { echo "release.sh: $*" >&2; exit 1; }
semsort() { sort -t. -k1,1n -k2,2n -k3,3n; }
remote_tags() { git ls-remote --tags --refs "$RELEASES_GIT" | sed 's#.*refs/tags/##'; }
# an all-digit id would be a numeric semver identifier (no leading zeros allowed): prefix it
branch_id() { printf '%s' "$1" | sed -E 's/[^0-9A-Za-z-]+/-/g; s/^-+//; s/-+$//; s/^([0-9]+)$/b\1/'; }
subjects() { git log --no-merges --format=%s "$@" | { grep -vE '^(Version [0-9]+(\.[0-9]+)*|Merge .*)$' || true; } | sed 's/^/- /'; }

plan() {
    local mode=${1:-ci} notes=${2:-} branch tags last floor next version tag base="" publish=false reason body done_tags
    branch=${GITHUB_REF_NAME:-$(git rev-parse --abbrev-ref HEAD)}
    tags=$(remote_tags)
    last=$(printf '%s\n' "$tags" | { grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' || true; } | sed 's/^v//' | semsort | tail -n 1)
    floor=$(sed -nE 's/^#define EXPLORER_LITE_VERSION "([0-9]+\.[0-9]+\.[0-9]+)".*/\1/p' src/update.h)
    [ -n "$floor" ] || die "no X.Y.Z EXPLORER_LITE_VERSION in src/update.h"
    next=$floor
    if [ -n "$last" ]; then
        IFS=. read -r ma mi pa <<< "$last"
        next="$ma.$mi.$((pa + 1))"
    fi
    version=$(printf '%s\n' "$next" "$floor" | semsort | tail -n 1)
    if [ -n "$last" ]; then
        base=$(git rev-parse -q --verify "refs/tags/v$last^{commit}" || true)
        [ -n "$base" ] || base=$(git log -n 1 --format=%H --grep="^Version ${last//./\\.}\$" HEAD)
    fi
    case $mode in
        ci|stable)
            tag=v$version
            if [ -n "$base" ]; then body=$(subjects "$base..HEAD"); else body=$(subjects -n 20 HEAD); fi
            reason="CI only: v$version is what main would publish"
            if [ "$mode" = stable ]; then
                done_tags=$(git tag --points-at HEAD | { grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' || true; })
                if git log -1 --format=%B HEAD | grep -qiF '[skip release]'; then
                    reason="[skip release] in the head commit"
                elif [ -n "$done_tags" ]; then
                    reason="HEAD is already released as ${done_tags//$'\n'/ }"
                else
                    publish=true
                    reason="release $tag"
                fi
            fi
            ;;
        prerelease)
            [ "$branch" != main ] || die "main publishes releases; a prerelease is for a branch"
            local id n
            id=$(branch_id "$branch")
            [ -n "$id" ] || die "no usable id in the branch name '$branch'"
            n=$(printf '%s\n' "$tags" | sed -nE "s/^v[0-9]+\.[0-9]+\.[0-9]+-$id\.([0-9]+)$/\1/p" | sort -n | tail -n 1)
            version="$version-$id.$(( ${n:-0} + 1 ))"
            tag=v$version
            if git rev-parse -q --verify origin/main >/dev/null; then body=$(subjects origin/main..HEAD); else body=$(subjects -n 20 HEAD); fi
            publish=true
            reason="prerelease $tag of $branch"
            ;;
        *) die "unknown mode '$mode' (ci, stable or prerelease)" ;;
    esac
    [ -n "$body" ] || body="- Maintenance release."
    local out
    out=$(printf '%s\n' "version=$version" "tag=$tag" "last=${last:+v$last}" "base=$base" "publish=$publish" "reason=$reason")
    echo "$out"
    [ -z "${GITHUB_OUTPUT:-}" ] || echo "$out" >> "$GITHUB_OUTPUT"
    if [ -n "$notes" ]; then printf '%s\n' "$body" > "$notes"; else printf '\n%s\n' "$body"; fi
}

prereleases() {
    gh release list --repo "$RELEASES_REPO" --limit 500 --json tagName,isPrerelease --jq '.[] | select(.isPrerelease) | .tagName'
}
delete_tags() {
    local t
    for t in "$@"; do
        if [ "${DRY_RUN:-}" = 1 ]; then echo "would delete $t"; continue; fi
        if gh release delete "$t" --repo "$RELEASES_REPO" --cleanup-tag --yes; then echo "deleted $t"; else echo "::notice::$t was already gone"; fi
    done
}

prune() {
    local id t del=()
    id=$(branch_id "${1:?branch}")
    [ -n "$id" ] || exit 0
    for t in $(prereleases); do
        [[ $t =~ ^v[0-9]+\.[0-9]+\.[0-9]+-$id\.[0-9]+$ ]] && del+=("$t")
    done
    [ ${#del[@]} -gt 0 ] || { echo "no prereleases of $1"; return 0; }
    delete_tags "${del[@]}"
}

# prereleases whose branch is merged into HEAD (main) or no longer exists, by the origin/* refs as fetched
# (actions/checkout with fetch-depth: 0 brings every branch)
prune_merged() {
    local b t live="" del=()
    for b in $(git for-each-ref --format='%(refname:strip=3)' refs/remotes/origin); do
        [ "$b" != HEAD ] || continue
        git merge-base --is-ancestor "refs/remotes/origin/$b" HEAD && continue
        live+="$(branch_id "$b")"$'\n'
    done
    for t in $(prereleases); do
        [[ $t =~ ^v[0-9]+\.[0-9]+\.[0-9]+-([0-9A-Za-z-]+)\.[0-9]+$ ]] || continue
        printf '%s' "$live" | grep -qxF "${BASH_REMATCH[1]}" || del+=("$t")
    done
    [ ${#del[@]} -gt 0 ] || { echo "no prereleases of merged or deleted branches"; return 0; }
    delete_tags "${del[@]}"
}

cmd=${1:-}
shift || true
case $cmd in
    plan) plan "$@" ;;
    id) branch_id "${1:?branch}"; echo ;;
    prune) prune "$@" ;;
    prune-merged) prune_merged ;;
    *) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
