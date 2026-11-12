#!/bin/bash
# Builds a gles driver from its JSON, as build/driver.sh does a Mesa one: MobileGlues with CMake, and gles/mgx11.c,
# the EGL library the game's SDL loads to reach it, which shows the frames:
#   build/gles.sh /repo/drivers/mobileglues.json /out /out/runtime-libs.txt
# Writes /out/<id>.tar.xz, the files the app installs, and /out/<id>.build.json.
set -euo pipefail

json=$1
out=$2
runtime_libs=${3:-}
id=$(basename "$json" .json)
spec() { jq -r "$1" "$json"; }

mapfile -t packages < <(spec '.build.packages // [] | .[]')
apt-get update -qq
apt-get install -y -qq --no-install-recommends git "${packages[@]}" > /dev/null

# The tree lives in the container's own filesystem: on a folder shared with a Mac, git sees stale stat data.
src=/build/$id
rm -rf "$src" && mkdir -p "$src" && cd "$src"
git init -q
git remote add origin "$(spec .source.git)"
git fetch -q --depth 1 origin "$(spec .source.ref)"
git checkout -q --detach FETCH_HEAD
mapfile -t prepare < <(spec '.build.prepare // [] | .[]')
for step in "${prepare[@]}"; do (cd "$src" && sh -euc "$step"); done
mapfile -t patches < <(spec '.build.patches // [] | .[]')
for p in "${patches[@]}"; do patch -d "$src" -p1 --silent < "/repo/$p"; done

# Its C++ library inside it: the runtime's libstdc++ is older than this compiler's.
build=/build/$id-build
cmake -S "$src/$(spec .build.cmake)" -B "$build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_SHARED_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build "$build" -j"$(nproc)"

pkg=/build/$id-package
rm -rf "$pkg" && mkdir -p "$pkg"
cp "$build/libmobileglues.so" "$pkg/"
gcc -O2 -shared -fPIC -fvisibility=hidden -Wall -Werror -o "$pkg/libmgx11.so" /repo/gles/mgx11.c \
    -ldl -lX11 -lxcb -lxcb-dri3 -lxcb-present -lpthread
strip --strip-unneeded "$pkg"/*.so
# LICENSE from the top, LICENSE-<its folder> from below.
mapfile -t licences < <(spec '.build.licences // [] | .[]')
for f in "${licences[@]}"; do
    folder=$(dirname "$f")
    name=$(basename "$f")
    [ "$folder" = . ] || name=${name%%.*}-$(basename "$folder")
    cp "$src/$f" "$pkg/$name"
done

# Every library it links must be in the package or in the app's runtime.
needed=$(for f in "$pkg"/*.so*; do readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; done | sort -u)
if [ -n "$runtime_libs" ]; then
    missing=$(for n in $needed; do [ -e "$pkg/$n" ] || grep -qx "$n" "$runtime_libs" || echo "$n"; done)
    [ -z "$missing" ] || { echo "Not in the app's runtime: $missing"; exit 1; }
fi

version=$(spec .version)
tar -C "$pkg" -cf - . | xz -9e -T0 > "$out/$id.tar.xz"
jq -n --arg version "$version" --arg built "$(date -u +%F)" \
      --arg sha256 "$(sha256sum "$out/$id.tar.xz" | cut -d' ' -f1)" --argjson size "$(stat -c %s "$out/$id.tar.xz")" \
      --arg needed "$needed" \
      '{version: $version, mesa: "", built: $built, sha256: $sha256, size: $size,
        needed: ($needed | split("\n") | map(select(. != "")))}' > "$out/$id.build.json"
cat "$out/$id.build.json"
