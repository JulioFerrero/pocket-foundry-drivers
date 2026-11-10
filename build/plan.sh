#!/bin/bash
# The drivers whose recipe has no release yet, for the workflow's build matrix: build=["turnip", ...]
set -euo pipefail
ids=()
for json in drivers/*.json; do
    id=$(basename "$json" .json)
    gh release view "$id-$(build/recipe.sh "$json")" > /dev/null 2>&1 || ids+=("$id")
done
echo "build=$(printf '%s\n' "${ids[@]+"${ids[@]}"}" | jq -Rsc 'split("\n") | map(select(. != ""))')"
