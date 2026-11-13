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

Patching the descriptor bytes invalidates two image-integrity values that
downstream verifiers check (freeink-sdk RecoveryBoot/FirmwareFlasher, the
bootloader): the appended SHA-256 digest (esp_image_header_t.hash_appended at
0x17 — sha256(image[:-32]) vs the final 32 bytes) AND the ESP-image XOR
checksum byte (seed 0xEF over all segment data, stored at the last byte of the
16-byte alignment padding after the final segment). The stamp recomputes both.
Idempotent: a repeated stamp recomputes both from the current content.

Callers: dev-build.yml (stamps dev builds with the derived dev version),
pr-firmware-links.yml (PR firmware, --branch override), and release.yml
(release builds stamped with the plain tag version — the esp_app_desc_t of
an unstamped release shows the prebuilt core's constant, not 2.X.Y).
"""

import argparse
import hashlib
import re
import struct
import sys

MAGIC_OFFSET = 0x20
VERSION_OFFSET = 0x30
VERSION_FIELD_SIZE = 32
MAGIC = 0xABCD5432
MIN_IMAGE_SIZE = VERSION_OFFSET + VERSION_FIELD_SIZE  # 0x50
HASH_APPENDED_OFFSET = 0x17
DEV_MARK = "-dev-"
# The only valid shape of a dev-version string: a numeric release base, the
# -dev- marker, then a printable-ASCII tail (branch + sha, no whitespace).
# git_branch.py warns on stderr, so a CI capture that mixed stderr in (e.g.
# "WARNING [git_branch.py]: ...\n2.16.1-dev-abc") can never match; the guard
# exists so such a blob is refused, never truncated into a valid-looking
# stamp.
DEV_VERSION_RE = re.compile(r"-?[0-9.]+-dev-[\x21-\x7e]+")
# ESP app-image layout: 24-byte image header, then segCount 8-byte segment
# headers, each followed by its data. The XOR checksum byte sits at the last
# byte of the 16-byte alignment padding after the final segment. These mirror
# freeink-sdk RecoveryBoot/FirmwareFlasher.cpp exactly.
IMAGE_HEADER_SIZE = 24
SEG_HEADER_SIZE = 8
CHECKSUM_SEED = 0xEF


def recompute_checksum(data: bytes) -> bytes:
    """Refresh the ESP-image XOR checksum byte after descriptor bytes changed.

    Mirrors the verifier: seed 0xEF, XOR over every byte of every segment's
    data (headers excluded), result stored at padEnd-1 where padEnd is the
    16-byte-aligned offset after the final segment. Fails closed on a
    malformed segment table instead of writing a wrong checksum.
    """
    seg_count = data[1]
    if seg_count == 0:
        fail("image declares 0 segments; cannot recompute the XOR checksum")
    pos = IMAGE_HEADER_SIZE
    xor = CHECKSUM_SEED
    for i in range(seg_count):
        if pos + SEG_HEADER_SIZE > len(data):
            fail(f"segment {i} header overruns EOF while recomputing the checksum")
        (data_len,) = struct.unpack_from("<I", data, pos + 4)
        pos += SEG_HEADER_SIZE
        if pos + data_len > len(data):
            fail(f"segment {i} data overruns EOF while recomputing the checksum")
        for b in data[pos:pos + data_len]:
            xor ^= b
        pos += data_len
    pad_end = (pos + 16) & ~15
    if pad_end > len(data):
        fail(f"checksum padding runs past EOF (padEnd {pad_end} > {len(data)} bytes)")
    out = bytearray(data)
    out[pad_end - 1] = xor & 0xFF
    return bytes(out)


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


def is_dev_version(value: str) -> bool:
    """True when value is exactly one dev-stamp string (<base>-dev-<tail>).

    The WHOLE string must match, so an embedded newline or any other
    contamination makes this False. Exposed for callers (CI capture gates)
    that validate a derived version before stamping with it.
    """
    return DEV_VERSION_RE.fullmatch(value) is not None


def fit_version(value: str) -> bytes:
    """Fit the whole dev-version string into the 31-byte field, NUL-padded.

    Refuses loudly anything that is not a version string: a capture that
    mixed stderr in (newline or WARNING text) or a -dev- string outside the
    stamp shape must fail here, never be truncated into a valid-looking
    stamp. Over-length strings then drop the branch component (the segment
    between -dev- and the final -<sha> tail); if the result still does not
    fit, the base is truncated on the right. The -dev-<sha> tail is never
    cut — build identity is the guarantee.
    """
    if "\n" in value or "\r" in value or "WARNING [" in value:
        fail(f"version string {value!r} is contaminated (embedded newline or "
             "WARNING text); refusing to stamp")
    if DEV_MARK in value and not is_dev_version(value):
        fail(f"version string {value!r} does not match the dev-stamp shape "
             "<base>-dev-<tail>; refusing to fit")
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
    data = recompute_checksum(data)
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


def validate(image: str) -> None:
    """Run the full verifier against the image, exiting non-zero on failure.

    Mirrors freeink-sdk RecoveryBoot/FirmwareFlasher.cpp and the bootloader:
    magic + segment table walk, the padEnd/size rule, the XOR checksum byte,
    and (when hash_appended) the appended SHA-256. CI runs this on every
    stamped dev asset before it may reach the release.
    """
    data = load_image(image)
    seg_count = data[1]
    if seg_count == 0:
        fail(f"{image} declares 0 segments")
    pos = IMAGE_HEADER_SIZE
    xor = CHECKSUM_SEED
    for i in range(seg_count):
        if pos + SEG_HEADER_SIZE > len(data):
            fail(f"{image}: segment {i} header overruns EOF")
        (data_len,) = struct.unpack_from("<I", data, pos + 4)
        pos += SEG_HEADER_SIZE
        if pos + data_len > len(data):
            fail(f"{image}: segment {i} data overruns EOF")
        for b in data[pos:pos + data_len]:
            xor ^= b
        pos += data_len
    pad_end = (pos + 16) & ~15
    hash_appended = data[HASH_APPENDED_OFFSET] == 0x01
    if pad_end + (32 if hash_appended else 0) != len(data):
        fail(f"{image}: size rule violated (padEnd {pad_end} + trailer vs {len(data)} bytes)")
    if (xor & 0xFF) != data[pad_end - 1]:
        fail(f"{image}: XOR checksum mismatch (computed {xor & 0xFF:#04x}, "
             f"stored {data[pad_end - 1]:#04x}) — the image any verifier rejects")
    if hash_appended and hashlib.sha256(data[:-32]).digest() != data[-32:]:
        fail(f"{image}: appended SHA-256 digest mismatch")
    print(f"validated {image}: checksum + digest OK")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--image", help="firmware.bin to patch in place")
    parser.add_argument("--version", help="full dev-version string, e.g. the output of "
                        "scripts/git_branch.py --print-dev-version")
    parser.add_argument("--verify", action="store_true",
                        help="print the image's current version field instead of stamping")
    parser.add_argument("--validate", action="store_true",
                        help="run the full image verification (checksum + digest) and exit non-zero on failure")
    parser.add_argument("--check-version", metavar="VERSION",
                        help="validate VERSION is a dev-stamp string and exit; no image is read")
    args = parser.parse_args()

    if args.check_version is not None:
        if args.version or args.verify or args.validate:
            parser.error("--check-version takes no other flags")
        if not is_dev_version(args.check_version):
            fail(f"not a dev version: {args.check_version!r}")
        return

    if not args.image:
        parser.error("--image is required for --version/--verify/--validate")

    if args.validate:
        if args.version or args.verify:
            parser.error("--validate takes no other flags")
        validate(args.image)
        return

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
