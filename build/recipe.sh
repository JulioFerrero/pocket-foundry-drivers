#!/bin/bash
# What a driver's build depends on, hashed: its kind, source and build steps, the patches they name, the build script
# and the app runtime it's built against. A new recipe means a new build, released as <id>-<recipe>.
#   build/recipe.sh drivers/turnip.json
set -euo pipefail
json=$1
{
    jq -S '{kind, source, build}' "$json"
    jq -r '.build.patches // [] | .[]' "$json" | while read -r p; do cat "$p"; done
    cat build/driver.sh runtime.json
} | sha256sum | cut -c1-12
