#!/usr/bin/env python3
"""Regenerate the rolling `dev` prerelease's manifest.json.

Shared manifest generator for the three dev-channel publishers:
- dev-build.yml (develop merge publishes),
- pr-firmware-links.yml (open-PR firmware publish),
- pr-dev-cleanup.yml (closed-PR firmware removal).

The manifest is rebuilt from the release's actual assets on every run: the
newest --dev-keep `xerif-dev-*` entries plus the newest --pr-keep `xerif-pr-*`
entries, deduped by name, newest first, capped at the device's 4096-byte fetch
(DevOtaClient MAX_MANIFEST_BYTES). Design:
docs/design/2026-11-08-dev-ota-pr-firmware.md §4.4.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys

RELEASE_TAG = "dev"
DEV_PREFIX = "xerif-dev-"
PR_PREFIX = "xerif-pr-"
NAME_SUFFIX = "-x4pro.bin"
CAP_BYTES = 4096


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_release(repo):
    """Return the release JSON, or (None, stderr) when the call fails."""
    proc = subprocess.run(["gh", "api", f"repos/{repo}/releases/tags/{RELEASE_TAG}"],
                          capture_output=True)
    if proc.returncode != 0:
        return None, proc.stderr.decode(errors="replace")
    return json.loads(proc.stdout), None


def prior_manifest_shas(path):
    """name -> sha256 map from the previous manifest (best effort)."""
    try:
        with open(path) as f:
            old = json.load(f)
    except (OSError, ValueError):
        return {}
    return {v["name"]: v["sha256"]
            for v in old.get("versions", [])
            if v.get("name") and v.get("sha256")}


def download_sha256(repo, asset_id):
    proc = subprocess.run(
        ["gh", "api", "-H", "Accept: application/octet-stream",
         f"repos/{repo}/releases/assets/{asset_id}"],
        capture_output=True, check=True)
    return hashlib.sha256(proc.stdout).hexdigest()


def sort_key(candidate):
    # Newest first by asset date; undated assets sort last; name breaks ties
    # deterministically.
    name, _, date = candidate
    return (bool(date), date, name)


def main():
    parser = argparse.ArgumentParser(description="Regenerate the dev manifest.json")
    parser.add_argument("--out", required=True, help="manifest.json output path")
    parser.add_argument("--dev-keep", type=int, default=3)
    parser.add_argument("--pr-keep", type=int, default=6)
    parser.add_argument("--local", action="append", default=[],
                        help="directory holding local .bin files available for hashing")
    parser.add_argument("--prior", help="previous manifest.json path (sha256 reuse)")
    parser.add_argument("--new-bin",
                        help="local bin published by this run but not yet on the release")
    parser.add_argument("--new-date", default="", help="date for --new-bin")
    args = parser.parse_args()

    repo = os.environ.get("GITHUB_REPOSITORY")
    if not repo:
        sys.exit("GITHUB_REPOSITORY is not set")

    release, err = fetch_release(repo)
    if release is None:
        if "404" in err or "Not Found" in err:
            if not args.new_bin:
                sys.exit(f"release '{RELEASE_TAG}' not found and no --new-bin given")
            assets = []
        else:
            sys.exit(f"could not read release '{RELEASE_TAG}': {err}")
    else:
        assets = release.get("assets", [])

    prior = prior_manifest_shas(args.prior) if args.prior else {}

    def local_file_for(name):
        if args.new_bin and os.path.basename(args.new_bin) == name:
            return args.new_bin
        for directory in args.local:
            path = os.path.join(directory, name)
            if os.path.isfile(path):
                return path
        return None

    def entry(name, asset_id, date):
        path = local_file_for(name)
        if path:
            sha256 = sha256_of(path)
        elif name in prior:
            sha256 = prior[name]
        elif asset_id is not None:
            sha256 = download_sha256(repo, asset_id)
        else:
            sys.exit(f"no sha256 available for asset {name}")
        return {
            "name": name,
            "url": f"https://github.com/{repo}/releases/download/{RELEASE_TAG}/{name}",
            "sha256": sha256,
            "date": date,
        }

    new_name = os.path.basename(args.new_bin) if args.new_bin else None
    candidates = [
        (a["name"], a.get("id"), a.get("created_at") or a.get("updated_at") or "")
        for a in assets
        if a["name"].startswith((DEV_PREFIX, PR_PREFIX))
        and a["name"].endswith(NAME_SUFFIX)
        # A locally re-published bin supersedes the release's stale copy.
        and a["name"] != new_name
    ]
    if args.new_bin:
        candidates.append((new_name, None, args.new_date))

    dev = sorted((c for c in candidates if c[0].startswith(DEV_PREFIX)),
                 key=sort_key, reverse=True)[:args.dev_keep]
    pr = sorted((c for c in candidates if c[0].startswith(PR_PREFIX)),
                key=sort_key, reverse=True)[:args.pr_keep]
    ordered = sorted(dev + pr, key=sort_key, reverse=True)
    versions = [entry(*candidate) for candidate in ordered]

    text = json.dumps({"versions": versions}, separators=(",", ":")) + "\n"
    size = len(text.encode())
    if size > CAP_BYTES:
        sys.exit(f"dev manifest is {size} bytes, over the {CAP_BYTES}-byte fetch cap")

    tmp_path = args.out + ".tmp"
    with open(tmp_path, "w") as f:
        f.write(text)
    os.replace(tmp_path, args.out)
    print(text, end="")


if __name__ == "__main__":
    main()
