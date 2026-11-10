#!/bin/bash
# GitHub's side of the drivers: a release per build, titled so a person can tell them apart, and only the builds the
# catalog points to.
#   build/release.sh create <id> <out dir>    a new build's release, with <id>.tar.xz and <id>.build.json from <out dir>
#   build/release.sh retitle <catalog.json>   the current builds' titles and notes, from what the catalog says of them
#   build/release.sh prune <catalog.json>     the releases no catalog entry points to, with their tags
set -euo pipefail
cd "$(dirname "$0")/.."

# "PanVK-kbase 26.3.1 · Mali-G615": the driver's own version, and what it's for.
title() {
    local id=$1 version=$2 for
    for=$(jq -r '.for // empty' "drivers/$id.json")
    echo "$(jq -r .name "drivers/$id.json") $version${for:+ · $for}"
}

notes() {
    local id=$1 version=$2 mesa=$3 built=$4 sha256=$5 json="drivers/$1.json"
    local on_mesa=""
    [ "$version" = "$mesa" ] || on_mesa=", on Mesa $mesa"
    jq -r '.about // empty' "$json"
    echo
    echo "**For:** $(jq -r '.for // "-"' "$json")"
    echo "**Version:** $version$on_mesa"
    echo "**Made by:** $(jq -r '.madeBy // "-"' "$json")"
    echo "**Source:** $(jq -r '.source.git + " at " + .source.ref' "$json")"
    echo "**Built:** $built, by this repository's CI from \`$json\`"
    echo "**SHA-256:** \`$sha256\`"
    echo
    echo "Pocket Foundry downloads it by itself on a phone whose GPU it fits. The Catalog release lists every driver."
}

# A catalog entry's release: the tag its file is in.
TAG='.file.url | split("/download/")[1] | split("/")[0]'

case $1 in
create)
    id=$2 build="$3/$2.build.json"
    tag="$id-$(build/recipe.sh "drivers/$id.json")"
    IFS=$'\t' read -r version mesa built sha256 < <(jq -r '[.version, .mesa, .built, .sha256] | @tsv' "$build")
    gh release create "$tag" "$3/$id.tar.xz" "$build" --latest=false --title "$(title "$id" "$version")" \
        --notes "$(notes "$id" "$version" "$mesa" "$built" "$sha256")"
    ;;
retitle)
    jq -c '.drivers[]' "$2" | while read -r entry; do
        id=$(jq -r .id <<< "$entry")
        tag=$(jq -r "$TAG" <<< "$entry")
        IFS=$'\t' read -r version mesa built sha256 < <(
            jq -r '[.version, .mesa, .updated, .file.sha256] | @tsv' <<< "$entry")
        gh release edit "$tag" --title "$(title "$id" "$version")" \
            --notes "$(notes "$id" "$version" "$mesa" "$built" "$sha256")" > /dev/null
        echo "$tag: $(title "$id" "$version")"
    done
    ;;
prune)
    keep=$(jq -r ".drivers[] | $TAG" "$2")
    gh release list --limit 200 --json tagName --jq '.[].tagName' | while read -r tag; do
        [ "$tag" = catalog ] || grep -qxF "$tag" <<< "$keep" && continue
        gh release delete "$tag" --cleanup-tag --yes
        echo "deleted $tag: a newer build replaced it"
    done
    ;;
*)
    echo "usage: $0 create <id> <out dir> | retitle <catalog.json> | prune <catalog.json>" >&2
    exit 1 ;;
esac
