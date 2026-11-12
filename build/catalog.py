"""
The catalog the app reads: every driver and fix, with each driver's file from its release. Builds made in this run
are in OUT/<id>.build.json; the others come from their release's build.json.
    python3 build/catalog.py OUT > catalog.json
"""
import glob
import json
import os
import subprocess
import sys
import urllib.request

REPO = "JulioFerrero/pocket-foundry-drivers"
DOWNLOADS = f"https://github.com/{REPO}/releases/download"
# What the app shows and uses; the rest of a driver's JSON is for the build.
SHOWN = ["name", "kind", "testing", "about", "madeBy", "licence", "builtFor", "priority", "for", "gpus", "family",
         "android", "env", "settings", "compression", "tested"]


def released(tag, out, id):
    path = os.path.join(out, f"{id}.build.json")
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    with urllib.request.urlopen(f"{DOWNLOADS}/{tag}/{id}.build.json") as response:
        return json.load(response)


def driver(path, out):
    id = os.path.basename(path)[:-len(".json")]
    with open(path) as f:
        spec = json.load(f)
    recipe = subprocess.run(["build/recipe.sh", path], check=True, capture_output=True, text=True).stdout.strip()
    tag = f"{id}-{recipe}"
    build = released(tag, out, id)
    entry = {"id": id, **{k: spec[k] for k in SHOWN if k in spec}}
    entry["version"] = spec.get("version") or build["mesa"]
    entry["mesa"] = build["mesa"]
    entry["source"] = f"{spec['source']['git']}/tree/{spec['source']['ref']}"
    entry["updated"] = build["built"]
    entry["file"] = {"url": f"{DOWNLOADS}/{tag}/{id}.tar.xz", "sha256": build["sha256"], "size": build["size"]}
    return entry


def fix(path):
    with open(path) as f:
        return {"id": os.path.basename(path)[:-len(".json")], **json.load(f)}


def main():
    out = sys.argv[1]
    with open("runtime.json") as f:
        app = json.load(f)["app"]
    catalog = {
        "format": 1,
        "app": app,
        "drivers": [driver(p, out) for p in sorted(glob.glob("drivers/*.json"))],
        "fixes": [fix(p) for p in sorted(glob.glob("fixes/*.json"))],
    }
    json.dump(catalog, sys.stdout, indent=1, ensure_ascii=False)
    sys.stdout.write("\n")


main()
