#!/bin/bash
# The app's build container and the libraries its runtime ships, for the app tag in runtime.json:
#   build/runtime.sh out    -> the pocketfoundry-build image, out/runtime-libs.txt
set -euo pipefail
out=$1
mkdir -p "$out"
tag=$(jq -r .app runtime.json)
repo=JulioFerrero/pocket-foundry
work=$(mktemp -d)
git clone -q --depth 1 --branch "$tag" "https://github.com/$repo" "$work/app"
docker build -q --platform linux/arm64 -t pocketfoundry-build -f "$work/app/runtime/Dockerfile" "$work/app/runtime"
gh release download "$tag" -R "$repo" -p '*.apk' -D "$work"
unzip -q -o "$work"/*.apk assets/runtime.tar.xz -d "$work"
tar -tJf "$work/assets/runtime.tar.xz" | sed -n 's|^usr/lib/\([^/]*\.so[^/]*\)$|\1|p' | sort -u > "$out/runtime-libs.txt"
echo "$(wc -l < "$out/runtime-libs.txt") libraries in $tag's runtime"
