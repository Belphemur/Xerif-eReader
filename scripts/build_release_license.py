#!/usr/bin/env python3
"""Collect source references and original notices for a firmware release."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


PROJECT_DIR = Path(__file__).resolve().parent.parent
LIBRARY_NOTICES = {
    # Our fork pins wolfSSL as a GIT dependency (platformio.ini, Arduino-wolfSSL
    # tag 5.8.4): PlatformIO names the libdeps directory after the package name
    # in its library.json ("wolfssl"), NOT the GitHub repo name, and never
    # writes .piopm for git deps.
    "wolfssl": ("LICENSING", "COPYING"),
    "ArduinoJson": ("LICENSE.txt",),
    "JPEGDEC": ("LICENSE",),
    "NimBLE-Arduino": ("LICENSE", "NOTICE"),
    "PNGdec": ("LICENSE",),
    "QRCode": ("LICENSE.txt",),
    "SdFat": ("LICENSE.md",),
    "WebSockets": ("LICENSE",),
}


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=PROJECT_DIR, text=True
    ).strip()


def section(title: str, body: str) -> str:
    return f"\n{'=' * 72}\n{title}\n{'=' * 72}\n\n{body.rstrip()}\n"


def source_reference(directory: Path) -> str:
    metadata_path = directory / ".piopm"
    if metadata_path.is_file():
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        spec = metadata["spec"]
        if spec.get("uri"):
            return spec["uri"]
        owner = spec["owner"]
        name = metadata["name"]
        version = metadata["version"]
        return (
            f"https://dl.registry.platformio.org/download/{owner}/library/"
            f"{name}/{version}/{name}-{version}.tar.gz"
        )
    # Git dependencies keep their exact URL and revision in platformio.ini.
    # Registry installs carry .piopm; git deps may not (PIO version dependent),
    # so name every git dep in lib_deps explicitly instead of guessing.
    GIT_DEP_REFERENCES = {
        "JPEGDEC": "See the JPEGDEC URL and commit in platformio.ini.",
        "PNGdec": "See the PNGdec URL and commit in platformio.ini.",
        "AnimatedGIF": "See the AnimatedGIF URL and commit in platformio.ini.",
        "NimBLE-Arduino": "See the NimBLE-Arduino URL and commit in platformio.ini.",
        "wolfssl": "See the Arduino-wolfSSL URL and 5.8.4 tag in platformio.ini.",
    }
    if directory.name in GIT_DEP_REFERENCES:
        return GIT_DEP_REFERENCES[directory.name]
    raise ValueError(f"Missing source package metadata: {directory}")


def dependency_dir(dependencies: Path, name: str) -> Path:
    """Resolve a libdeps directory; older PIO names git deps by repo name."""
    directory = dependencies / name
    if directory.is_dir():
        return directory
    if name == "wolfssl":
        alias = dependencies / "Arduino-wolfSSL"
        if alias.is_dir():
            return alias
    return directory


def build_license(environment: str, repository: str, packages_dir: Path) -> str:
    revision = git("rev-parse", "HEAD")
    sdk_revision = git("rev-parse", "HEAD:freeink-sdk")
    if git("-C", "freeink-sdk", "rev-parse", "HEAD") != sdk_revision:
        raise ValueError("The SDK checkout differs from the recorded source revision.")
    source_url = f"https://github.com/{repository}"
    text = (
        "CrossPoint Reader firmware licenses\n\n"
        "Original CrossPoint code uses MIT, except where a file states other terms.\n"
        "Third-party code and assets keep their own licenses.\n"
        "The combined firmware executable uses GPLv3 because it includes wolfSSL.\n\n"
        f"Firmware source revision: {revision}\n"
        f"Notice collection environment: {environment}\n"
        f"Source archive: {source_url}/archive/{revision}.tar.gz\n"
        f"Source and patches: {source_url}/tree/{revision}\n"
        f"SDK source: https://github.com/Free-Ink/freeink-sdk/tree/{sdk_revision}\n"
        f"Build instructions: {source_url}/blob/{revision}/licenses/README.md\n\n"
        "GitHub's source archive omits SDK contents and downloaded libraries.\n"
        "Use the SDK revision and library source packages listed in this file.\n"
        "platformio.ini selects the platform, flags, and dependency patch scripts.\n"
    )
    text += section("CrossPoint MIT license", (PROJECT_DIR / "LICENSE").read_text())
    for path in sorted((PROJECT_DIR / "licenses").iterdir()):
        if path.is_file():
            text += section(f"licenses/{path.name}", path.read_text(encoding="utf-8"))

    dependencies = PROJECT_DIR / ".pio" / "libdeps" / environment
    dependency_sources = []
    for name, filenames in LIBRARY_NOTICES.items():
        directory = dependency_dir(dependencies, name)
        dependency_sources.extend(
            path for path in (directory / "src").rglob("*")
            if path.suffix in {".h", ".c", ".cpp"}
        )
        text += section(f"{name}: source package", source_reference(directory))
        for filename in filenames:
            text += section(
                f"{name}/{filename}",
                (directory / filename).read_text(encoding="utf-8-sig"),
            )
        for path in sorted(directory.rglob("*")):
            if path.is_file() and path.name.upper() in {"LICENSE", "NOTICE", "COPYING"}:
                if path.parent != directory:
                    text += section(f"{name}/{path.relative_to(directory)}", path.read_text())

    framework = packages_dir / "framework-arduinoespressif32"
    text += section("Arduino-ESP32: source package", source_reference(framework))
    idf = packages_dir / "framework-espidf"
    if idf.is_dir():
        text += section("ESP-IDF: source package", source_reference(idf))
    text += section(
        "ESP-IDF source",
        "https://github.com/espressif/esp-idf/tree/v5.5.5\n"
        "Some platform libraries arrive as binaries. Their source obligations remain open.\n"
        "See licenses/README.md for the remaining license questions.",
    )

    sdk = PROJECT_DIR / "freeink-sdk"
    for filename in (
        "LICENSE", "NOTICE", "libs/assets/Icons/lucide/LICENSE",
        "libs/font/FreeInkFont/third_party/freetype/LICENSE.TXT",
        # This fork carries FreeType as a git submodule: FTL.TXT lives under
        # docs/ there, not at the vendored-tree root upstream uses.
        "libs/font/FreeInkFont/third_party/freetype/docs/FTL.TXT",
    ):
        text += section(f"FreeInk SDK/{filename}", (sdk / filename).read_text())
    text += section(
        "FreeType credit",
        "This software is based in part on the work of the FreeType Team.",
    )

    # Preserve notices that vendored libraries keep in source comments.
    paths = [
        *PROJECT_DIR.glob("lib/expat/*.[ch]"),
        # This fork vendors miniz inside the SDK submodule (FreeInkBook
        # third_party), not at lib/miniz/third_party like upstream.
        *(sdk / "libs/book/FreeInkBook/third_party/miniz").rglob("*.[ch]"),
        *PROJECT_DIR.glob("lib/uzlib/src/*.[ch]"),
        sdk / "libs/font/FreeInkFont/third_party/stb/stb_truetype.h",
        sdk / "libs/font/FreeInkFont/third_party/freetype/src/base/fthash.c",
        sdk / "libs/font/FreeInkFont/third_party/freetype/include/freetype/internal/fthash.h",
        *framework.glob("cores/esp32/**/*.[ch]"),
        *framework.glob("cores/esp32/**/*.cpp"),
        *dependency_sources,
    ]
    tinyusb = packages_dir / "framework-arduinoespressif32-libs/esp32s3/include/arduino_tinyusb/tinyusb/src/tusb.h"
    if tinyusb.is_file():
        paths.append(tinyusb)
    for name in (
        "AsyncUDP", "DNSServer", "ESPmDNS", "Ethernet", "FS", "Hash", "Network",
        "NetworkClientSecure", "Preferences", "SPI", "WebServer", "WiFi", "Wire",
    ):
        paths.extend((framework / "libraries" / name / "src").rglob("*.h"))
        paths.extend((framework / "libraries" / name / "src").rglob("*.cpp"))
    notices = set()
    for path in sorted(paths):
        source = path.read_text(encoding="utf-8")
        for match in re.finditer(r"/\*.*?\*/", source, re.DOTALL):
            notice = match.group()
            if re.search(r"copyright|permission is|ALTERNATIVE [AB]", notice, re.IGNORECASE):
                if notice not in notices:
                    if path.is_relative_to(framework):
                        label = Path("Arduino-ESP32") / path.relative_to(framework)
                    elif path.is_relative_to(packages_dir):
                        label = path.relative_to(packages_dir)
                    else:
                        label = path.relative_to(PROJECT_DIR)
                    text += section(str(label), notice)
                    notices.add(notice)
    return text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--packages-dir", type=Path,
        default=Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio")) / "packages",
    )
    parser.add_argument(
        "--repository",
        default=os.environ.get("GITHUB_REPOSITORY", "crosspoint-reader/crosspoint-reader"),
    )
    args = parser.parse_args()
    text = build_license(args.env, args.repository, args.packages_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
