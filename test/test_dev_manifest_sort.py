"""generate_dev_manifest.py sort_key orders by real instant, not date string."""

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "generate_dev_manifest",
    Path(__file__).resolve().parents[1] / ".github/workflows/generate_dev_manifest.py",
)
generate_dev_manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generate_dev_manifest)


def named(name):
    return (name, None)


class DevManifestSortTest(unittest.TestCase):
    def test_mixed_timezones_sort_by_instant(self):
        # Release assets carry UTC "Z" dates while dev-build.yml passes the
        # commit date in the author's local offset (%cI, e.g. -05:00).
        # String comparison puts "12:52:30Z" above "07:54:08-05:00" although
        # the offset entry is the newer instant (12:54:08Z).
        newest = named("xerif-dev-newest-x4pro.bin") + ("2026-11-08T07:54:08-05:00",)
        middle = named("xerif-dev-middle-x4pro.bin") + ("2026-11-08T12:52:30Z",)
        oldest = named("xerif-dev-oldest-x4pro.bin") + ("2026-11-08T11:00:00-00:00",)
        ordered = sorted([middle, newest, oldest],
                         key=generate_dev_manifest.sort_key, reverse=True)
        self.assertEqual([newest, middle, oldest], ordered)

    def test_unparseable_and_empty_dates_sort_last(self):
        dated = named("xerif-dev-dated-x4pro.bin") + ("2026-11-08T12:00:00Z",)
        unparseable = named("xerif-dev-bad-x4pro.bin") + ("not-a-date",)
        empty = named("xerif-dev-undated-x4pro.bin") + ("",)
        ordered = sorted([unparseable, empty, dated],
                         key=generate_dev_manifest.sort_key, reverse=True)
        self.assertEqual([dated, unparseable, empty], ordered)

    def test_name_breaks_instant_ties(self):
        # reverse=True makes the name tie-break descending; the contract is
        # only that ties resolve deterministically.
        a = named("xerif-dev-aaa-x4pro.bin") + ("2026-11-08T12:00:00Z",)
        b = named("xerif-dev-bbb-x4pro.bin") + ("2026-11-08T12:00:00Z",)
        ordered = sorted([b, a], key=generate_dev_manifest.sort_key, reverse=True)
        self.assertEqual([b, a], ordered)


if __name__ == "__main__":
    unittest.main()