"""Dev-version stamping end to end: patch_dev_version.py (fit rule, appended
digest recompute, fail-closed) and the manifest generator's per-entry version
field. The version string's single source is scripts/git_branch.py
--print-dev-version; these tests pin the consumers around it."""

import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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

    def test_long_pr_branch_via_cli_drops_branch_keeps_sha_tail(self):
        # §14 fit rule end to end: a long PR branch name (from --branch)
        # drops out of the stamped descriptor first; the -dev-<sha> tail is
        # never cut.
        short = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=REPO, text=True).strip()
        branch = "pr" + "-x" * 40
        version = subprocess.check_output(
            [sys.executable, str(GIT_BRANCH), "--print-dev-version", short,
             "--branch", branch], cwd=REPO, text=True).strip()
        self.assertIn(branch, version)
        image = Path(self.make_temp_image())
        self.assertEqual(run_stamp(image, version).returncode, 0, version)
        base = version.split("-dev-")[0]
        self.assertEqual(field_of(image), f"{base}-dev-{short}")
        self.assertTrue(digest_ok(image))

    def test_fit_rule_ladder(self):
        fit = self.fit_version
        # Fits: unchanged.
        self.assertEqual(fit("2.14.0-dev-develop-32c5038a4"), b"2.14.0-dev-develop-32c5038a4" + b"\x00" * 4)
        # Branch dropped, sha kept.
        self.assertEqual(fit("1.2.3-dev-feat-x-y-0123456789abcd"), b"1.2.3-dev-0123456789abcd" + b"\x00" * 8)
        # Branch dropped first, then the base truncated on the right; tail intact.
        overlong = "9" * 30 + "-dev-" + "c" * 20 + "-0123456789abcd"
        self.assertEqual(fit(overlong), b"9" * 12 + b"-dev-0123456789abcd" + b"\x00")
        # Already-dropped form with an over-long base: base truncated, tail intact.
        self.assertEqual(fit("9" * 40 + "-dev-0123456789abcd"),
                         b"9" * 12 + b"-dev-0123456789abcd" + b"\x00")
        # No -dev- marker: refuse rather than guess.
        with self.assertRaises(SystemExit):
            fit("x" * 40)

    def test_fit_refuses_mixed_stderr_capture(self):
        # The PR lane once captured git_branch.py with 2>&1; a derivation
        # warning glued the WARNING line onto the stamp and fit_version then
        # truncated the head into "WARNING [git_bran-dev-<sha>". It must
        # fail instead, leaving no partial stamp.
        contaminated = ('WARNING [git_branch.py]: git command failed (exit 128): '
                        "fatal: bad revision; SHA suffix will be \"unknown\"\n"
                        "2.16.1-dev-pr-42-0123456789abcd")
        with self.assertRaises(SystemExit):
            self.fit_version(contaminated)

    def test_fit_refuses_embedded_newline(self):
        with self.assertRaises(SystemExit):
            self.fit_version("2.16.1-dev-pr-42-0123456789abcd\nextra")

    def test_fit_refuses_non_stamp_dev_shape(self):
        # Any -dev- string whose head is not the numeric release base is
        # contamination, even when it fits and carries no newline.
        with self.assertRaises(SystemExit):
            self.fit_version("WARNING [git_branch.py]: oops-dev-0123456789abcd")

    def test_fit_accepts_two_segment_capture(self):
        # The GOOD capture: base + -dev- + branch + sha (the branch component
        # present), which must stamp unchanged when it fits.
        self.assertEqual(self.fit_version("2.16.1-dev-pr-42-0123456789abcd"),
                         b"2.16.1-dev-pr-42-0123456789abcd" + b"\x00")

    def test_is_dev_version_shape(self):
        spec = importlib.util.spec_from_file_location("patch_dev_version", PATCH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.assertTrue(module.is_dev_version("2.16.1-dev-pr-42-0123456789abcd"))
        self.assertTrue(module.is_dev_version("0.0.0-dev-develop-32c5038a4"))
        self.assertFalse(module.is_dev_version(
            "WARNING [git_branch.py]: x\n2.16.1-dev-pr-42-0123456789abcd"))
        self.assertFalse(module.is_dev_version("2.16.1-dev-pr-42-0123456789abcd\n"))
        self.assertFalse(module.is_dev_version("garbage-dev-0123456789abcd"))
        # Plain release versions (no -dev- marker) are not dev versions.
        self.assertFalse(module.is_dev_version("2.16.1"))

    def test_check_version_flag_gates_the_stamp_shape(self):
        good = subprocess.run(
            [sys.executable, str(PATCH), "--check-version",
             "2.16.1-dev-pr-42-0123456789abcd"], capture_output=True, text=True)
        self.assertEqual(good.returncode, 0, good.stderr)
        bad = subprocess.run(
            [sys.executable, str(PATCH), "--check-version",
             "WARNING [git_branch.py]: x\n2.16.1-dev-pr-42-0123456789abcd"],
            capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1, bad.stderr)
        self.assertIn("not a dev version", bad.stderr)

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

    def short_sha(self):
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, text=True).strip()

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

    def test_pr_entry_version_with_branch_override(self):
        # §14: the PR entry published by this run derives its version with
        # the --pr-branch override, exactly like the stamp does.
        sha = self.short_sha()
        name = f"xerif-pr-999-{sha}-x4pro.bin"
        version = self.generator.entry_version(name, pr_new=name, pr_branch="feature/pr-stamp")
        expected = subprocess.check_output(
            [sys.executable, str(GIT_BRANCH), "--print-dev-version", sha,
             "--branch", "feature/pr-stamp"], cwd=REPO, text=True).strip()
        self.assertEqual(version, expected)
        self.assertIn("feature/pr-stamp", version)

    def test_carried_pr_assets_omit_version(self):
        # Only the asset named by --pr-new gets the branch override; carried
        # PR assets (sha from the asset name, no branch) omit the field, as
        # does the new asset when the lane passes no branch.
        self.assertIsNone(self.generator.entry_version("xerif-pr-303-42136b2-x4pro.bin"))
        name = f"xerif-pr-999-{self.short_sha()}-x4pro.bin"
        self.assertIsNone(self.generator.entry_version(name))
        self.assertIsNone(self.generator.entry_version(name, pr_branch="feature/pr-stamp"))
        other = f"xerif-pr-998-{self.short_sha()}-x4pro.bin"
        self.assertIsNone(self.generator.entry_version(
            other, pr_new=name, pr_branch="feature/pr-stamp"))

    def test_carried_pr_version_survives_dev_lane_regeneration(self):
        # The develop publish lane regenerates the manifest WITHOUT
        # --pr-new/--pr-branch; carried PR entries keep the version the PR
        # lane stamped for the same asset (name embeds PR number + head sha)
        # instead of downgrading the picker to the raw asset name.
        name = f"xerif-pr-999-{self.short_sha()}-x4pro.bin"
        stamped = f"2.15.0-dev-feature/pr-999-{self.short_sha()}"
        self.assertEqual(self.generator.entry_version(name, prior_versions={name: stamped}), stamped)

    def test_dev_entries_always_rederive_version(self):
        # Dev builds re-derive from the single source even when the prior
        # manifest carries a field — stale identities must not survive.
        sha = self.head_sha()
        name = f"xerif-dev-{sha}-x4pro.bin"
        expected = subprocess.check_output(
            [sys.executable, str(GIT_BRANCH), "--print-dev-version", sha],
            cwd=REPO, text=True).strip()
        self.assertEqual(self.generator.entry_version(name, prior_versions={name: "stale"}), expected)


class GitBranchCliTest(unittest.TestCase):
    """--print-dev-version CLI arg parsing (--branch override)."""

    def test_branch_without_value_fails(self):
        proc = subprocess.run([sys.executable, str(GIT_BRANCH), "--print-dev-version", "--branch"],
                              cwd=REPO, capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("usage", proc.stderr)

    def test_branch_flag_position_independent(self):
        sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=REPO, text=True).strip()
        before = subprocess.check_output(
            [sys.executable, str(GIT_BRANCH), "--print-dev-version", "--branch", "feat/pos", sha],
            cwd=REPO, text=True).strip()
        after = subprocess.check_output(
            [sys.executable, str(GIT_BRANCH), "--print-dev-version", sha, "--branch", "feat/pos"],
            cwd=REPO, text=True).strip()
        self.assertEqual(before, after)
        self.assertIn("feat/pos", before)


class GitBranchDetachedTest(unittest.TestCase):
    """Detached checkouts must never stamp a "detached" label.

    CI checkouts are detached; the branch comes from the environment override
    or from references pointing at the commit. When neither resolves, the
    derivation degrades to "unknown" exactly like the other git fields.
    """

    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("git_branch_module", GIT_BRANCH)
        cls.gb = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.gb)

    def setUp(self):
        self._dirs = []

    def tearDown(self):
        import shutil
        for path in self._dirs:
            shutil.rmtree(path, ignore_errors=True)

    def _git(self, *args, cwd):
        return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)

    def scratch_repo(self):
        path = tempfile.mkdtemp(prefix="gb-detach-")
        self._dirs.append(path)
        self._git("init", "-q", cwd=path)
        for k, v in (("user.name", "DevOta"), ("user.email", "dev-ota@example.com")):
            self._git("config", k, v, cwd=path)
        with open(os.path.join(path, "f.txt"), "w") as f:
            f.write("x\n")
        self._git("add", "f.txt", cwd=path)
        self._git("commit", "-q", "-m", "c1", cwd=path)
        self._git("branch", "ci-branch", cwd=path)
        return path

    def without_env_branch(self):
        return mock.patch.dict(os.environ, clear=False)

    def test_detached_resolves_branch_pointing_at_commit(self):
        path = self.scratch_repo()
        self._git("checkout", "-q", "--detach", "HEAD", cwd=path)
        with self.without_env_branch():
            os.environ.pop("CROSSPOINT_GIT_BRANCH", None)
            self.assertEqual(self.gb.get_git_branch(path), "ci-branch")

    def test_detached_without_resolvable_branch_fails_closed(self):
        path = self.scratch_repo()
        # Delete the local branches; the bare scratch repo has no remote
        # refs for the resolver to fall back to, so the resolution must
        # fail closed to "unknown" — never "detached".
        self._git("checkout", "-q", "--detach", "HEAD", cwd=path)
        for branch in ("ci-branch", "master"):
            self._git("branch", "-d", branch, cwd=path)
        with self.without_env_branch():
            os.environ.pop("CROSSPOINT_GIT_BRANCH", None)
            self.assertEqual(self.gb.get_git_branch(path), "unknown")

    def test_env_override_wins_over_git(self):
        path = self.scratch_repo()
        self._git("checkout", "-q", "--detach", "HEAD", cwd=path)
        with mock.patch.dict(os.environ, {"CROSSPOINT_GIT_BRANCH": "feat/pr-head"}):
            self.assertEqual(self.gb.get_git_branch(path), "feat/pr-head")

    def test_env_override_never_injects_quote_metacharacters(self):
        # The value lands inside a C string literal: quotes and backslashes
        # are stripped like every other git-derived field.
        with mock.patch.dict(os.environ, {"CROSSPOINT_GIT_BRANCH": 'a"b\\c'}):
            self._dirs.append(tempfile.mkdtemp())
            self.assertEqual(self.gb.get_git_branch(self._dirs[-1]), "abc")


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
