#!/usr/bin/env python3
"""Stamp a dev firmware image's esp_app_desc_t version with the build identity.

The prebuilt Arduino core bakes a constant version string into
libesp_app_format.a, so every dev build otherwise reports the same version in
the slot chooser (design docs/design/2026-11-07-dev-ota-slots.md §12). This
script rewrites the 32-byte version field of the image's esp_app_desc_t in
place:

    offset 0x20: magic 0xABCD5432 (little-endian bytes 32 54 CD AB)
    offset 0x30: char version[32], NUL-padded

The stamped value arrives whole via --version (printed by
scripts/git_branch.py --print-dev-version, the single derivation site). The
only fit rule lives here: if the string exceeds the 31 usable bytes, the
branch component is dropped (<base>-dev-<sha>); if it still does not fit, the
base is truncated on the right. The -dev-<sha> tail is never cut so builds
stay distinguishable.

Patching the descriptor bytes invalidates the image's appended SHA-256 digest
(esp_image_header_t.hash_appended at 0x17): the bootloader/install path
verifies sha256(image[:-32]) against the final 32 bytes, so the stamp
recomputes and rewrites that digest whenever the header flag says one is
appended. Idempotent: a repeated stamp recomputes the digest from the current
content.

Dev-channel builds only — release flows never call this and keep their
signed-release descriptors untouched.
"""

import argparse
import hashlib
import struct
import sys

MAGIC_OFFSET = 0x20
VERSION_OFFSET = 0x30
VERSION_FIELD_SIZE = 32
MAGIC = 0xABCD5432
MIN_IMAGE_SIZE = VERSION_OFFSET + VERSION_FIELD_SIZE  # 0x50
HASH_APPENDED_OFFSET = 0x17
DEV_MARK = "-dev-"


def fail(message: str) -> "NoReturn":  # noqa: F821
    print(f"patch_dev_version: {message}", file=sys.stderr)
    sys.exit(1)


def load_image(image: str) -> bytes:
    """Read the whole image, exiting non-zero on a missing magic or bad size."""
    try:
        with open(image, "rb") as f:
            data = f.read()
    except OSError as e:
        fail(f"cannot read {image}: {e}")
    if len(data) < MIN_IMAGE_SIZE:
        fail(f"{image} is {len(data)} bytes, smaller than the {MIN_IMAGE_SIZE:#x}-byte app descriptor")
    (magic,) = struct.unpack_from("<I", data, MAGIC_OFFSET)
    if magic != MAGIC:
        fail(f"{image} lacks the esp_app_desc_t magic {MAGIC:#010x} at offset {MAGIC_OFFSET:#x} "
             f"(found {magic:#010x}) — not an ESP-IDF app image?")
    return data


def read_field(image: str) -> str:
    """Print the current version field, or exit non-zero on a bad image."""
    data = load_image(image)
    return data[VERSION_OFFSET:MIN_IMAGE_SIZE].split(b"\x00", 1)[0].decode("ascii", "replace")


def fit_version(value: str) -> bytes:
    """Fit the whole dev-version string into the 31-byte field, NUL-padded.

    Over-length strings first drop the branch component (the segment between
    -dev- and the final -<sha> tail); if the result still does not fit, the
    base is truncated on the right. The -dev-<sha> tail is never cut — build
    identity is the guarantee.
    """
    if len(value) <= VERSION_FIELD_SIZE - 1:
        return value.encode("ascii")[:VERSION_FIELD_SIZE - 1].ljust(VERSION_FIELD_SIZE, b"\x00")

    head, mark, rest = value.partition(DEV_MARK)
    if not mark:
        fail(f"version string {value!r} has no -dev-<sha> tail to preserve; refusing to truncate")
    # The tail is -dev-<sha>: the sha is the segment after the final dash (a
    # string already in dropped form has no further dash, so rest itself is
    # the sha and the branch drop is a no-op).
    parts = rest.rsplit("-", 1)
    sha = parts[1] if len(parts) == 2 else rest
    tail = DEV_MARK + sha
    candidate = head + tail
    if len(candidate) <= VERSION_FIELD_SIZE - 1:
        return candidate.encode("ascii")[:VERSION_FIELD_SIZE - 1].ljust(VERSION_FIELD_SIZE, b"\x00")
    budget = VERSION_FIELD_SIZE - 1 - len(tail)
    if budget < 0:
        fail(f"version string {value!r} has a -dev-<sha> tail longer than the field")
    return (head[:budget] + tail).encode("ascii")[:VERSION_FIELD_SIZE - 1].ljust(VERSION_FIELD_SIZE, b"\x00")


def stamp(image: str, version: str) -> None:
    field = fit_version(version)
    data = load_image(image)
    data = data[:VERSION_OFFSET] + field + data[MIN_IMAGE_SIZE:]
    if data[HASH_APPENDED_OFFSET] == 0x01:
        data = data[:-32] + hashlib.sha256(data[:-32]).digest()
    elif data[-32:] != hashlib.sha256(data[:-32]).digest():
        print(f"patch_dev_version: {image} has no appended hash "
              f"(header byte {HASH_APPENDED_OFFSET:#x} = {data[HASH_APPENDED_OFFSET]:#04x}); "
              "leaving the trailing bytes alone", file=sys.stderr)
    try:
        with open(image, "r+b") as f:
            f.write(data)
    except OSError as e:
        fail(f"cannot write {image}: {e}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image", required=True, help="firmware.bin to patch in place")
    parser.add_argument("--version", help="full dev-version string, e.g. the output of "
                        "scripts/git_branch.py --print-dev-version")
    parser.add_argument("--verify", action="store_true",
                        help="print the image's current version field instead of stamping")
    args = parser.parse_args()

    if args.verify:
        if args.version:
            parser.error("--verify takes no --version")
        print(read_field(args.image))
        return

    if not args.version:
        parser.error("stamping requires --version")
    stamp(args.image, args.version)
    print(f"stamped {args.image}: {read_field(args.image)}")


if __name__ == "__main__":
    main()
