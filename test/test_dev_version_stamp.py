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


def make_image(size=256, magic=True, hash_appended=True):
    """Synthetic app image: magic at 0x20, hash_appended at 0x17, and (when
    hash_appended) a valid appended sha256 over image[:-32]."""
    data = bytearray(size)
    if magic:
        struct.pack_into("<I", data, MAGIC_OFFSET, MAGIC)
    data[0x17] = 0x01 if hash_appended else 0x00
    # The appended digest covers image[:-32] only.
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


class DevVersionStampTest(unittest.TestCase):
    def test_stamp_writes_version_and_recomputes_digest(self):
        image = Path(self.make_temp_image())
        proc = run_stamp(image, "2.14.0-dev-develop-32c5038a4")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(field_of(image), "2.14.0-dev-develop-32c5038a4")
        # The re-stamped field bytes invalidate the appended digest unless
        # the stamp recomputes it — this assert is the whole point.
        self.assertTrue(digest_ok(image))

    def test_stamp_idempotent(self):
        image = Path(self.make_temp_image())
        version = "2.14.0-dev-develop-32c5038a4"
        self.assertEqual(run_stamp(image, version).returncode, 0)
        self.assertEqual(run_stamp(image, version).returncode, 0)
        self.assertEqual(field_of(image), version)
        self.assertTrue(digest_ok(image))

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
        before = image.read_bytes()[-32:]
        proc = run_stamp(image, "1.0.0-dev-x-abc")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(image.read_bytes()[-32:], before)
        self.assertIn("leaving the trailing bytes alone", proc.stderr)

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


if __name__ == "__main__":
    unittest.main()
