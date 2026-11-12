#!/bin/bash
# What a driver's build depends on, hashed: its kind, source and build steps, the patches they name, the build script
# (build/gles.sh and the shim it builds for a gles driver, build/driver.sh for the others) and the app runtime it's
# built against. A new recipe means a new build, released as <id>-<recipe>.
#   build/recipe.sh drivers/turnip.json
set -euo pipefail
json=$1
{
    jq -S '{kind, source, build}' "$json"
    jq -r '.build.patches // [] | .[]' "$json" | while read -r p; do cat "$p"; done
    if [ "$(jq -r .kind "$json")" = gles ]; then cat build/gles.sh gles/mgx11.c; else cat build/driver.sh; fi
    cat runtime.json
} | sha256sum | cut -c1-12
