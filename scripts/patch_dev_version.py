#!/usr/bin/env python3
"""Stamp a dev firmware image's esp_app_desc_t version with the build identity.

The prebuilt Arduino core bakes a constant version string into
libesp_app_format.a, so every dev build otherwise reports the same version in
the slot chooser (design docs/design/2026-11-07-dev-ota-slots.md §12). This
script rewrites the 32-byte version field of the image's esp_app_desc_t in
place:

    offset 0x20: magic 0xABCD5432 (little-endian bytes 32 54 CD AB)
    offset 0x30: char version[32], NUL-padded

The stamped value is `<version>-dev-<sha7>` where the commit SHA is shortened
to 7 hex chars. If even that does not fit, the version component is cut (the
SHA stays intact so builds remain distinguishable); the field always keeps a
NUL terminator.

Dev-channel builds only — release flows never call this and keep their
signed-release descriptors untouched.
"""

import argparse
import struct
import sys

MAGIC_OFFSET = 0x20
VERSION_OFFSET = 0x30
VERSION_FIELD_SIZE = 32
MAGIC = 0xABCD5432
MIN_IMAGE_SIZE = VERSION_OFFSET + VERSION_FIELD_SIZE  # 0x50
SHA_CHARS = 7
SUFFIX = "-dev-"


def fail(message: str) -> "NoReturn":  # noqa: F821
    print(f"patch_dev_version: {message}", file=sys.stderr)
    sys.exit(1)


def read_field(image: str) -> str:
    """Print the current version field, or exit non-zero on a bad image."""
    try:
        with open(image, "rb") as f:
            data = f.read(MIN_IMAGE_SIZE)
    except OSError as e:
        fail(f"cannot read {image}: {e}")
    if len(data) < MIN_IMAGE_SIZE:
        fail(f"{image} is {len(data)} bytes, smaller than the {MIN_IMAGE_SIZE:#x}-byte app descriptor")
    (magic,) = struct.unpack_from("<I", data, MAGIC_OFFSET)
    if magic != MAGIC:
        fail(f"{image} lacks the esp_app_desc_t magic {MAGIC:#010x} at offset {MAGIC_OFFSET:#x} "
             f"(found {magic:#010x}) — not an ESP-IDF app image?")
    return data[VERSION_OFFSET:MIN_IMAGE_SIZE].split(b"\x00", 1)[0].decode("ascii", "replace")


def stamped_version(base: str, sha: str) -> bytes:
    sha = sha.lower()
    if len(sha) > SHA_CHARS:
        sha = sha[:SHA_CHARS]
    budget = VERSION_FIELD_SIZE - 1 - len(SUFFIX) - len(sha)
    if len(base) > budget:
        base = base[:budget]
    value = f"{base}{SUFFIX}{sha}"
    return value.encode("ascii")[:VERSION_FIELD_SIZE - 1].ljust(VERSION_FIELD_SIZE, b"\x00")


def stamp(image: str, base: str, sha: str) -> None:
    field = stamped_version(base, sha)
    try:
        with open(image, "r+b") as f:
            data = f.read(MIN_IMAGE_SIZE)
            if len(data) < MIN_IMAGE_SIZE:
                fail(f"{image} is {len(data)} bytes, smaller than the {MIN_IMAGE_SIZE:#x}-byte app descriptor")
            (magic,) = struct.unpack_from("<I", data, MAGIC_OFFSET)
            if magic != MAGIC:
                fail(f"{image} lacks the esp_app_desc_t magic {MAGIC:#010x} at offset {MAGIC_OFFSET:#x} "
                     f"(found {magic:#010x}) — not an ESP-IDF app image?")
            f.seek(VERSION_OFFSET)
            f.write(field)
    except OSError as e:
        fail(f"cannot write {image}: {e}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image", required=True, help="firmware.bin to patch in place")
    parser.add_argument("--version", help="base version, e.g. the platformio.ini `version =` value")
    parser.add_argument("--sha", help="commit SHA (full or short); shortened to 7 hex chars")
    parser.add_argument("--verify", action="store_true",
                        help="print the image's current version field instead of stamping")
    args = parser.parse_args()

    if args.verify:
        if args.version or args.sha:
            parser.error("--verify takes no --version/--sha")
        print(read_field(args.image))
        return

    if not args.version or not args.sha:
        parser.error("stamping requires --version and --sha")
    stamp(args.image, args.version, args.sha)
    print(f"stamped {args.image}: {read_field(args.image)}")


if __name__ == "__main__":
    main()
