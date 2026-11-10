#!/bin/bash
# Builds one driver from its JSON, in Pocket Foundry's build container (runtime/Dockerfile of the app tag in
# runtime.json), so it links against what the app's runtime ships:
#   build/driver.sh /repo/drivers/turnip.json /out /out/runtime-libs.txt
# Writes /out/<id>.tar.xz, the files the app installs, and /out/<id>.build.json.
set -euo pipefail

json=$1
out=$2
runtime_libs=${3:-}
id=$(basename "$json" .json)
spec() { jq -r "$1" "$json"; }
# Where the app keeps its runtime: Mesa looks there for its data files.
PREFIX=/data/data/app.pocketfoundry/files/usr

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
export GIT_AUTHOR_NAME=build GIT_AUTHOR_EMAIL=build@localhost GIT_COMMITTER_NAME=build GIT_COMMITTER_EMAIL=build@localhost
mapfile -t prepare < <(spec '.build.prepare // [] | .[]')
for step in "${prepare[@]}"; do (cd "$src" && sh -euc "$step"); done

mesa=$src/$(spec '.build.mesa // "."')
mapfile -t patches < <(spec '.build.patches // [] | .[]')
for p in "${patches[@]}"; do patch -d "$mesa" -p1 --silent < "/repo/$p"; done

mapfile -t options < <(spec '.build.options | .[]')
cd "$mesa"
meson setup build --prefix=$PREFIX --libdir=lib --buildtype=release -Db_ndebug=true "${options[@]}"
ninja -C build
install=/build/$id-install
rm -rf "$install" && DESTDIR=$install ninja -C build install
lib=$install$PREFIX/lib

# What the app installs: a Vulkan driver's library and its ICD manifest, or an OpenGL or CPU driver's whole Mesa
# (glvnd's vendor libraries and the gallium library they load), each under the name the loader asks for.
pkg=/build/$id-package
rm -rf "$pkg" && mkdir -p "$pkg"
case $(spec .kind) in
    vulkan)
        cp -L "$lib"/libvulkan_*.so "$pkg/"
        cp "$(ls "$install$PREFIX"/share/vulkan/icd.d/*.json | head -1)" "$pkg/icd.json" ;;
    opengl|cpu)
        cp -L "$lib"/libgallium-*.so "$lib/libGLX_mesa.so.0" "$lib/libEGL_mesa.so.0" "$pkg/" ;;
    *)
        echo "$json: kind must be vulkan, opengl or cpu"; exit 1 ;;
esac
strip --strip-unneeded "$pkg"/*.so*
[ -f "$mesa/docs/license.rst" ] && cp "$mesa/docs/license.rst" "$pkg/LICENSE-mesa.rst"
mapfile -t licences < <(spec '.build.licences // [] | .[]')
for f in "${licences[@]}"; do cp "$src/$f" "$pkg/"; done

# Every library it links must be in the package or in the app's runtime.
needed=$(for f in "$pkg"/*.so*; do readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; done | sort -u)
if [ -n "$runtime_libs" ]; then
    missing=$(for n in $needed; do [ -e "$pkg/$n" ] || grep -qx "$n" "$runtime_libs" || echo "$n"; done)
    [ -z "$missing" ] || { echo "Not in the app's runtime: $missing"; exit 1; }
fi

version=$(spec '.version // empty')
[ -n "$version" ] || version=$(cat "$mesa/VERSION")
tar -C "$pkg" -cf - . | xz -9e -T0 > "$out/$id.tar.xz"
jq -n --arg version "$version" --arg mesa "$(cat "$mesa/VERSION")" --arg built "$(date -u +%F)" \
      --arg sha256 "$(sha256sum "$out/$id.tar.xz" | cut -d' ' -f1)" --argjson size "$(stat -c %s "$out/$id.tar.xz")" \
      --arg needed "$needed" \
      '{version: $version, mesa: $mesa, built: $built, sha256: $sha256, size: $size,
        needed: ($needed | split("\n") | map(select(. != "")))}' > "$out/$id.build.json"
cat "$out/$id.build.json"
