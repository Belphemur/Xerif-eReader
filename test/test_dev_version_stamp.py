"""Dev-version stamping end to end: patch_dev_version.py (fit rule, appended
digest recompute, fail-closed) and the manifest generator's per-entry version
field. The version string's single source is scripts/git_branch.py
--print-dev-version; these tests pin the consumers around it."""

import hashlib
import importlib.util
import struct
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PATCH = REPO / "scripts" / "patch_dev_version.py"
GIT_BRANCH = REPO / "scripts" / "git_branch.py"

MAGIC_OFFSET = 0x20
VERSION_OFFSET = 0x30
MAGIC = 0xABCD5432


def make_image(size=272, magic=True, hash_appended=True):
    """Synthetic app image with a REAL, validator-consistent layout.

    24-byte image header (segCount=1 at byte 1, hash_appended at 0x17), an
    8-byte segment header at 0x18 whose data starts at 0x20 (so the magic at
    0x20 sits inside segment data, like a real app image's esp_app_desc_t),
    then the ESP-image padding rule the verifier applies: a full 16-byte pad
    block whose LAST byte carries the XOR checksum (seed 0xEF over the
    segment data), then — when hash_appended — a 32-byte sha256 over
    image[:-32]. Size must equal padEnd + trailer: 24 + 8 + 192 + 16 + 32 =
    272 by default; other sizes must keep the same arithmetic.
    """
    trailer = 32 if hash_appended else 0
    pad_end = size - trailer
    data_len = pad_end - 16 - (IMAGE_HEADER_SIZE + SEG_HEADER_SIZE)
    assert data_len % 16 == 0, "segment data must end 16-aligned for the fixture"
    data = bytearray(size)
    data[0] = 0xE9  # ESP image magic
    data[1] = 1  # one segment
    data[0x17] = 0x01 if hash_appended else 0x00
    seg_data_start = IMAGE_HEADER_SIZE + SEG_HEADER_SIZE  # 0x20
    struct.pack_into("<I", data, IMAGE_HEADER_SIZE, 0x3C000000)  # DROM load addr
    struct.pack_into("<I", data, IMAGE_HEADER_SIZE + 4, data_len)
    if magic:
        struct.pack_into("<I", data, MAGIC_OFFSET, MAGIC)
    xor = CHECKSUM_SEED
    for b in data[seg_data_start:seg_data_start + data_len]:
        xor ^= b
    data[pad_end - 1] = xor & 0xFF
    if hash_appended:
        data[-32:] = hashlib.sha256(bytes(data[:-32])).digest()
    return bytes(data)


def run_stamp(image, version):
    return subprocess.run([sys.executable, str(PATCH), "--image", str(image),
                           "--version", version], capture_output=True, text=True)


def field_of(image):
    data = Path(image).read_bytes()
    return data[VERSION_OFFSET:VERSION_OFFSET + 32].split(b"\x00", 1)[0].decode()


def digest_ok(image):
    data = Path(image).read_bytes()
    return hashlib.sha256(data[:-32]).digest() == data[-32:]


# Mirrors freeink-sdk RecoveryBoot/FirmwareFlasher.cpp: seed 0xEF, XOR over
# every segment's data bytes; the checksum byte sits at padEnd-1 where padEnd
# is the 16-byte-aligned offset after the final segment.
IMAGE_HEADER_SIZE = 24
SEG_HEADER_SIZE = 8
CHECKSUM_SEED = 0xEF


def checksum_ok(image):
    data = Path(image).read_bytes()
    seg_count = data[1]
    if seg_count == 0:
        return True  # synthetic images with no segment table are skipped
    pos = IMAGE_HEADER_SIZE
    xor = CHECKSUM_SEED
    for _ in range(seg_count):
        if pos + SEG_HEADER_SIZE > len(data):
            return True  # not a real segment table
        (data_len,) = struct.unpack_from("<I", data, pos + 4)
        pos += SEG_HEADER_SIZE
        if pos + data_len > len(data):
            return True
        for b in data[pos:pos + data_len]:
            xor ^= b
        pos += data_len
    pad_end = (pos + 16) & ~15
    if pad_end > len(data):
        return True
    return xor & 0xFF == data[pad_end - 1]


class DevVersionStampTest(unittest.TestCase):
    def test_stamp_writes_version_and_recomputes_digest(self):
        image = Path(self.make_temp_image())
        proc = run_stamp(image, "2.14.0-dev-develop-32c5038a4")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(field_of(image), "2.14.0-dev-develop-32c5038a4")
        # The re-stamped field bytes invalidate the appended digest unless
        # the stamp recomputes it — this assert is the whole point.
        self.assertTrue(digest_ok(image))

    def test_stamp_recomputes_xor_checksum(self):
        # The version field lives inside the first segment's data; a stale
        # XOR checksum byte (seed 0xEF) makes every verifier — the SDK
        # FirmwareFlasher AND the bootloader — reject the image as invalid
        # even when the appended SHA-256 was recomputed. The stamp must
        # refresh the checksum byte with the version bytes' new XOR.
        image = Path(self.make_temp_image())
        run_stamp(image, "2.14.0-dev-develop-32c5038a4")
        self.assertTrue(checksum_ok(image))

    def test_stamp_idempotent(self):
        image = Path(self.make_temp_image())
        version = "2.14.0-dev-develop-32c5038a4"
        self.assertEqual(run_stamp(image, version).returncode, 0)
        self.assertEqual(run_stamp(image, version).returncode, 0)
        self.assertEqual(field_of(image), version)
        self.assertTrue(digest_ok(image))
        self.assertTrue(checksum_ok(image))

    def test_missing_magic_fails_closed(self):
        image = Path(self.make_temp_image(magic=False))
        proc = run_stamp(image, "1.0.0-dev-x-abc")
        self.assertNotEqual(proc.returncode, 0)
        self.assertTrue(proc.stderr.strip())
        self.assertTrue(digest_ok(image))  # untouched

    def test_undersized_image_fails_closed(self):
        path = Path("/tmp") / "devver_tiny.bin"
        path.write_bytes(make_image(size=256)[:0x30])  # no room for the version field
        proc = run_stamp(path, "1.0.0-dev-x-abc")
        self.assertNotEqual(proc.returncode, 0)

    def test_no_hash_appended_leaves_tail_alone(self):
        image = Path(self.make_temp_image(hash_appended=False))
        before = image.read_bytes()
        proc = run_stamp(image, "1.0.0-dev-x-abc")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        after = image.read_bytes()
        self.assertEqual(len(after), len(before))
        # Without an appended hash the checksum byte is the file's last byte
        # and legitimately changes with the version bytes. Everything else
        # outside the version field must be byte-identical.
        self.assertEqual(after[:VERSION_OFFSET], before[:VERSION_OFFSET])
        self.assertEqual(after[VERSION_OFFSET + 32:-1], before[VERSION_OFFSET + 32:-1])
        self.assertIn("leaving the trailing bytes alone", proc.stderr)
        self.assertTrue(checksum_ok(image))

    def test_overlong_string_drops_branch_keeps_sha_tail(self):
        image = Path(self.make_temp_image())
        long_version = "2.14.0-dev-feat-a-really-long-branch-name-0123456789abcd"
        self.assertGreater(len(long_version), 31)
        proc = run_stamp(image, long_version)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        # Branch component dropped; the -dev-<sha> tail is intact.
        self.assertEqual(field_of(image), "2.14.0-dev-0123456789abcd")
        self.assertTrue(digest_ok(image))

    def test_fit_rule_ladder(self):
        fit = self.fit_version
        # Fits: unchanged.
        self.assertEqual(fit("2.14.0-dev-develop-32c5038a4"), b"2.14.0-dev-develop-32c5038a4" + b"\x00" * 4)
        # Branch dropped, sha kept.
        self.assertEqual(fit("1.2.3-dev-feat-x-y-0123456789abcd"), b"1.2.3-dev-0123456789abcd" + b"\x00" * 8)
        # Branch dropped first, then the base truncated on the right; tail intact.
        overlong = "b" * 30 + "-dev-" + "c" * 20 + "-0123456789abcd"
        self.assertEqual(fit(overlong), b"b" * 12 + b"-dev-0123456789abcd" + b"\x00")
        # Already-dropped form with an over-long base: base truncated, tail intact.
        self.assertEqual(fit("b" * 40 + "-dev-0123456789abcd"),
                         b"b" * 12 + b"-dev-0123456789abcd" + b"\x00")
        # No -dev- marker: refuse rather than guess.
        with self.assertRaises(SystemExit):
            fit("x" * 40)

    def fit_version(self, value):
        spec = importlib.util.spec_from_file_location("patch_dev_version", PATCH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.fit_version(value)

    def make_temp_image(self, **kwargs):
        import tempfile
        path = Path(tempfile.mkstemp(suffix=".bin")[1])
        path.write_bytes(make_image(**kwargs))
        return path


class GeneratorVersionFieldTest(unittest.TestCase):
    """The manifest generator's per-entry version field."""

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location(
            "generate_dev_manifest", REPO / ".github/workflows/generate_dev_manifest.py")
        cls.generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.generator)

    def head_sha(self):
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()

    def test_dev_entry_version_from_git_branch_cli(self):
        sha = self.head_sha()
        version = self.generator.dev_version_for(f"xerif-dev-{sha}-x4pro.bin")
        expected = subprocess.check_output(
            [sys.executable, str(GIT_BRANCH), "--print-dev-version", sha],
            cwd=REPO, text=True).strip()
        self.assertEqual(version, expected)

    def test_unknown_sha_omits_version(self):
        # A carried asset whose commit is unreachable yields no version —
        # the device falls back to the asset name.
        self.assertIsNone(self.generator.dev_version_for("xerif-dev-" + "f" * 40 + "-x4pro.bin"))

    def test_pr_entries_get_no_version(self):
        self.assertIsNone(self.generator.entry_version("xerif-pr-303-42136b2-x4pro.bin"))


class ValidateModeTest(unittest.TestCase):
    """CI regression gate: --validate rejects a verifier-invalid image before
    the dev-build workflow may publish it (the XOR-checksum stamp regression).
    """

    def setUp(self):
        self.tmp = Path(self.enterContext(__import__("tempfile").TemporaryDirectory()))
        self.good = self.tmp / "good.bin"
        self.good.write_bytes(make_image())

    def test_validate_accepts_stamped_image(self):
        self.assertEqual(run_stamp(self.good, "2.14.0-dev-develop-32c5038a4").returncode, 0)
        proc = subprocess.run([sys.executable, str(PATCH), "--image", str(self.good),
                               "--validate"], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("checksum + digest OK", proc.stdout)

    def test_validate_rejects_stale_checksum(self):
        # Exactly the regression that shipped once: version bytes rewritten,
        # digest recomputed, XOR checksum byte left stale. Simulate it by
        # flipping a segment-data byte and repairing the digest — only the
        # checksum is stale, and validate must still reject.
        self.assertEqual(run_stamp(self.good, "2.14.0-dev-develop-32c5038a4").returncode, 0)
        data = bytearray(self.good.read_bytes())
        data[100] ^= 0xFF  # segment data (between version field and checksum byte)
        data[-32:] = hashlib.sha256(bytes(data[:-32])).digest()
        corrupted = self.tmp / "corrupt.bin"
        corrupted.write_bytes(bytes(data))
        proc = subprocess.run([sys.executable, str(PATCH), "--image", str(corrupted),
                               "--validate"], capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("checksum", proc.stderr)

    def test_validate_rejects_stale_digest(self):
        stamped = self.tmp / "stamped.bin"
        stamped.write_bytes(make_image())
        data = bytearray(stamped.read_bytes())
        data[-1] ^= 0xFF  # corrupt the appended digest
        stamped.write_bytes(bytes(data))
        proc = subprocess.run([sys.executable, str(PATCH), "--image", str(stamped),
                               "--validate"], capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("digest", proc.stderr)

    def test_validate_rejects_bad_size_rule(self):
        # A truncated file that still carries magic + checksum cannot pass.
        data = make_image()
        truncated = self.tmp / "trunc.bin"
        truncated.write_bytes(data[:-8])
        proc = subprocess.run([sys.executable, str(PATCH), "--image", str(truncated),
                               "--validate"], capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)

    def test_validate_takes_no_other_flags(self):
        proc = subprocess.run([sys.executable, str(PATCH), "--image", str(self.good),
                               "--validate", "--version", "x"], capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)


if __name__ == "__main__":
    unittest.main()
