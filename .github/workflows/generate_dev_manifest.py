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

Dev entries additionally carry the stamped firmware version (the same string
the esp_app_desc_t shows) derived by the single version source
scripts/git_branch.py --print-dev-version <sha>, with the SHA taken from the
asset name — the just-published asset embeds DEV_SHA, carried assets embed
their own. PR entries (`xerif-pr-*`) carry the field too (design
docs/design/2026-11-07-dev-ota-slots.md §14, one manifest schema): the
publish lane passes --pr-new <asset name> --pr-branch <head branch> for the
asset it just published; its sha comes from the asset name's last dash
segment and the branch override replaces the checkout's branch in the
derivation. Carried PR assets have no branch and omit the optional field,
same degradation as unknown dev shas — the device falls back to the asset
name.
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
from datetime import datetime, timezone

RELEASE_TAG = "dev"
DEV_PREFIX = "xerif-dev-"
PR_PREFIX = "xerif-pr-"
NAME_SUFFIX = "-x4pro.bin"
CAP_BYTES = 4096
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def dev_version_for(name, branch=None):
    """Stamped dev version for a dev/PR asset, or None when it cannot be derived.

    Single source of truth: scripts/git_branch.py --print-dev-version. The
    sha is the asset name's last dash-separated segment (dev names embed the
    full sha, PR names `<pr-number>-<short-sha>` the short sha —
    git_branch.py re-shortens either way, so both print the same string).
    `branch` is the PR head-branch override (--pr-branch). A failed lookup
    only drops the optional field — the device falls back to the asset name.
    """
    prefix = DEV_PREFIX if name.startswith(DEV_PREFIX) else PR_PREFIX
    sha = name[len(prefix):-len(NAME_SUFFIX)].rsplit("-", 1)[-1]
    script = os.path.join(PROJECT_DIR, "scripts", "git_branch.py")
    cmd = [sys.executable, script, "--print-dev-version", sha]
    if branch:
        cmd += ["--branch", branch]
    try:
        proc = subprocess.run(cmd, capture_output=True, cwd=PROJECT_DIR)
    except OSError as e:
        print(f"warning: dev version lookup for {name}: {e}", file=sys.stderr)
        return None
    if proc.returncode != 0:
        print(f"warning: dev version lookup for {name}: "
              f"{proc.stderr.decode(errors='replace').strip()}", file=sys.stderr)
        return None
    return proc.stdout.decode().strip()


def entry_version(name, pr_new=None, pr_branch=None, prior_versions=None):
    """Optional `version` field for a manifest entry (§14: one schema).

    Dev assets always derive it. The PR asset published by this run
    (--pr-new) derives it with the --pr-branch override. Carried PR assets
    keep the version the PR lane stamped for the same asset (the name
    embeds PR number + head sha, so the prior-manifest match is exact) —
    the develop publish lane regenerates the manifest without --pr-new,
    and without this retention it would silently downgrade every carried
    PR entry to the raw asset name. A failed dev lookup still drops the
    field (the device falls back to the asset name).
    """
    if name.startswith(DEV_PREFIX):
        return dev_version_for(name)
    if name == pr_new and pr_branch:
        return dev_version_for(name, branch=pr_branch)
    if prior_versions and name in prior_versions:
        return prior_versions[name]
    return None


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
    # Newest first by asset date; undated/unparseable assets sort last; name
    # breaks ties deterministically.
    # Dates mix formats: release asset created_at is UTC ("Z"), while
    # dev-build.yml passes a commit date (%cI) in the author's local offset
    # (e.g. "-05:00"). String comparison misorders mixed formats
    # ("12:52:30Z" > "07:54:08-05:00" as strings although the latter is the
    # newer instant: 12:54:08Z), so compare parsed UTC instants instead.
    name, _, date = candidate
    try:
        instant = datetime.fromisoformat(date.replace("Z", "+00:00"))
        if instant.tzinfo is None:
            instant = instant.replace(tzinfo=timezone.utc)
    except ValueError:
        instant = datetime.min.replace(tzinfo=timezone.utc)
    return (bool(date), instant, name)


def main():
    parser = argparse.ArgumentParser(description="Regenerate the dev manifest.json",
                                     allow_abbrev=False)
    parser.add_argument("--out", required=True, help="manifest.json output path")
    parser.add_argument("--dev-keep", type=int, default=3)
    parser.add_argument("--pr-keep", type=int, default=6)
    parser.add_argument("--local", action="extend", nargs="+", default=[],
                        help="directories holding local .bin files available for hashing")
    parser.add_argument("--prior", help="previous manifest.json path (sha256 reuse)")
    parser.add_argument("--new-bin",
                        help="local bin published by this run but not yet on the release")
    parser.add_argument("--new-date", default="", help="date for --new-bin")
    parser.add_argument("--pr-new",
                        help="asset name of the PR firmware published by this run")
    parser.add_argument("--pr-branch",
                        help="PR head branch override for --pr-new's version derivation")
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
    prior_versions = {}
    if args.prior:
        try:
            with open(args.prior) as f:
                prior_versions = {v.get("name", ""): v.get("version")
                                  for v in json.load(f).get("versions", [])
                                  if v.get("version")}
        except (OSError, ValueError):
            prior_versions = {}

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
        item = {
            "name": name,
            "url": f"https://github.com/{repo}/releases/download/{RELEASE_TAG}/{name}",
            "sha256": sha256,
            "date": date,
        }
        version = entry_version(name, args.pr_new, args.pr_branch, prior_versions)
        if version:
            item["version"] = version
        return item

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
